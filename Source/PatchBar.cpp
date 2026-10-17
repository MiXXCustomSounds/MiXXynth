#include "PatchBar.h"

#include "PluginProcessor.h"

namespace mixxynth::ui
{
    namespace
    {
        const juce::Colour kBackground { 0xff14181d };
        const juce::Colour kBorder { 0xff39434f };
        const juce::Colour kText { 0xffe2e8ee };
        const juce::Colour kDim { 0xff8b98a5 };
        const juce::Colour kAccent { 0xffffa53d };

        // How often the bar re-reads the session. Four hertz is enough for a dirty dot and a
        // name, and it is a fraction of what the editor's own thirty-hertz poll costs.
        constexpr int kRefreshHz = 4;

        // Item ids in the browser are offset, so a section heading cannot collide with a preset.
        constexpr int kFirstItemId = 100;
    }

    PatchBar::PatchBar (MiXXynthProcessor& p)
        : proc (p)
    {
        auto configure = [this] (juce::TextButton& b, const juce::String& text,
                                 const juce::String& tooltip)
        {
            b.setButtonText (text);
            b.setTooltip (tooltip);
            b.setColour (juce::TextButton::buttonColourId, kBackground);
            b.setColour (juce::TextButton::textColourOffId, kText);
            addAndMakeVisible (b);
        };

        browser.setTooltip ("The preset bank");
        browser.setColour (juce::ComboBox::backgroundColourId, kBackground);
        browser.setColour (juce::ComboBox::outlineColourId, kBorder);
        browser.setColour (juce::ComboBox::textColourId, kText);
        browser.onChange = [this] { loadSelected(); };
        addAndMakeVisible (browser);

        configure (prevButton, "<", "Previous preset");
        configure (nextButton, ">", "Next preset");
        configure (initButton, "INIT", "Start a patch from the init patch");
        configure (saveButton, "SAVE", "Save this patch as a user preset");
        configure (deleteButton, "DEL", "Delete the loaded user preset");
        configure (undoButton, "UNDO", "Put back the patch from before the last roll or load");

        prevButton.onClick = [this] { step (-1); };
        nextButton.onClick = [this] { step (1); };
        initButton.onClick = [this] { loadInit(); };
        saveButton.onClick = [this] { showSaveDialog(); };
        deleteButton.onClick = [this] { removeSelected(); };
        undoButton.onClick = [this] { undoRoll(); };

        // The roll, and the five locks beside it. A lock reads as the state you are IN, like
        // the Snap switch: lit means "this section will not move".
        rollButton.setButtonText ("ROLL");
        rollButton.setTooltip ("Roll a new patch from the unlocked sections");
        rollButton.setColour (juce::TextButton::buttonColourId, kBackground);
        rollButton.setColour (juce::TextButton::textColourOffId, kAccent);
        rollButton.onClick = [this] { rollNow(); };
        addAndMakeVisible (rollButton);

        for (int s = 0; s < mixxynth::kNumRollSections; ++s)
        {
            auto* b = new juce::TextButton (mixxynth::rollSectionName (s));
            b->setTooltip (juce::String ("Lock ") + mixxynth::rollSectionName (s)
                             + " so a roll leaves it alone");
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, kAccent);
            b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff101418));
            b->setColour (juce::TextButton::buttonColourId, kBackground);
            b->setColour (juce::TextButton::textColourOffId, kDim);
            b->onClick = [this, s, b]
            {
                proc.randomizer.setLocked (s, b->getToggleState());
                setMessage (juce::String (mixxynth::rollSectionName (s))
                              + (b->getToggleState() ? " locked" : " unlocked"));
            };

            lockButtons.add (b);
            addAndMakeVisible (b);
        }

        status.setJustificationType (juce::Justification::centredLeft);
        status.setColour (juce::Label::textColourId, kText);
        addAndMakeVisible (status);

        message.setJustificationType (juce::Justification::centredLeft);
        message.setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (message);

        refresh();
        startTimerHz (kRefreshHz);
    }

    PatchBar::~PatchBar() = default;

    void PatchBar::timerCallback()
    {
        refresh();
    }

    void PatchBar::rebuildList()
    {
        const int keep = browser.getSelectedId();

        browser.clear (juce::dontSendNotification);

        auto& store = proc.presets.store();
        int item = kFirstItemId;

        for (const auto& category : store.categories())
        {
            browser.addSectionHeading (category);

            for (int i = 0; i < store.size(); ++i)
            {
                if (store.categoryOf (i) != category)
                    continue;

                // Factory presets are marked, because they cannot be overwritten and the SAVE
                // button behaves differently on one.
                browser.addItem (store.nameOf (i) + (store.isFactory (i) ? "  (factory)" : ""),
                                 ++item);
            }
        }

        listedSize = store.size();

        // The selection is restored by id, because a save can insert a preset ahead of the
        // loaded one and every id after it shifts.
        browser.setSelectedId (keep, juce::dontSendNotification);
    }

    void PatchBar::refresh()
    {
        auto& store = proc.presets.store();

        if (store.size() != listedSize)
            rebuildList();

        const auto now = proc.presets.status();

        // The browser follows the session, not the other way round: a preset loaded by the host
        // has to move the list too.
        const int wanted = now.index >= 0 ? kFirstItemId + 1 + now.index : 0;

        if (browser.getSelectedId() != wanted)
            browser.setSelectedId (wanted, juce::dontSendNotification);

        const juce::String dirty = now.dirty ? " *" : "";

        status.setText (now.category.isEmpty() ? now.name + dirty
                                               : now.category + " / " + now.name + dirty,
                        juce::dontSendNotification);

        status.setColour (juce::Label::textColourId, now.dirty ? kAccent : kText);

        deleteButton.setEnabled (now.index >= 0 && ! now.factory);

        const bool canUndo = proc.presets.canUndo();

        undoButton.setEnabled (canUndo);
        undoButton.setColour (juce::TextButton::textColourOffId, canUndo ? kText : kDim);

        for (int s = 0; s < lockButtons.size(); ++s)
            lockButtons[s]->setToggleState (proc.randomizer.isLocked (s),
                                            juce::dontSendNotification);
    }

    bool PatchBar::loadSelected()
    {
        const int id = browser.getSelectedId();

        if (id < kFirstItemId + 1)
            return false;

        if (! proc.presets.load (id - kFirstItemId - 1))
        {
            setMessage ("could not load that preset");
            return false;
        }

        setMessage ({});
        return true;
    }

    bool PatchBar::step (int delta)
    {
        if (! proc.presets.step (delta))
            return false;

        setMessage ({});
        return true;
    }

    bool PatchBar::loadInit()
    {
        proc.presets.loadInit();
        setMessage ("init patch");

        return true;
    }

    int PatchBar::saveAs (const juce::String& category, const juce::String& name)
    {
        const int index = proc.presets.saveAs (category, name);

        setMessage (index >= 0 ? "saved " + name : "could not save that name");

        return index;
    }

    bool PatchBar::removeSelected()
    {
        if (! proc.presets.remove (proc.presets.currentIndex()))
        {
            setMessage ("a factory preset cannot be deleted");
            return false;
        }

        setMessage ("deleted");

        return true;
    }

    void PatchBar::rollNow()
    {
        // The undo point and the roll are one gesture. Capturing the patch here rather than
        // inside the roll keeps the undo with the session, where the preset loads also use it.
        proc.presets.rememberForUndo();
        proc.randomizer.roll (mixxynth::Randomizer::clockSeed());

        setMessage (proc.randomizer.numLocked() > 0
                      ? "rolled, with " + juce::String (proc.randomizer.numLocked()) + " locked"
                      : "rolled");
    }

    bool PatchBar::undoRoll()
    {
        if (! proc.presets.undo())
            return false;

        setMessage ("undone");

        return true;
    }

    void PatchBar::showSaveDialog()
    {
        auto* window = new juce::AlertWindow ("Save preset",
                                              "Name the patch. The category comes from the name, "
                                              "so \"Bass - My Patch\" makes a Bass preset.",
                                              juce::MessageBoxIconType::NoIcon);

        window->addTextEditor ("name", proc.presets.currentName(), "Category - Name");
        window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, window] (int result)
            {
                const auto typed = window->getTextEditorContents ("name");

                // Read before the callback returns: the window deletes itself afterwards.
                if (result != 0 && typed.isNotEmpty())
                {
                    // The name carries the category, so there is one thing to type rather than
                    // two - and it is the same "Category - Name" the bank's own files use.
                    juce::String category ("Other");
                    juce::String name (typed);
                    mixxynth::PresetStore::splitName (typed, category, name);

                    saveAs (category, name);
                }
            }), true);
    }

    void PatchBar::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (juce::Colour (0xff101418));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (kBorder);
        g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);

        // The dirty dot, drawn rather than typed: it has to read at a glance from across the
        // room, which an asterisk in a label does not.
        if (proc.presets.status().dirty)
        {
            const auto dot = juce::Rectangle<float> (7.0f, 7.0f)
                                 .withCentre ({ b.getRight() - 12.0f, b.getCentreY() });

            g.setColour (kAccent);
            g.fillEllipse (dot);
        }
    }

    void PatchBar::resized()
    {
        auto b = getLocalBounds().reduced (6, 4);

        browser.setBounds (b.removeFromLeft (220));
        b.removeFromLeft (4);
        prevButton.setBounds (b.removeFromLeft (24));
        nextButton.setBounds (b.removeFromLeft (24));
        b.removeFromLeft (6);
        initButton.setBounds (b.removeFromLeft (46));
        saveButton.setBounds (b.removeFromLeft (48));
        deleteButton.setBounds (b.removeFromLeft (42));
        b.removeFromLeft (10);

        rollButton.setBounds (b.removeFromLeft (52));

        for (auto* lock : lockButtons)
        {
            b.removeFromLeft (2);
            lock->setBounds (b.removeFromLeft (40));
        }

        b.removeFromLeft (4);
        undoButton.setBounds (b.removeFromLeft (54));
        b.removeFromLeft (10);

        // The dot needs the last few pixels of the bar to itself.
        auto textArea = b.withTrimmedRight (16);

        status.setBounds (textArea.removeFromTop (juce::jmax (14, textArea.getHeight() / 2)));
        message.setBounds (textArea);
    }

    juce::String PatchBar::debug() const
    {
        const auto now = const_cast<MiXXynthProcessor&> (proc).presets.status();

        juce::String locks;

        for (int s = 0; s < mixxynth::kNumRollSections; ++s)
            locks << (proc.randomizer.isLocked (s) ? "1" : "0");

        juce::String s;
        s << "name=" << now.name
          << " category=" << now.category
          << " index=" << now.index
          << " factory=" << (now.factory ? 1 : 0)
          << " dirty=" << (now.dirty ? 1 : 0)
          << " locks=" << locks
          << " list=" << proc.presets.store().size()
          << " items=" << browser.getNumItems()
          << " selected=" << browser.getSelectedId()
          << " undo=" << (proc.presets.canUndo() ? 1 : 0);

        return s;
    }

    void PatchBar::setMessage (const juce::String& text)
    {
        message.setText (text, juce::dontSendNotification);
    }
}
