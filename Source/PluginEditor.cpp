#include "PluginEditor.h"

#include "AdsrEditor.h"
#include "Algorithms.h"
#include "EditorTheme.h"
#include "ModMatrix.h"
#include "PluginProcessor.h"
#include "SquareCell.h"
#include "SnapValues.h"

using mixxynth::ui::ModKnob;
using mixxynth::ui::ModTarget;
using mixxynth::ui::SquareCell;
using mixxynth::ui::AdsrEditor;

namespace
{
    // Source names come from mixxynth::modSourceNames() so there is exactly one
    // list. A second copy here silently went stale when the module count grew,
    // and the perf-chip loop then read past its end.

    // Mirrors mixxynth::ModDest ordering for the 30 off-diagonal matrix cells.
    int matrixDestFor (int src, int dst)
    {
        int k = 0;

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        {
            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                if (s == d)
                    continue;

                if (s == src && d == dst)
                    return mixxynth::dstMtxFirst + k;

                ++k;
            }
        }

        return mixxynth::dstOff;
    }

    juce::RangedAudioParameter* ranged (MiXXynthProcessor& p, const juce::String& id)
    {
        return p.apvts.getParameter (id);
    }

    juce::AudioParameterChoice* choice (MiXXynthProcessor& p, const juce::String& id)
    {
        return dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (id));
    }

    // Bool toggle wired straight to an APVTS parameter.
    juce::ToggleButton* makeBoolToggle (MiXXynthProcessor& p, const juce::String& id,
                                        const juce::String& text)
    {
        auto* t = new juce::ToggleButton (text);

        if (auto* param = dynamic_cast<juce::AudioParameterBool*> (p.apvts.getParameter (id)))
        {
            t->setToggleState (param->get(), juce::dontSendNotification);
            t->onClick = [t, param]
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (t->getToggleState() ? 1.0f : 0.0f);
                param->endChangeGesture();
            };
        }

        return t;
    }
}

// ---------------------------------------------------------------------------
// Draggable modulation-source chip
// ---------------------------------------------------------------------------
class MiXXynthEditor::SourceChip : public juce::Component
{
public:
    SourceChip (const juce::String& chipName, int sourceIndex)
        : name (chipName), src (sourceIndex)
    {
        setAlpha (0.85f);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff2b343f));
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (juce::Colour (0xff7c8a99));
        g.drawRoundedRectangle (b, 3.0f, 1.0f);
        g.setColour (juce::Colour (0xffe2e8ee));
        g.setFont (11.0f);
        g.drawFittedText (name, getLocalBounds(), juce::Justification::centred, 1);
    }

    void mouseDrag (const juce::MouseEvent&) override
    {
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! container->isDragAndDropActive())
                container->startDragging (juce::var (src), this);
    }

    void mouseEnter (const juce::MouseEvent&) override { setAlpha (1.0f); }
    void mouseExit (const juce::MouseEvent&) override { setAlpha (0.85f); }

private:
    juce::String name;
    int src = 0;
};
// ---------------------------------------------------------------------------
// Sidebar shape strip: drag chip + breakpoint editor + its controls.
// Indices 0..3 are the LFOs, 4..6 the envelopes. Everything edits the ModShape
// directly - shapes are state, not parameters, exactly as in Serum and Vital -
// so there is nothing to automate here, you just draw and play.
// ---------------------------------------------------------------------------
class MiXXynthEditor::ShapeStrip : public juce::Component
{
public:
    ShapeStrip (MiXXynthEditor& owner, int shapeIndex)
        : ed (owner), index (shapeIndex)
    {
        auto& p = ed.processor();
        const bool isLfo = p.shapeIsLfo (index);
        const int src = p.shapeSourceIndex (index);

        chip = std::make_unique<SourceChip> (mixxynth::modSourceNames()[src], src);
        addAndMakeVisible (*chip);

        modeBox.addItemList ({ "LFO", "ENV", "One-shot" }, 1);
        addAndMakeVisible (modeBox);

        resetButton.setButtonText ("reset");
        resetButton.onClick = [this]
        {
            ed.processor().resetShapeToDefault (index);
            load();
            ed.setModStatus (chipName() + " reset to default");
        };
        addAndMakeVisible (resetButton);

        removeButton.setButtonText ("-");
        removeButton.setTooltip ("Remove this module");
        removeButton.onClick = [this] { ed.removeShapeModule (index); };
        addAndMakeVisible (removeButton);

        addAndMakeVisible (shapeEditor);
        shapeEditor.setGridDivisions (16);
        shapeEditor.setAccentColour (isLfo ? juce::Colour (0xff35e0ff) : juce::Colour (0xffffa63d));
        shapeEditor.onChange = [this] { ed.shapeEdited (index, shapeEditor.getShape()); };
        shapeEditor.onStatus = [this] (const juce::String& t) { ed.setModStatus (chipName() + ": " + t); };

        configureBar (rateBar, "Rate (Hz)", 0.01, 40.0);
        configureBar (smoothBar, "Smooth", 0.0, 1.0);
        configureBar (phaseBar, "Phase", 0.0, 1.0);
        configureBar (delayBar, "Delay (s)", 0.0, 5.0);

        syncButton.setButtonText ("sync");
        divBox.addItemList (syncDivisionList(), 1);
        bipolarButton.setButtonText ("bip");
        retrigButton.setButtonText ("rtg");

        for (auto* c : { &rateBar, &smoothBar, &phaseBar, &delayBar })
            addAndMakeVisible (*c);

        addAndMakeVisible (syncButton);
        addAndMakeVisible (divBox);
        addAndMakeVisible (bipolarButton);
        addAndMakeVisible (retrigButton);

        rateBar.onValueChange = [this] { store(); };
        smoothBar.onValueChange = [this] { store(); };
        phaseBar.onValueChange = [this] { store(); };
        delayBar.onValueChange = [this] { store(); };
        syncButton.onClick = [this] { store(); updateEnablement(); };
        divBox.onChange = [this] { store(); };
        bipolarButton.onClick = [this] { store(); };
        retrigButton.onClick = [this] { store(); };
        modeBox.onChange = [this] { store(); updateEnablement(); };

        load();
    }

    static int preferredHeight() { return 22 + 2 + 88 + 3 + 20 + 20 + 6; }

    // Called from the editor's timer: advances the playhead and picks up any
    // external change (preset load) without clobbering an in-progress drag.
    void poll (float phase)
    {
        shapeEditor.setPlayhead (phase);

        if (shapeEditor.isEditing())
            return;

        if (! sameShape (ed.processor().getShape (index), shapeEditor.getShape()))
            load();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff11151a));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (3);
        auto top = b.removeFromTop (22);
        chip->setBounds (top.removeFromLeft (86));
        top.removeFromLeft (4);
        removeButton.setBounds (top.removeFromRight (26).reduced (0, 1));
        resetButton.setBounds (top.removeFromRight (48).reduced (0, 1));
        modeBox.setBounds (top.reduced (0, 1));

        b.removeFromTop (2);
        shapeEditor.setBounds (b.removeFromTop (88));

        b.removeFromTop (3);

        auto r3 = b.removeFromTop (20);
        rateBar.setBounds (r3.removeFromLeft (112).reduced (1, 0));
        syncButton.setBounds (r3.removeFromLeft (44).reduced (1, 0));
        divBox.setBounds (r3.removeFromLeft (66).reduced (1, 0));
        smoothBar.setBounds (r3.reduced (1, 0));

        auto r4 = b.removeFromTop (20);
        phaseBar.setBounds (r4.removeFromLeft (112).reduced (1, 0));
        bipolarButton.setBounds (r4.removeFromLeft (44).reduced (1, 0));
        retrigButton.setBounds (r4.removeFromLeft (44).reduced (1, 0));
        delayBar.setBounds (r4.reduced (1, 0));
    }

private:
    // PART_TWO

    static void configureBar (juce::Slider& s, const juce::String& name, double lo, double hi)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (lo, hi, 0.001);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 16);
        s.setName (name);
        s.setTooltip (name);
    }

    static juce::StringArray syncDivisionList()
    {
        juce::StringArray a;

        for (int i = 0; i < mixxynth::numSyncDivisions(); ++i)
            a.add (mixxynth::syncDivisionNames()[i]);

        return a;
    }

    static bool sameShape (const mixxynth::ModShape& a, const mixxynth::ModShape& b)
    {
        if (a.numPoints != b.numPoints || a.mode != b.mode
              || std::abs (a.rateHz - b.rateHz) > 1.0e-4f
              || std::abs (a.smooth - b.smooth) > 1.0e-4f
              || std::abs (a.phase - b.phase) > 1.0e-4f
              || std::abs (a.delaySeconds - b.delaySeconds) > 1.0e-4f
              || a.sync != b.sync || a.syncIndex != b.syncIndex
              || a.bipolar != b.bipolar || a.retrig != b.retrig)
            return false;

        for (int i = 0; i < a.numPoints; ++i)
        {
            const auto& p = a.points[static_cast<size_t> (i)];
            const auto& q = b.points[static_cast<size_t> (i)];

            if (std::abs (p.x - q.x) > 1.0e-4f
                  || std::abs (p.y - q.y) > 1.0e-4f
                  || std::abs (p.tension - q.tension) > 1.0e-4f)
                return false;
        }

        return true;
    }

    juce::String chipName() const
    {
        const auto& p = ed.processor();

        return p.shapeIsLfo (index)
                 ? "LFO " + juce::String (index + 1)
                 : "Env " + juce::String (index - p.getNumLfoModules() + 1);
    }

    // Reads every control back into the shape and publishes it.
    void store()
    {
        auto s = shapeEditor.getShape();
        s.mode = juce::jlimit (0, mixxynth::kNumShapeModes - 1, modeBox.getSelectedId() - 1);
        s.rateHz = static_cast<float> (rateBar.getValue());
        s.smooth = static_cast<float> (smoothBar.getValue());
        s.phase = static_cast<float> (phaseBar.getValue());
        s.delaySeconds = static_cast<float> (delayBar.getValue());
        s.sync = syncButton.getToggleState();
        s.syncIndex = juce::jmax (0, divBox.getSelectedId() - 1);
        s.bipolar = bipolarButton.getToggleState();
        s.retrig = retrigButton.getToggleState();
        shapeEditor.setShape (s);
        ed.shapeEdited (index, s);
    }

    // Pushes processor state into the controls (constructor and preset loads).
    void load()
    {
        const auto s = ed.processor().getShape (index);
        shapeEditor.setShape (s);
        modeBox.setSelectedId (s.mode + 1, juce::dontSendNotification);
        rateBar.setValue (s.rateHz, juce::dontSendNotification);
        smoothBar.setValue (s.smooth, juce::dontSendNotification);
        phaseBar.setValue (s.phase, juce::dontSendNotification);
        delayBar.setValue (s.delaySeconds, juce::dontSendNotification);
        syncButton.setToggleState (s.sync, juce::dontSendNotification);
        divBox.setSelectedId (juce::jlimit (1, juce::jmax (1, divBox.getNumItems()), s.syncIndex + 1),
                              juce::dontSendNotification);
        bipolarButton.setToggleState (s.bipolar, juce::dontSendNotification);
        retrigButton.setToggleState (s.retrig, juce::dontSendNotification);
        updateEnablement();
    }

    void updateEnablement()
    {
        const bool isLfo = modeBox.getSelectedId() == 1;
        syncButton.setEnabled (isLfo);
        divBox.setEnabled (isLfo && syncButton.getToggleState());
        retrigButton.setEnabled (isLfo);
        bipolarButton.setEnabled (isLfo);
    }

    MiXXynthEditor& ed;
    int index = 0;

    std::unique_ptr<SourceChip> chip;
    mixxynth::ShapeEditorComponent shapeEditor;
    juce::ComboBox modeBox;
    juce::TextButton resetButton, removeButton;
    juce::Slider rateBar, smoothBar, phaseBar, delayBar;
    juce::TextButton syncButton, bipolarButton, retrigButton;
    juce::ComboBox divBox;
};



// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// FM grid furniture
//
// The grid only reads as a grid if every cell is the same shape and none of them
// is a word. So the four column headers are pictures, the dice is a die face
// rather than the word "Dice", and the operator cell is a label and a switch in
// one square instead of a label with a checkbox beside it.
// ---------------------------------------------------------------------------
namespace
{
    // The four column headers, each a glyph that says what the column does without
    // spending a word on it.
    class GridIcon : public juce::Component
    {
    public:
        enum Kind { shape, ratio, level, feedback };

        explicit GridIcon (Kind k) : kind (k) {}

        void paint (juce::Graphics& g) override
        {
            const auto c = getLocalBounds().toFloat().getCentre();
            const float s = static_cast<float> (juce::jmin (getWidth(), getHeight())) * 0.6f;

            g.setColour (juce::Colour (0xff7c8896));

            switch (kind)
            {
                case shape:
                {
                    // One cycle of a sine, which is where the morph starts.
                    const float w = s, h = s * 0.42f;
                    const float x = c.x - w * 0.5f;

                    juce::Path p;
                    p.startNewSubPath (x, c.y);
                    p.cubicTo (x + w * 0.25f, c.y - h, x + w * 0.25f, c.y - h, x + w * 0.5f, c.y);
                    p.cubicTo (x + w * 0.75f, c.y + h, x + w * 0.75f, c.y + h, x + w, c.y);

                    g.strokePath (p, juce::PathStrokeType (1.8f));
                    break;
                }

                case ratio:
                {
                    // A multiplication sign: the ratio multiplies the pitch.
                    const float r = s * 0.36f;
                    g.drawLine (c.x - r, c.y - r, c.x + r, c.y + r, 1.8f);
                    g.drawLine (c.x - r, c.y + r, c.x + r, c.y - r, 1.8f);
                    break;
                }

                case level:
                {
                    // Three rising bars.
                    const float barW = s * 0.17f;
                    const float gap = s * 0.15f;
                    const float left = c.x - (barW * 3.0f + gap * 2.0f) * 0.5f;
                    const float base = c.y + s * 0.42f;

                    for (int i = 0; i < 3; ++i)
                    {
                        const float hgt = s * (0.32f + 0.28f * static_cast<float> (i));
                        g.fillRect (left + static_cast<float> (i) * (barW + gap),
                                    base - hgt, barW, hgt);
                    }

                    break;
                }

                case feedback:
                {
                    // An open loop with an arrowhead: the operator feeding itself.
                    const float r = s * 0.36f;

                    juce::Path arc;
                    arc.addCentredArc (c.x, c.y, r, r, 0.0f, 0.7f, 5.4f, true);
                    g.strokePath (arc, juce::PathStrokeType (1.8f));

                    juce::Path head;
                    head.addTriangle (c.x + r * 0.15f, c.y - r * 1.28f,
                                      c.x + r * 0.72f, c.y - r * 0.95f,
                                      c.x + r * 0.30f, c.y - r * 0.45f);
                    g.fillPath (head);
                    break;
                }
            }
        }

    private:
        Kind kind;
    };

    // A square button showing a die face. The word "Dice" told you what the button
    // did; the die says the same thing in the space the grid has to spare.
    class DiceButton : public juce::Button
    {
    public:
        DiceButton() : juce::Button ("Dice")
        {
            setTooltip ("Randomise every control in this section");
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced (1.0f);
            const bool hot = over || down;

            g.setColour (hot ? juce::Colour (0xff232b34) : juce::Colour (0xff161c22));
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (hot ? juce::Colour (0xffffa53d) : juce::Colour (0xff39434f));
            g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.5f);

            // Six pips in two columns of three.
            const auto c = b.getCentre();
            const float s = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
            const float r = juce::jmax (1.6f, s * 0.12f);
            const float dx = s * 0.23f, dy = s * 0.27f;

            g.setColour (hot ? juce::Colour (0xffffa53d) : juce::Colour (0xffc2ccd6));

            for (int col = 0; col < 2; ++col)
                for (int row = 0; row < 3; ++row)
                    g.fillEllipse (c.x + (col == 0 ? -dx : dx) - r,
                                   c.y + (static_cast<float> (row) - 1.0f) * dy - r,
                                   r * 2.0f, r * 2.0f);
        }
    };

    // The operator cell: the label and the on/off switch in one square. The old
    // layout put a checkbox beside the row, which cost a whole column and made the
    // operator's own identity the least prominent thing about it.
    class OperatorCell : public juce::Button
    {
    public:
        explicit OperatorCell (const juce::String& t) : juce::Button (t)
        {
            setClickingTogglesState (true);
            setTooltip ("Enable or silence this operator");
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat();
            const bool on = getToggleState();

            g.setColour (on ? juce::Colour (0xff18251c) : juce::Colour (0xff101418));
            g.fillRoundedRectangle (b, 3.0f);

            g.setColour (on ? juce::Colour (0xff4ade80)
                            : (over || down ? juce::Colour (0xff55606d) : juce::Colour (0xff2a323d)));
            g.drawRoundedRectangle (b.reduced (0.5f), 3.0f, on ? 2.0f : 1.0f);

            g.setColour (on ? juce::Colour (0xff4ade80) : juce::Colour (0xff5a6673));
            g.setFont (juce::FontOptions (12.0f, on ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (getButtonText(), getLocalBounds(), juce::Justification::centred, 1);
        }
    };

    // A segmented switch for choosing which module of the FM tab you are looking at.
    // Drawn rather than built from TextButtons, so it sits in the same visual language as
    // the cells it switches between.
    //
    // It used to be hardcoded to two segments. It takes a list now, because the tab is
    // absorbing the VA and the filter and that is more than two readings of one grid.
    class ViewSwitch : public juce::Component
    {
    public:
        explicit ViewSwitch (juce::StringArray segmentNames) : names (std::move (segmentNames))
        {
            jassert (! names.isEmpty());
        }

        int getNumSegments() const noexcept { return names.size(); }

        void setSelected (int index, juce::NotificationType n = juce::sendNotificationSync)
        {
            index = juce::jlimit (0, getNumSegments() - 1, index);

            if (selected == index)
                return;

            selected = index;
            repaint();

            if (n != juce::dontSendNotification && onChange != nullptr)
                onChange (selected);
        }

        int getSelected() const noexcept { return selected; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            setSelected (segmentAt (e.getPosition().x));
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();

            g.setColour (juce::Colour (0xff161c22));
            g.fillRoundedRectangle (b, 4.0f);

            for (int i = 0; i < getNumSegments(); ++i)
            {
                const auto seg = segmentBounds (i);

                if (i == selected)
                {
                    g.setColour (juce::Colour (0xffffa53d));
                    g.fillRoundedRectangle (seg.toFloat().reduced (1.5f), 3.0f);
                }

                g.setColour (i == selected ? juce::Colour (0xff101418)
                                           : juce::Colour (0xff8b98a5));
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawFittedText (names[i], seg, juce::Justification::centred, 1);
            }

            g.setColour (juce::Colour (0xff39434f));
            g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
        }

        std::function<void (int)> onChange;

    private:
        juce::Rectangle<int> segmentBounds (int i) const
        {
            const int w = getWidth() / juce::jmax (1, getNumSegments());
            return getLocalBounds().withWidth (w).withX (i * w);
        }

        int segmentAt (int x) const
        {
            const int w = juce::jmax (1, getWidth() / juce::jmax (1, getNumSegments()));
            return juce::jlimit (0, getNumSegments() - 1, x / w);
        }

        juce::StringArray names;
        int selected = 0;
    };
}

// ---------------------------------------------------------------------------
// One modulation slot: source, destination, bipolar depth, clear
// ---------------------------------------------------------------------------
namespace
{
    class SlotRow : public juce::Component
    {
    public:
        SlotRow (MiXXynthProcessor& p, int index)
        {
            const juce::String n = juce::String (index + 1);

            srcParam = choice (p, "mod" + n + "src");

            if (srcParam != nullptr)
            {
                for (int i = 0; i < srcParam->choices.size(); ++i)
                    src.addItem (srcParam->choices[i], i + 1);

                src.setSelectedId (srcParam->getIndex() + 1, juce::dontSendNotification);
                src.onChange = [this]
                {
                    if (srcParam == nullptr)
                        return;

                    srcParam->beginChangeGesture();
                    srcParam->setValueNotifyingHost (
                        srcParam->convertTo0to1 (static_cast<float> (src.getSelectedId() - 1)));
                    srcParam->endChangeGesture();
                };
            }

            dstParam = choice (p, "mod" + n + "dst");

            if (dstParam != nullptr)
            {
                for (int i = 0; i < dstParam->choices.size(); ++i)
                    dst.addItem (dstParam->choices[i], i + 1);

                dst.setSelectedId (dstParam->getIndex() + 1, juce::dontSendNotification);
                dst.onChange = [this]
                {
                    if (dstParam == nullptr)
                        return;

                    dstParam->beginChangeGesture();
                    dstParam->setValueNotifyingHost (
                        dstParam->convertTo0to1 (static_cast<float> (dst.getSelectedId() - 1)));
                    dstParam->endChangeGesture();
                };
            }

            auto* depthParam = ranged (p, "mod" + n + "depth");

            // A cell, like every other value on the FM tab. It carries no label of its own:
            // the column is headed "Mod Depth" once, rather than thirty-two times.
            //
            // And no value text either. A row here is 26px tall, so a number in the cell would
            // be most of the cell and would sit on top of the fill - which is already showing
            // the value. Dragging reports the number in the status line, where there is room
            // for it.
            depth = new SquareCell ("Mod Depth", depthParam, mixxynth::dstOff, false);
            addAndMakeVisible (depth);

            clear.setButtonText ("x");
            clear.onClick = [this, &p, n]
            {
                if (auto* s = choice (p, "mod" + n + "src"))
                {
                    s->beginChangeGesture();
                    s->setValueNotifyingHost (0.0f);
                    s->endChangeGesture();
                    src.setSelectedId (1, juce::dontSendNotification);
                }

                if (auto* d = choice (p, "mod" + n + "dst"))
                {
                    d->beginChangeGesture();
                    d->setValueNotifyingHost (0.0f);
                    d->endChangeGesture();
                    dst.setSelectedId (1, juce::dontSendNotification);
                }

                if (onChanged != nullptr)
                    onChanged();
            };

            addAndMakeVisible (label);
            label.setText ("Slot " + n, juce::dontSendNotification);
            addAndMakeVisible (src);
            addAndMakeVisible (dst);
            addAndMakeVisible (clear);
        }

        // Re-reads the three parameters this row drives.
        //
        // The combos are bound by hand in the constructor, so without this a slot
        // assigned by DRAGGING A CHIP ONTO A KNOB - which writes exactly these
        // parameters - leaves the row still showing Off. That was the whole of the
        // "drag and drop does not sync with the modulation matrix" bug: the
        // parameters were always correct, and this tab simply never looked again.
        //
        // Guarded so it cannot fight the user: an open combo popup or a knob being
        // dragged is left alone, and the timer picks it up on the next tick.
        void poll()
        {
            if (srcParam != nullptr && ! src.isPopupActive())
            {
                const int id = srcParam->getIndex() + 1;

                if (src.getSelectedId() != id)
                    src.setSelectedId (id, juce::dontSendNotification);
            }

            if (dstParam != nullptr && ! dst.isPopupActive())
            {
                const int id = dstParam->getIndex() + 1;

                if (dst.getSelectedId() != id)
                    dst.setSelectedId (id, juce::dontSendNotification);
            }

            if (depth != nullptr && ! depth->isAdjusting())
                depth->refreshFromParam();
        }

        // For the audit: what this row is actually SHOWING. The bug this exists to
        // catch was invisible in the parameters and visible only here.
        juce::String debugDump() const
        {
            juce::String s;
            s << "srcId=" << src.getSelectedId()
              << " srcText=" << src.getText()
              << " dstId=" << dst.getSelectedId()
              << " dstText=" << dst.getText();

            return s;
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (2);
            label.setBounds (b.removeFromLeft (labelWidth));
            src.setBounds (b.removeFromLeft (srcWidth).reduced (2));
            clear.setBounds (b.removeFromRight (clearWidth).reduced (2, 8));

            // The depth cell takes the row's height, so it is square and as large as the row
            // allows - which is what the taller rows are for.
            auto depthArea = b.removeFromRight (depthWidth);
            const int side = juce::jmin (depthArea.getHeight(), depthArea.getWidth());

            depth->setBounds (depthArea.withSizeKeepingCentre (side, side));

            // The destination takes whatever is left, so the row FILLS its width. With the
            // columns at fixed widths the row used to end short and leave a strip of nothing
            // before the clear button.
            dst.setBounds (b.reduced (2));
        }

        std::function<void()> onChanged;

        ModTarget* getDepthTarget() const { return depth; }

        // Where the depth column sits, measured from the row's RIGHT edge - it is anchored
        // there now that the destination column is the one that stretches.
        //
        // The header above it needs this, and only the row knows it. A header positioned by a
        // number repeated elsewhere lines up until someone changes a column width, and then
        // quietly does not.
        static constexpr int depthColumnRightOffset() { return 2 + clearWidth + depthWidth; }

        static constexpr int depthColumnWidth() { return depthWidth; }

    private:
        // The row's column widths, named so the header above the depth column can be placed
        // from the same numbers the row lays itself out with.
        static constexpr int labelWidth = 52;
        static constexpr int srcWidth = 110;
        static constexpr int depthWidth = 48;
        static constexpr int clearWidth = 26;

        juce::Label label;
        juce::ComboBox src, dst;
        juce::AudioParameterChoice* srcParam = nullptr;
        juce::AudioParameterChoice* dstParam = nullptr;
        SquareCell* depth = nullptr;
        juce::TextButton clear;
    };
}

// ---------------------------------------------------------------------------
// FM page: algorithm + FM index + per-op ratio/level/feedback/ADSR, the VA
// panel, the envelope section, and the modulation matrix
// ---------------------------------------------------------------------------
// A combo font small enough that "Low Pass" fits one square.
//
// JUCE takes a combo's font from the LookAndFeel rather than from the control - there is no
// setFont to call - so a smaller font means a LookAndFeel. Six combos share this one, and
// FmPage owns it, declared before them so it outlives every control pointing at it.
class CompactComboLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        juce::ignoreUnused (box);
        return juce::Font (juce::FontOptions (10.5f));
    }

    // The default reserves about a third of the width for the arrow, which on a single square
    // leaves too little for the word - "Low" came out as "L". A fixed narrow strip for the
    // arrow instead, so the text gets everything else.
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (4, 1, juce::jmax (0, box.getWidth() - 22), box.getHeight() - 2);
    }
};

class MiXXynthEditor::FmPage : public juce::Component
{
public:
    // The per-operator envelope editors, in operator order. Exposed for the audit,
    // because they are deliberately not ModTargets and so the modTargets list does
    // not hold them.
    const juce::OwnedArray<AdsrEditor>& envelopeEditors() const { return adsrEditors; }

    explicit FmPage (MiXXynthEditor& e) : ed (e)
    {
        auto& p = e.processor();

        // M6: the algorithm section is gone from the UI, so the parameter is pinned
        // to Free (matrix) right here. Leaving it wherever it happened to be would
        // leave an invisible control still overriding the routing matrix, which is
        // exactly the confusing behaviour being escaped. The DX7 DSP stays in place
        // so the feature can come back once it works properly.
        if (auto* algoParam = choice (p, "algorithm"))
        {
            if (algoParam->getIndex() != mixxynth::kFreeAlgorithm)
            {
                algoParam->beginChangeGesture();
                algoParam->setValueNotifyingHost (
                    algoParam->convertTo0to1 (static_cast<float> (mixxynth::kFreeAlgorithm)));
                algoParam->endChangeGesture();
            }
        }

        diceButton.onClick = [this] { rollDice(); };
        addAndMakeVisible (diceButton);

        // Snap or Free. Bound to the parameter so it saves with the patch, and the label is
        // set from the value on the editor's poll rather than here - one place decides what
        // the button says, and it is the same place that tells the controls how to behave.
        snapButton.setTooltip ("Snap to musical values, or move freely");
        addAndMakeVisible (snapButton);
        snapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            ed.processor().apvts, "snapValues", snapButton);

        audibility.setJustificationType (juce::Justification::centredLeft);
        audibility.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (audibility);

        // One glyph per value column, so the header row stays square.
        headerIcons.add (new GridIcon (GridIcon::shape));
        headerIcons.add (new GridIcon (GridIcon::ratio));
        headerIcons.add (new GridIcon (GridIcon::level));
        headerIcons.add (new GridIcon (GridIcon::feedback));

        for (auto* icon : headerIcons)
            addAndMakeVisible (icon);

        // The envelope section's page switch. It sits on the grid's header row, so the two
        // sections read across as one layout rather than as two stacks beside each other.
        adsrPages.onChange = [this] (int page) { applyAdsrPage (page); };
        addAndMakeVisible (adsrPages);

        // FM Index is one value for the whole stack rather than a per-operator one,
        // so it sits in the header row's first cell, out of the operator rows' way.
        // FM Index is one value for the whole stack rather than a per-operator one,
        // so it sits in the header row's first cell, out of the operator rows' way.
        indexCell = addCell (ranged (p, "fmIndex"), mixxynth::dstFmIndex, "FM Index");

        // It is its own column heading. Every other grid cell has an icon above it and does not
        // need to say what it is; this one has nothing above it, so without this it is a bare
        // number in the corner with no way to tell what it controls.
        indexCell->setShowLabel (true);

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);

            // The operator cell is the label and the on/off switch in one square. A
            // disabled operator outputs silence, which also stops it modulating
            // anything downstream.
            auto* t = new OperatorCell ("OP " + n);
            opOnToggles.add (t);
            addAndMakeVisible (t);

            if (auto* param = dynamic_cast<juce::AudioParameterBool*> (ranged (p, "op" + n + "on")))
            {
                t->setToggleState (param->get(), juce::dontSendNotification);
                t->onClick = [t, param]
                {
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (t->getToggleState() ? 1.0f : 0.0f);
                    param->endChangeGesture();
                };
            }

            // Shape first, so the row reads: what the operator is, then how much of
            // it there is.
            auto* shape = addCell (ranged (p, "op" + n + "wave"), mixxynth::dstOff,
                                   "Op" + n + " Shape");

            // The morph is a continuous sweep through sine > triangle > saw > square >
            // pulses, so the cell draws the shape rather than naming it. The name is still
            // what the status line reports while you drag, which is where a word is
            // genuinely more use than a picture.
            shape->setValueTextFunction ([] (float v)
            {
                return juce::String (mixxynth::OpWaveTables::shapeName (v));
            });

            shape->setWaveformDisplay (&p.getWaveTables());

            addCell (ranged (p, "op" + n + "ratio"), mixxynth::dstOp1Ratio + i, "Op" + n + " Ratio");
            addCell (ranged (p, "op" + n + "level"), mixxynth::dstOp1Level + i, "Op" + n + " Level");
            addCell (ranged (p, "op" + n + "feedback"), mixxynth::dstOff, "Op" + n + " Fb");

            // The envelope: the same editor the modulation section uses, so an
            // operator envelope looks and behaves like a modulation one. It is not a
            // modulation target, because it only ever controls this operator.
            auto* env = new AdsrEditor (ranged (p, "op" + n + "attack"),
                                        ranged (p, "op" + n + "decay"),
                                        ranged (p, "op" + n + "sustain"),
                                        ranged (p, "op" + n + "release"));

            env->onStatus = [this] (const juce::String& t) { ed.setModStatus (t); };

            adsrEditors.add (env);
            addChildComponent (env);
        }

        // ---- page two: the per-operator filters ----
        // Everything here is hidden until the Filter page is selected, which is why these use
        // addChildComponent rather than addAndMakeVisible: the page switch owns their
        // visibility, and a control that adds itself visible would flash on the wrong page
        // for one frame.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);

            // The filter's own envelope, through the same editor the other two envelope
            // types use - a fourth kind of envelope that behaved differently would be a
            // fourth set of bugs.
            auto* fenv = new AdsrEditor (ranged (p, "op" + n + "fltAttack"),
                                         ranged (p, "op" + n + "fltDecay"),
                                         ranged (p, "op" + n + "fltSustain"),
                                         ranged (p, "op" + n + "fltRelease"));

            fenv->onStatus = [this] (const juce::String& t) { ed.setModStatus (t); };
            filterEnvEditors.add (fenv);
            addChildComponent (fenv);

            // On/off, labelled with the operator it belongs to: six identical "FILT" cells
            // would say nothing about which one you are switching.
            auto* on = new OperatorCell ("OP " + n);
            filterOnCells.add (on);
            addChildComponent (on);

            if (auto* b = dynamic_cast<juce::AudioParameterBool*> (ranged (p, "op" + n + "fltOn")))
            {
                on->setToggleState (b->get(), juce::dontSendNotification);
                on->onClick = [on, b]
                {
                    b->beginChangeGesture();
                    b->setValueNotifyingHost (on->getToggleState() ? 1.0f : 0.0f);
                    b->endChangeGesture();
                };
            }

            // The mode is a list, so it stays a combo: a cell can hold a value, not a list.
            auto* type = new juce::ComboBox();

            // Through the compact look and feel, because the combo gets one square and
            // "Low Pass" does not fit across one at the default font - it came out as "..." and
            // told you nothing about which mode you were in.
            type->setLookAndFeel (&compactComboLook);

            if (auto* tp = dynamic_cast<juce::AudioParameterChoice*> (ranged (p, "op" + n + "fltType")))
            {
                for (int c = 0; c < tp->choices.size(); ++c)
                    type->addItem (tp->choices[c], c + 1);

                type->setSelectedId (tp->getIndex() + 1, juce::dontSendNotification);
                type->onChange = [type, tp]
                {
                    tp->beginChangeGesture();
                    tp->setValueNotifyingHost (
                        tp->convertTo0to1 (static_cast<float> (type->getSelectedId() - 1)));
                    tp->endChangeGesture();
                };
            }

            filterTypeBoxes.add (type);
            addChildComponent (type);

            // Real modulation destinations, which they were not before: the cells existed and
            // worked, but nothing could drive them, so an LFO could reach the per-voice filter
            // and not an operator's own. Eighteen destinations were added for this.
            filterCutoffCell[static_cast<size_t> (i)] =
                addFilterCell (ranged (p, "op" + n + "fltCutoff"),
                               mixxynth::dstOp1FilterCutoff + i, "Cutoff");
            filterResoCell[static_cast<size_t> (i)] =
                addFilterCell (ranged (p, "op" + n + "fltReso"),
                               mixxynth::dstOp1FilterReso + i, "Reso");
            filterDriveCell[static_cast<size_t> (i)] =
                addFilterCell (ranged (p, "op" + n + "fltDrive"),
                               mixxynth::dstOp1FilterDrive + i, "Drive");

            addRouteButtons (ranged (p, "op" + n + "fltRoute"), i);
        }

        // ---- the modulation matrix ----
        // All thirty-two slots, built up front. A slot's controls only exist if it has
        // parameters, which it always does - they are a fixed pool - so unlike the FX rack
        // there is nothing to build lazily here.
        for (int n = 0; n < mixxynth::kNumModSlots; ++n)
        {
            auto* row = new SlotRow (p, n);
            row->onChanged = [this] { ed.refreshModRings(); };
            modContent.addAndMakeVisible (row);
            modRows.add (row);
            ed.modTargets.push_back (row->getDepthTarget());
        }

        modViewport.setViewedComponent (&modContent, false);
        modViewport.setScrollBarsShown (true, false);
        addChildComponent (modViewport);

        // The depth column's heading. It sits on the header row above the rows rather than
        // inside the scrolled content, so it stays put while the slots scroll under it.
        modDepthHeader.setText ("Mod Depth", juce::dontSendNotification);
        modDepthHeader.setJustificationType (juce::Justification::centred);
        modDepthHeader.setColour (juce::Label::textColourId, juce::Colour (0xff7c8896));
        modDepthHeader.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        addChildComponent (modDepthHeader);

        // ---- the routing view ----
        // The header row's first cell names the row axis, because this grid reads as
        // "row sends into column" and that is not obvious from the cells themselves.
        routingFrom.setText ("FROM", juce::dontSendNotification);
        routingFrom.setJustificationType (juce::Justification::centred);
        routingFrom.setColour (juce::Label::textColourId, juce::Colour (0xff7c8896));
        routingFrom.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        addChildComponent (routingFrom);

        for (int d = 0; d < MiXXFmVoice::numOps; ++d)
        {
            auto* h = new juce::Label();
            h->setText ("OP" + juce::String (d + 1), juce::dontSendNotification);
            h->setJustificationType (juce::Justification::centred);
            h->setColour (juce::Label::textColourId, juce::Colour (0xff7c8896));
            h->setFont (juce::FontOptions (11.0f, juce::Font::bold));
            routingHeaders.add (h);
            addChildComponent (h);
        }

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        {
            const juce::String sn = juce::String (s + 1);

            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                const juce::String dn = juce::String (d + 1);

                if (s == d)
                {
                    // The diagonal is this operator's own output, not a self-send. A
                    // self-send would be feedback, and feedback already has a column
                    // in the values view - which is exactly why this cell is free.
                    auto* t = new OperatorCell ("OUT");
                    t->setTooltip ("Whether Op" + sn + " reaches the output on its own");
                    outCells.add (t);
                    addChildComponent (t);

                    if (auto* param = dynamic_cast<juce::AudioParameterBool*> (ranged (p, "carrier" + sn)))
                    {
                        t->setToggleState (param->get(), juce::dontSendNotification);
                        t->onClick = [t, param]
                        {
                            param->beginChangeGesture();
                            param->setValueNotifyingHost (t->getToggleState() ? 1.0f : 0.0f);
                            param->endChangeGesture();
                        };
                    }

                    continue;
                }

                routingCell[static_cast<size_t> (s * MiXXFmVoice::numOps + d)]
                    = addRoutingCell (ranged (p, "mtx" + sn + dn), matrixDestFor (s, d),
                                      "Op" + sn + " to Op" + dn);
            }
        }

        // ---- the VA panel ----
        // The VA oscillator and the per-voice filter. This is a SECTION of the tab rather
        // than another reading of the grid: the VA is one oscillator and one filter, not
        // six operators, so it sits in its own column to the left of the grid and stays
        // visible whichever view the grid is showing.
        // Captions for the panel and for its two controls that are not cells, and so cannot
        // caption themselves. One style, so the panel's words all look alike.
        auto caption = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centredLeft);
            l.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
            l.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            addAndMakeVisible (l);
        };

        caption (vaHeader, "VA");
        caption (vaEnvLabel, "ENVELOPE");
        caption (vaModeLabel, "FILTER TYPE");

        // The shape is the same morph the operators use, so its cell draws the same
        // waveform the operator shape cells do - and keeps its caption, because this cell
        // is in a panel rather than under a column header.
        vaCells[0] = addVaCell (ranged (p, "vaShape"), mixxynth::dstVaPulseWidth, "VA Shape");
        vaCells[0]->setValueTextFunction ([] (float v)
        {
            return juce::String (mixxynth::OpWaveTables::shapeName (v));
        });

        vaCells[0]->setWaveformDisplay (&p.getWaveTables());

        vaCells[1] = addVaCell (ranged (p, "vaSubLevel"), mixxynth::dstOff, "Sub");
        vaCells[2] = addVaCell (ranged (p, "vaNoiseLevel"), mixxynth::dstOff, "Noise");
        vaCells[3] = addVaCell (ranged (p, "vaLevel"), mixxynth::dstVaLevel, "VA Level");
        vaCells[4] = addVaCell (ranged (p, "vaDetune"), mixxynth::dstVaDetune, "Detune");
        vaCells[5] = addVaCell (ranged (p, "vaOctave"), mixxynth::dstOff, "Octave");

        // The filter's three values follow the oscillator's in the same list, so indices
        // 6..8 are cutoff, resonance and drive.
        addVaCell (ranged (p, "filterCutoff"), mixxynth::dstFilterCutoff, "Cutoff");
        addVaCell (ranged (p, "filterReso"), mixxynth::dstFilterReso, "Reso");
        addVaCell (ranged (p, "filterDrive"), mixxynth::dstFilterDrive, "Drive");

        // The VA's amplitude envelope: the same editor the operator envelopes use, so the
        // two cannot drift apart.
        vaEnvEditor = new AdsrEditor (ranged (p, "vaAttack"), ranged (p, "vaDecay"),
                                      ranged (p, "vaSustain"), ranged (p, "vaRelease"));
        vaEnvOwned.add (vaEnvEditor);
        addChildComponent (vaEnvEditor);

        // The filter's on/off and its mode. The mode is a choice, so it stays a combo box:
        // a cell can hold a value, not a list.
        auto* filtOn = new OperatorCell ("FILT");
        vaToggles.add (filtOn);
        addChildComponent (filtOn);

        if (auto* param = dynamic_cast<juce::AudioParameterBool*> (ranged (p, "filterOn")))
        {
            filtOn->setToggleState (param->get(), juce::dontSendNotification);
            filtOn->onClick = [filtOn, param]
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (filtOn->getToggleState() ? 1.0f : 0.0f);
                param->endChangeGesture();
            };
        }

        if (auto* typeParam = choice (p, "filterType"))
        {
            for (int i = 0; i < typeParam->choices.size(); ++i)
                vaFilterType.addItem (typeParam->choices[i], i + 1);

            vaFilterType.setSelectedId (typeParam->getIndex() + 1, juce::dontSendNotification);
            vaFilterType.onChange = [this, typeParam]
            {
                typeParam->beginChangeGesture();
                typeParam->setValueNotifyingHost (
                    typeParam->convertTo0to1 (static_cast<float> (vaFilterType.getSelectedId() - 1)));
                typeParam->endChangeGesture();
            };
        }

        addChildComponent (vaFilterType);

        viewSwitch.onChange = [this] (int v) { applyView (v); };
        addAndMakeVisible (viewSwitch);

        // Both views are built up front; only one is shown at a time.
        applyView (viewSwitch.getSelected());

        refreshAudibility();
    }

    // Randomises every knob on this page. The roll is constrained to musically
    // usable ranges rather than 0..max, and at least one carrier is forced to be
    // audible so a roll can never land on silence.
    void rollDice()
    {
        auto& p = ed.processor();

        auto set = [&p] (const juce::String& id, float realValue)
        {
            if (auto* param = ranged (p, id))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->convertTo0to1 (realValue));
                param->endChangeGesture();
            }
        };

        auto read = [&p] (const juce::String& id)
        {
            if (auto* param = ranged (p, id))
                return param->convertFrom0to1 (param->getValue());

            return 0.0f;
        };

        juce::ignoreUnused (read);

        // Which operators reach the output depends on the algorithm, so work the
        // carriers out first and make sure one of them actually sounds.
        const auto isCarrier = carrierMask();

        // Ratio 0 stops an operator's phase advancing, so the roll picks from real
        // musical ratios rather than the whole range.
        static const float ratios[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f };

        set ("fmIndex", 0.15f + rng.nextFloat() * 1.1f);

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);

            set ("op" + n + "ratio", ratios[rng.nextInt (8)]);
            set ("op" + n + "level", 0.15f + rng.nextFloat() * 0.85f);
            set ("op" + n + "feedback", rng.nextFloat() * 0.7f);
            set ("op" + n + "attack", rng.nextFloat() * 0.5f);
            set ("op" + n + "decay", 0.05f + rng.nextFloat() * 1.95f);
            set ("op" + n + "sustain", rng.nextFloat());
            set ("op" + n + "release", 0.05f + rng.nextFloat() * 2.95f);
            set ("op" + n + "wave", rng.nextFloat());
        }

        // Guarantee audibility: give one carrier a real ratio, a real level and an
        // envelope that sustains.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            if (! isCarrier[static_cast<size_t> (i)])
                continue;

            const juce::String n = juce::String (i + 1);

            set ("op" + n + "ratio", ratios[rng.nextInt (8)]);
            set ("op" + n + "level", 0.5f + rng.nextFloat() * 0.5f);
            set ("op" + n + "sustain", 0.4f + rng.nextFloat() * 0.6f);
            set ("op" + n + "decay", 0.1f + rng.nextFloat() * 1.5f);
            break;
        }

        ed.setModStatus ("Dice rolled a new FM patch");
    }

    // refreshWaveLabels is deliberately gone. It existed because the wave morph was
    // shown in a knob's label, and a knob does not know how to rename itself, so
    // something had to poll. The shape cell is given a text function instead, and
    // evaluates it at paint time from the live value, so there is nothing to poll.

    // Which operators reach the output, given the current algorithm (or the
    // carrier switches in Free mode).
    std::array<bool, MiXXFmVoice::numOps> carrierMask()
    {
        std::array<bool, MiXXFmVoice::numOps> mask {};
        auto& p = ed.processor();

        auto* a = choice (p, "algorithm");
        const int algoIndex = a != nullptr ? a->getIndex() : 0;

        if (algoIndex >= 0 && algoIndex < mixxynth::kNumAlgorithms)
        {
            const auto& algo = mixxynth::algorithmTable()[static_cast<size_t> (algoIndex)];

            for (int i = 0; i < MiXXFmVoice::numOps; ++i)
                mask[static_cast<size_t> (i)] = algo.carrier[static_cast<size_t> (i)];
        }
        else
        {
            for (int i = 0; i < MiXXFmVoice::numOps; ++i)
                mask[static_cast<size_t> (i)] = readReal ("carrier" + juce::String (i + 1)) > 0.5f;
        }

        return mask;
    }

    float readReal (const juce::String& id)
    {
        if (auto* param = ranged (ed.processor(), id))
            return param->convertFrom0to1 (param->getValue());

        return 0.0f;
    }

    // Sets the Snap switch's label. The label names the mode you are IN rather than the one
    // you would switch to, because a switch that names the other state is one people misread.
    void setSnapLabel (bool snap)
    {
        const juce::String text = snap ? "Snap" : "Free";

        if (snapButton.getButtonText() != text)
            snapButton.setButtonText (text);
    }

    // With the all-zero init patch it is easy to sit on a patch that cannot make
    // a sound at all, which reads as a broken knob. This says so, and says why.
    void refreshAudibility()
    {
        // The page polls this, so it is also where the envelopes are told to catch up
        // with a preset load or a dice roll. Each one decides for itself whether
        // anything actually moved, so a quiet poll costs nothing.
        for (auto* env : adsrEditors)
            env->refreshFromParams();

        const auto carriers = carrierMask();
        bool fmAudible = false;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            if (! carriers[static_cast<size_t> (i)])
                continue;

            const auto n = juce::String (i + 1);

            if (readReal ("op" + n + "on") < 0.5f)
                continue;

            if (readReal ("op" + n + "level") <= 0.001f)
                continue;

            // Ratio 0 stops the phase advancing, so the operator never moves off
            // zero whatever its waveform is.
            if (readReal ("op" + n + "ratio") <= 0.001f)
                continue;

            // An all-zero envelope is a one-sample spike, not a sustain.
            if (readReal ("op" + n + "sustain") <= 0.001f
                  && readReal ("op" + n + "decay") <= 0.001f)
                continue;

            fmAudible = true;
        }

        const float vaLevel = readReal ("vaLevel");

        if (fmAudible)
        {
            audibility.setText ("FM audible", juce::dontSendNotification);
            audibility.setColour (juce::Label::textColourId, juce::Colour (0xff4ade80));
        }
        else if (vaLevel > 0.001f)
        {
            audibility.setText ("FM is silent - you are hearing the VA oscillator. "
                                "Give a carrier Level, Ratio and a sustaining envelope, or press Dice.",
                                juce::dontSendNotification);
            audibility.setColour (juce::Label::textColourId, juce::Colour (0xffffa63d));
        }
        else
        {
            audibility.setText ("Silent: no carrier has Level, Ratio and a sustaining envelope. "
                                "Press Dice, or raise a carrier's Level and its A/D/S/R.",
                                juce::dontSendNotification);
            audibility.setColour (juce::Label::textColourId, juce::Colour (0xffef4444));
        }
    }

    void paint (juce::Graphics& g) override
    {
        // The hint swaps with the view, and that matters for one specific reason: the
        // values view's Fb column and the routing view's diagonal both sit in the "self"
        // position and they do NOT mean the same thing. Rather than hoping nobody
        // conflates them, the text says which is which.
        // The bars either side of the grid. They run the grid's full height rather than their
        // own section's, so they read as section breaks down the whole tab instead of as
        // underlines to the things beside them.
        //
        // Drawn in one loop from one set of numbers, because two bars that are supposed to
        // match are two bars that can stop matching: symmetry you have to maintain by hand
        // is symmetry that eventually is not there.
        if (dividerBottom > dividerTop)
        {
            g.setColour (juce::Colour (0xff4a5765));

            for (const int x : { dividerX, divider2X })
                g.fillRect (x, dividerTop, dividerWidth, dividerBottom - dividerTop);
        }

        const int view = viewSwitch.getSelected();

        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText (view == 1
                            ? "Each cell sends its row's operator into its column's operator. The diagonal is that "
                              "operator's own output, not feedback - feedback is the Fb column back in Values."
                            : "Drag a cell up or down to set its value. Drop a modulation source onto one to modulate it, "
                              "then drag the white line to set how far it reaches. The VA panel holds the oscillator "
                              "and the filter; the modulation matrix is in the envelope section's Volume page.",
                          getLocalBounds().removeFromTop (18), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (18);   // the hint line, drawn in paint()

        // Toolbar: the die, the view switch, then the audibility warning.
        auto toolbar = b.removeFromTop (30);
        diceButton.setBounds (toolbar.removeFromLeft (30).reduced (2));
        toolbar.removeFromLeft (6);

        // Beside the die: the two are both about what moving a control does rather than what
        // it is set to.
        snapButton.setBounds (toolbar.removeFromLeft (66).reduced (0, 2));
        toolbar.removeFromLeft (8);

        viewSwitch.setBounds (toolbar.removeFromLeft (150).reduced (0, 2));
        toolbar.removeFromLeft (8);
        audibility.setBounds (toolbar);

        b.removeFromTop (6);

        const bool routing = viewSwitch.getSelected() == 1;

        constexpr int numRows = MiXXFmVoice::numOps + 1;  // header row + one per operator
        constexpr int gap = 3;

        // The geometry is computed ONCE, from the wider of the two grid views, and both are
        // laid out from that same origin. That is what makes the views align: the operator
        // column and the first four value columns sit at identical x in both, so switching
        // views moves nothing.
        //
        //   values:  operator, shape, ratio, level, fdbk
        //   routing: operator, then one column per target operator
        //
        // Routing is the wider of the two, so the values view leaves slack on the right
        // rather than re-centring itself into a different place.

        // The VA panel sits to the LEFT of the grid and the envelope section to the RIGHT,
        // so the width budget has to cover all three. That is what stops the grid being
        // sized as if it had the whole tab and then colliding with a section.
        constexpr int vaCols = 3;
        constexpr int vaRows = 5;   // oscillator x2, envelope, filter x2

        // The sections are separated from the grid by a BAR rather than by a bare gap. A gap
        // alone was not enough: at a glance everything read as one very wide grid, and a
        // section's cells looked like stray columns of it.
        //
        // The bars hug the grid. A gap either side of them was spending width on separation
        // that the squares need far more: at nineteen squares across, every pixel of gap is a
        // pixel off every cell.
        constexpr int dividerGap = 8;

        // The envelope section is sized for its WIDER page. Page one is the envelope alone;
        // page two adds the filter's own controls and the routing switch. Sizing it for the
        // narrower page would move the grid every time you switched pages, because the whole
        // block is centred as one.
        constexpr int adsrSquares = 3 + 5 + 1;   // envelope, filter controls, route switch

        // The widest the grid itself ever is: Routing's operator column plus its six targets.
        // Values is narrower, so it leaves slack on the right rather than re-centring itself -
        // which is what keeps the operator column at the same x in both views.
        constexpr int gridSquares = 7;

        constexpr int acrossSquares = vaCols + gridSquares + adsrSquares;
        constexpr int acrossGaps = (vaCols - 1) + (gridSquares - 1) + (adsrSquares - 1)
                                     + 2;   // one per divider

        // Squares are sized by whichever constraint binds first: the room there is, or the
        // room the squares need. This is what lets the grid scale with the window without
        // ever distorting, and why the minimum window size has to be large enough for the
        // squares to stay legible.
        const int rowH = (b.getHeight() - gap * (numRows - 1)) / numRows;

        const int square = juce::jmax (22, juce::jmin (rowH,
                                      (b.getWidth() - 2 * dividerGap - gap * acrossGaps)
                                        / acrossSquares));

        // The grid's footprint is whichever view is SHOWING, not the wider of the two.
        //
        // It used to reserve the routing view's width always, so that the operator column
        // could not shift when you switched. That is right for the grid, but it left the
        // values view two whole columns of nothing between its last control and the divider -
        // and the envelope section had to sit beyond that hole, which is a long way from the
        // grid it belongs to.
        //
        // Following the view buys a section that hugs the grid in both, and more room for the
        // section's own controls than the routing view leaves. It costs a reflow when you
        // switch, which is the price of not having a permanent hole.
        const int gridColsShown = routing ? gridSquares : 5;

        const int gridW = gridColsShown * square + (gridColsShown - 1) * gap;
        const int gridH = numRows * square + (numRows - 1) * gap;

        // The VA panel and the grid keep their square cells; the envelope section does not
        // have to, because it has no squares to keep square - its envelope is already three
        // columns wide by one tall. So it takes whatever width is left to the right margin,
        // which is what stops the tab having a strip of nothing along its right edge.
        const int vaW = vaCols * square + (vaCols - 1) * gap;

        const int gridX = b.getX() + vaW + dividerGap;
        const int adsrX = gridX + gridW + dividerGap;
        const int adsrW = juce::jmax (0, b.getRight() - adsrX);

        // The section's own column pitch. Divided from the room it actually has rather than
        // from the grid's square, so it grows to fill instead of leaving a gap.
        const int sectionCol = juce::jmax (22, (adsrW - (adsrSquares - 1) * gap) / adsrSquares);

        // Left-aligned and filling, rather than centred: there is nothing left over to centre
        // in, and centring a block that already spans the tab would only hide a mistake.
        const int originX = b.getX();
        const int originY = b.getY() + juce::jmax (0, (b.getHeight() - gridH) / 2);

        // Where the two dividers go. Stored for paint() rather than recomputed there, because
        // paint() has no access to the layout arithmetic above.
        //
        // Both run the grid's full height rather than their own section's, so they read as
        // section breaks down the tab instead of as underlines to the things beside them.
        dividerX = gridX - (dividerGap + dividerWidth) / 2;
        divider2X = adsrX - (dividerGap + dividerWidth) / 2;
        dividerTop = originY;
        dividerBottom = originY + gridH;

        // Every grid cell is the same square now that the envelope column has left for its
        // own section, so this is a plain pitch rather than a special case for a wider last
        // column. Rows run from the header row at 0.
        auto cellAt = [&] (int row, int col)
        {
            return juce::Rectangle<int> (gridX + col * (square + gap),
                                         originY + row * (square + gap), square, square);
        };

        // Both views are laid out every time, not just the visible one. They share the
        // operator column, and laying both out means their bounds are always valid
        // whichever view is showing - so a view switch before the first resize cannot
        // leave one of them unsized, and the audit can check the alignment without
        // having to drive the switch.
        //
        // ---- routing view ----
        routingFrom.setBounds (cellAt (0, 0));

        for (int c = 0; c < routingHeaders.size(); ++c)
            routingHeaders[c]->setBounds (cellAt (0, 1 + c));

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const int row = i + 1;

            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                const auto cell = cellAt (row, 1 + d);

                if (d == i)
                {
                    // The diagonal, wherever this row's lands: the carrier switch.
                    if (i < outCells.size())
                        outCells[i]->setBounds (cell);
                }
                else
                {
                    auto* c = routingCell[static_cast<size_t> (i * MiXXFmVoice::numOps + d)];

                    if (c != nullptr)
                        c->setBounds (cell);
                }
            }
        }

        // ---- values view ----
        if (indexCell != nullptr)
            indexCell->setBounds (cellAt (0, 0));

        for (int c = 0; c < headerIcons.size(); ++c)
            headerIcons[c]->setBounds (cellAt (0, 1 + c).reduced (square / 5));

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const int row = i + 1;

            if (i < opOnToggles.size())
                opOnToggles[i]->setBounds (cellAt (row, 0));

            // Shape, ratio, level, feedback. cells[0] is FM Index, so the operator
            // rows start at 1.
            for (int c = 0; c < 4; ++c)
            {
                const int idx = 1 + i * 4 + c;

                if (idx < cells.size())
                    cells[idx]->setBounds (cellAt (row, 1 + c));
            }
        }

        // ---- the envelope section ----
        // To the right of the grid, past a divider identical to the VA panel's. Its rows use
        // the grid's own originY and pitch, so row n here lines up with row n there - which is
        // the whole reason it is laid out from the same numbers rather than from its own.
        //
        //   row 0:   the page switch, on the grid's header row
        //   row 1+:  one row per operator
        {
            auto sectionCell = [&] (int row, int col, int span)
            {
                return juce::Rectangle<int> (adsrX + col * (sectionCol + gap),
                                             originY + row * (square + gap),
                                             span * sectionCol + (span - 1) * gap, square);
            };

            adsrPages.setBounds (sectionCell (0, 0, 4).reduced (0, 2));

            // Page one's envelopes are three columns wide, like the filter page's envelope, so
            // the two pages' envelopes line up with each other.
            for (int i = 0; i < adsrEditors.size(); ++i)
                adsrEditors[i]->setBounds (sectionCell (i + 1, 0, 3).reduced (1));

            // The modulation matrix takes the columns the volume envelopes leave, across all
            // six operator rows - so the slots are on screen while you work on the
            // oscillator, which is when you actually want them.
            //
            // A viewport rather than a grid of rows: thirty-two slots at this height would
            // need two columns too narrow to read a destination name in, or rows half the
            // height they are. Scrolling shows about a dozen at a time, which is more than
            // most patches use.
            {
                const int modW = (adsrSquares - 3) * sectionCol + (adsrSquares - 4) * gap;

                modViewport.setBounds (adsrX + 3 * (sectionCol + gap),
                                       originY + (square + gap),
                                       modW,
                                       6 * square + 5 * gap);

                // The rows are laid out inside the scrolled content, sized to the viewport so
                // they use its full width rather than a fixed width of their own.
                modContent.setSize (juce::jmax (320, modViewport.getMaximumVisibleWidth()),
                                    mixxynth::kNumModSlots * modRowHeight);

                auto area = modContent.getLocalBounds();

                for (auto* row : modRows)
                    row->setBounds (area.removeFromTop (modRowHeight));

                // The depth column's heading, on the header row above it. Placed from the
                // row's own column widths, measured from the right because that is where the
                // depth column is anchored now - so it cannot drift out of line.
                modDepthHeader.setBounds (modViewport.getX() + modContent.getWidth()
                                            - SlotRow::depthColumnRightOffset(),
                                          originY,
                                          SlotRow::depthColumnWidth(),
                                          square);
            }

            // Page two, in reading order: the filter's envelope, then what the filter is, then
            // where it goes.
            //
            // Both pages are laid out every time rather than only the visible one, for the same
            // reason the grid lays out both its views: the bounds are then always valid
            // whichever page is showing, so a switch before the first resize cannot leave one
            // of them unsized and the audit can check them without driving the switch.
            for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            {
                const int row = i + 1;

                if (i < filterEnvEditors.size())
                    filterEnvEditors[i]->setBounds (sectionCell (row, 0, 3).reduced (1));

                if (i < filterOnCells.size())
                    filterOnCells[i]->setBounds (sectionCell (row, 3, 1).reduced (1));

                if (i < filterTypeBoxes.size())
                    filterTypeBoxes[i]->setBounds (sectionCell (row, 4, 1)
                                                     .reduced (1, juce::jmax (2, square / 5)));

                if (filterCutoffCell[static_cast<size_t> (i)] != nullptr)
                    filterCutoffCell[static_cast<size_t> (i)]
                        ->setBounds (sectionCell (row, 5, 1).reduced (1));

                if (filterResoCell[static_cast<size_t> (i)] != nullptr)
                    filterResoCell[static_cast<size_t> (i)]
                        ->setBounds (sectionCell (row, 6, 1).reduced (1));

                if (filterDriveCell[static_cast<size_t> (i)] != nullptr)
                    filterDriveCell[static_cast<size_t> (i)]
                        ->setBounds (sectionCell (row, 7, 1).reduced (1));

                // The routing switch fills the last square with its three positions.
                if (i * 3 + 2 < routeButtons.size())
                {
                    auto cell = sectionCell (row, 8, 1).reduced (1);
                    const int each = cell.getHeight() / 3;

                    for (int v = 0; v < 3; ++v)
                        routeButtons[i * 3 + v]->setBounds (cell.withHeight (each)
                                                                .translated (0, v * each));
                }
            }
        }

        // ---- the VA panel ----
        // A column three squares wide to the LEFT of the grid. Its header shares the grid's
        // header row, so the two read across as one layout:
        //
        //   [ VA ]              | FM INDEX | icons | ADSR
        //   [shape][sub][noise] | OP 1
        //   [level][det] [oct]  | OP 2
        //   [    envelope    ]  | OP 3
        //   [FILT] [  mode   ]  | OP 4
        //   [cut] [reso][drv]   | OP 5
        //                       | OP 6
        {
            vaHeader.setBounds (originX, originY, vaW, square);

            auto va = juce::Rectangle<int> (originX, originY + square + gap,
                                            vaW, vaRows * square + (vaRows - 1) * gap);

            // Captions are a thin strip rather than a whole square: they name the control
            // below them, so they should not cost as much room as the control itself.
            constexpr int captionHeight = 12;

            // Takes the next square out of the row and advances past it and its gap.
            auto take = [&] (juce::Rectangle<int>& row)
            {
                auto r = row.removeFromLeft (square);
                row.removeFromLeft (gap);
                return r.reduced (1);
            };

            // The next row of the panel.
            auto nextRow = [&]
            {
                auto r = va.removeFromTop (square);
                va.removeFromTop (gap);
                return r;
            };

            // The oscillator: shape, sub, noise, then level, detune, octave. Six controls in
            // a three-wide column is two rows rather than the one they had as a view.
            auto row1 = nextRow();

            for (int i = 0; i < 3; ++i)
                if (vaCells[static_cast<size_t> (i)] != nullptr)
                    vaCells[static_cast<size_t> (i)]->setBounds (take (row1));

            auto row2 = nextRow();

            for (int i = 3; i < 6; ++i)
                if (vaCells[static_cast<size_t> (i)] != nullptr)
                    vaCells[static_cast<size_t> (i)]->setBounds (take (row2));

            // The envelope takes the panel's whole width, with a caption strip above it -
            // the editor draws a graph, so its name has to sit above the graph rather than
            // inside it.
            auto envRow = nextRow();
            vaEnvLabel.setBounds (envRow.removeFromTop (captionHeight).reduced (1, 0));

            if (vaEnvEditor != nullptr)
                vaEnvEditor->setBounds (envRow.reduced (1));

            // The filter's on/off and its mode. The mode is a choice, so it stays a combo
            // box: a cell can hold a value, not a list - and a combo cannot caption itself,
            // so it gets a strip of its own too.
            auto filtRow = nextRow();

            if (! vaToggles.isEmpty())
                vaToggles[0]->setBounds (take (filtRow));

            auto modeCell = filtRow.removeFromLeft (square * 2 + gap);
            vaModeLabel.setBounds (modeCell.removeFromTop (captionHeight).reduced (1, 0));
            vaFilterType.setBounds (modeCell.reduced (1, 2));

            // Cutoff, resonance, drive.
            auto valRow = nextRow();

            for (int i = 6; i < vaOwned.size(); ++i)
                vaOwned[i]->setBounds (take (valRow));
        }
    }

private:
    // A square cell. Registered for modulation exactly like a knob is, because both
    // are ModTargets and the editor makes no distinction between them.
    SquareCell* addCell (juce::RangedAudioParameter* param, int dest, const juce::String& label)
    {
        auto* c = new SquareCell (label, param, dest, true);
        ed.wireModTarget (*c);
        addAndMakeVisible (c);
        ed.modTargets.push_back (c);
        ownedCells.add (c);
        cells.add (c);
        return c;
    }

    // A routing depth cell. Registered for modulation exactly like a value cell is,
    // but kept in its own list so the values layout never sees it.
    SquareCell* addRoutingCell (juce::RangedAudioParameter* param, int dest,
                                const juce::String& label)
    {
        auto* c = new SquareCell (label, param, dest, true);

        // A routing depth of zero is "no connection here", and a black cell already
        // says that unambiguously, so the number is dropped at both ends of the range.
        c->setHideTextAtExtremes (true);

        ed.wireModTarget (*c);
        addChildComponent (c);          // the values view is the one that starts visible
        ed.modTargets.push_back (c);
        routingOwned.add (c);
        return c;
    }

    // A VA-view cell. Registered for modulation like any other, but kept in its own list so
    // the values layout never sees it.
    SquareCell* addVaCell (juce::RangedAudioParameter* param, int dest, const juce::String& label)
    {
        auto* c = new SquareCell (label, param, dest, true);

        // The panel has no column headers, so every one of its cells has to name itself.
        // The grid's cells do not, which is why this is opt-in.
        c->setShowLabel (true);

        ed.wireModTarget (*c);
        addChildComponent (c);
        ed.modTargets.push_back (c);
        vaOwned.add (c);
        return c;
    }

    // A filter-page cell. Registered for modulation like any other, but kept in its own list
    // so the grid layouts never see it.
    SquareCell* addFilterCell (juce::RangedAudioParameter* param, int dest, const juce::String& label)
    {
        auto* c = new SquareCell (label, param, dest, true);

        // This page has no column headers either, so its cells name themselves.
        c->setShowLabel (true);

        ed.wireModTarget (*c);
        addChildComponent (c);
        ed.modTargets.push_back (c);
        filterOwned.add (c);
        return c;
    }

    // The routing switch: three stacked buttons sharing a radio group.
    //
    // Radio buttons rather than three toggles, because the choice IS exclusive - "Out" and
    // "Mod" lit together is "Both", which already has its own button. Three independent
    // switches would invite a state the control cannot actually represent.
    void addRouteButtons (juce::RangedAudioParameter* param, int opIndex)
    {
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param);

        if (choice == nullptr)
            return;

        const int group = opIndex + 1;   // 0 means "no group", so it has to start at 1

        for (int v = 0; v < choice->choices.size(); ++v)
        {
            auto* b = new juce::TextButton (choice->choices[v]);

            b->setClickingTogglesState (true);
            b->setRadioGroupId (group);

            // Joined edges, so the three read as one control with three positions rather than
            // as three buttons that happen to be stacked.
            b->setConnectedEdges ((v > 0 ? juce::Button::ConnectedOnTop : 0)
                                    | (v < choice->choices.size() - 1
                                         ? juce::Button::ConnectedOnBottom : 0));

            b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffa53d));
            b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff101418));
            b->setColour (juce::TextButton::textColourOffId, juce::Colour (0xff8b98a5));
            b->setToggleState (choice->getIndex() == v, juce::dontSendNotification);

            b->onClick = [choice, v]
            {
                choice->beginChangeGesture();
                choice->setValueNotifyingHost (choice->convertTo0to1 (static_cast<float> (v)));
                choice->endChangeGesture();
            };

            addChildComponent (b);
            routeButtons.add (b);
        }
    }

    // Shows one page of the envelope section.
    //
    // The section itself never hides - it is a section, like the VA panel, not a reading of
    // the grid - so only its contents change. That is also why this is separate from
    // applyView: the two switches are independent, and folding them together would couple
    // "which grid view" to "which envelope page" for no reason.
    void applyAdsrPage (int page)
    {
        const bool volume = (page == 0);

        for (auto* e : adsrEditors)
            e->setVisible (volume);

        // Page two. Everything that page owns shows together: a filter envelope without its
        // controls, or controls without their envelope, would be half a page.
        for (auto* e : filterEnvEditors)
            e->setVisible (! volume);

        for (auto* c : filterOnCells)
            c->setVisible (! volume);

        for (auto* b : filterTypeBoxes)
            b->setVisible (! volume);

        for (auto* c : filterOwned)
            c->setVisible (! volume);

        for (auto* b : routeButtons)
            b->setVisible (! volume);

        // The matrix shares the volume page: it takes the columns that page's envelopes
        // leave, and the filter page needs that space for its own controls. The heading goes
        // with it, or the column would be unlabelled on the page it belongs to and labelled
        // on the page it is not on.
        modViewport.setVisible (volume);
        modDepthHeader.setVisible (volume);
    }

    // Shows one view's contents and hides the others'. The operator cells and the toolbar
    // are shared: every view is a reading of the same six operators, so switching views
    // moves nothing.
    void applyView (int v)
    {
        const bool values = (v == 0);
        const bool routing = (v == 1);

        for (auto* t : outCells)
            t->setVisible (routing);

        for (auto* c : routingOwned)
            c->setVisible (routing);

        for (auto* h : routingHeaders)
            h->setVisible (routing);

        routingFrom.setVisible (routing);

        // The operator cells are the shared axis: values and routing both label their rows
        // with them, which is what makes switching between those two views move nothing.
        //
        // They used to be switched off for the VA, back when the VA was a third view of
        // the grid. It is a panel beside the grid now, so there is nothing to switch off
        // for - and this list being forgotten is what put a column of operator boxes on
        // top of the VA content in the first place.
        for (auto* t : opOnToggles)
            t->setVisible (true);

        // indexCell is cells[0], so this covers it.
        for (auto* c : cells)
            c->setVisible (values);

        for (auto* icon : headerIcons)
            icon->setVisible (values);

        // The envelope section is a SECTION like the VA panel, so it is visible in both grid
        // views. What changes is which PAGE it shows, and that is the page switch's business
        // rather than the view switch's - the two are independent.
        applyAdsrPage (adsrPages.getSelected());

        // The VA panel is a SECTION of the tab rather than a reading of the grid, so it
        // is visible in both views. It was a third view itself until the panel moved
        // beside the grid, which is why it used to be gated on the view.
        for (auto* c : vaOwned)
            c->setVisible (true);

        vaFilterType.setVisible (true);

        for (auto* t : vaToggles)
            t->setVisible (true);

        if (vaEnvEditor != nullptr)
            vaEnvEditor->setVisible (true);

        vaHeader.setVisible (true);
        vaEnvLabel.setVisible (true);
        vaModeLabel.setVisible (true);

        resized();
        repaint();
    }

public:
    // Switches view programmatically. The audit uses this to walk every view's controls,
    // which is how an overlap check can see the state a user sees rather than only the
    // one the page happens to open on.
    void setView (int v) { viewSwitch.setSelected (v); }

    int getView() const { return viewSwitch.getSelected(); }

    // The envelope section's page, for the same reason as setView: the audit has to be able
    // to inspect a page it is not currently on, and page two's controls are only laid out -
    // and only overlap-checked - while it is showing.
    void setAdsrPage (int page) { adsrPages.setSelected (page); }

    int getAdsrPage() const { return adsrPages.getSelected(); }

    // The modulation matrix's rows are bound by hand, so they need an explicit refresh.
    // Without this a slot assigned by dragging a chip onto a knob leaves the row still showing
    // Off - the parameters were always correct, the row just never looked again.
    void pollModRows()
    {
        for (auto* row : modRows)
            row->poll();
    }

    juce::String modDebugDump (int slot) const
    {
        if (slot < 0 || slot >= modRows.size())
            return "out of range";

        return modRows[slot]->debugDump();
    }

private:

    MiXXynthEditor& ed;

    // Declared before the controls that use it: members are destroyed in reverse order, so a
    // look and feel has to come first to outlive every combo pointing at it.
    CompactComboLookAndFeel compactComboLook;

    // The modulation matrix's row height. Taller than the old Mod tab's rows were: this list
    // scrolls, so a short row saves nothing, and a slot that looks like the rest of the tab is
    // worth more than two more of them on screen.
    static constexpr int modRowHeight = 44;

    DiceButton diceButton;

    // Snap or Free, beside the die because they are both about what moving a control DOES
    // rather than what it is set to. The label shows the mode you are IN, not the one you
    // would switch to - a switch that names the other state is a switch people misread.
    juce::ToggleButton snapButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> snapAttachment;

    ViewSwitch viewSwitch { juce::StringArray { "Values", "Routing" } };
    juce::Label audibility;

    // The envelope section's own page switch. Two pages rather than three readings of the
    // grid: the volume envelopes and the filter envelopes are the same six operators, but
    // they are not the same numbers.
    ViewSwitch adsrPages { juce::StringArray { "Volume", "Filter" } };
    juce::OwnedArray<GridIcon> headerIcons;

    // cells[0] is FM Index; the operator rows follow as four per operator.
    SquareCell* indexCell = nullptr;

    // One envelope per operator, in operator order. Page one of the envelope section.
    juce::OwnedArray<AdsrEditor> adsrEditors;

    // ---- page two: the per-operator filters ---------------------------------
    // Laid out across the section in reading order: the filter's own envelope, then what the
    // filter is, then where it goes.
    juce::OwnedArray<AdsrEditor> filterEnvEditors;
    juce::OwnedArray<OperatorCell> filterOnCells;
    juce::OwnedArray<juce::ComboBox> filterTypeBoxes;
    juce::OwnedArray<SquareCell> filterOwned;
    std::array<SquareCell*, MiXXFmVoice::numOps> filterCutoffCell {};
    std::array<SquareCell*, MiXXFmVoice::numOps> filterResoCell {};
    std::array<SquareCell*, MiXXFmVoice::numOps> filterDriveCell {};

    // Three stacked buttons per operator, sharing a radio group so they behave as one choice
    // rather than as three switches.
    juce::OwnedArray<juce::TextButton> routeButtons;

    // ---- the modulation matrix, in the envelope section ----------------------
    // It lives here rather than on a tab of its own so the slots are visible while you work
    // on the oscillator - which is when you actually want to see them. It shares the envelope
    // section's page switch, so it is on the volume page and the filter page has the section
    // to itself.
    juce::Viewport modViewport;
    juce::Component modContent;
    juce::OwnedArray<SlotRow> modRows;

    // One heading for the depth column, rather than the word "Depth" on thirty-two knobs.
    juce::Label modDepthHeader;

    // ---- the routing view -------------------------------------------------
    // One cell per ordered operator pair, so the grid is 6x6 with no holes: the 30
    // off-diagonal cells are routing depths and the 6 diagonal cells are the carrier
    // switches. Self-modulation is the per-operator feedback parameter rather than
    // mtxNN, which is exactly why the diagonal is free to mean something else.
    //
    // The diagonal cells are switches, not depth sliders, so they cannot be mistaken
    // for the 30 cells around them even before their label is read.
    juce::Label routingFrom;
    juce::OwnedArray<juce::Label> routingHeaders;
    juce::OwnedArray<OperatorCell> outCells;
    juce::OwnedArray<SquareCell> routingOwned;
    std::array<SquareCell*, MiXXFmVoice::numOps * MiXXFmVoice::numOps> routingCell {};

    // ---- the VA view --------------------------------------------------------
    // The VA oscillator and the per-voice filter, which had a tab of their own until
    // reducing the tab count became worth doing.
    juce::OwnedArray<SquareCell> vaOwned;
    std::array<SquareCell*, 6> vaCells {};   // shape, sub, noise, level, detune, octave
    juce::ComboBox vaFilterType;
    juce::OwnedArray<OperatorCell> vaToggles;  // the filter on/off
    juce::OwnedArray<AdsrEditor> vaEnvOwned;
    AdsrEditor* vaEnvEditor = nullptr;
    juce::Label vaHeader;
    juce::Label vaEnvLabel;
    juce::Label vaModeLabel;

    // The two bars, in this component's own coordinates. Set by resized() because paint()
    // has no access to the layout arithmetic.
    //
    // Both dividers, not one: the grid is flanked symmetrically, and a change to the spacing
    // or the width applies to both or it is not symmetry.
    int dividerX = 0, divider2X = 0, dividerTop = 0, dividerBottom = 0;

    // Two pixels, not one: a hairline at this scale reads as an accident rather than as a
    // deliberate break, which is the opposite of what a section divider is for.
    static constexpr int dividerWidth = 2;

    juce::Random rng;
    juce::OwnedArray<SquareCell> ownedCells;
    juce::Array<SquareCell*> cells;
    juce::OwnedArray<OperatorCell> opOnToggles;
};

// ---------------------------------------------------------------------------
// FX page: the rack (M4b workstream 4)
//
// A card per live slot, in chain order. Slots are a fixed pool of eight with a
// per-slot type, so "add" means filling the first free slot rather than growing
// anything, and the chain always stays dense - which is what lets the arrows
// reorder by swapping one adjacent pair.
// ---------------------------------------------------------------------------
class MiXXynthEditor::FxPage : public juce::Component
{
public:
    explicit FxPage (MiXXynthEditor& e) : ed (e)
    {
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        addButton.setButtonText ("+  Add a module");
        addButton.setTooltip ("Append a module to the end of the chain");
        addButton.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (addButton);

        empty.setText ("The rack is empty. Modules run top to bottom, so slot 1 is first "
                       "in the chain and slot 8 is last.",
                       juce::dontSendNotification);
        empty.setJustificationType (juce::Justification::centredLeft);
        empty.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
        content.addAndMakeVisible (empty);

        rebuild();
    }

    // One module in the rack: title, bypass, the two arrows, remove, and the
    // controls the module's descriptor declares.
    class Card : public juce::Component,
                 private juce::Timer
    {
    public:
        Card (FxPage& o, int slotIndex) : owner (o), slot (slotIndex)
        {
            auto& p = o.ed.processor();
            const auto state = p.readFxSlot (slot);
            const auto desc = mixxynth::FxRack::descriptorFor (state.type);

            // A member rather than a local, because the graph's timer needs it long after the
            // constructor has returned.
            prefix = "fx" + juce::String (slot + 1);

            title.setText (desc.name, juce::dontSendNotification);
            title.setColour (juce::Label::textColourId, juce::Colour (0xff35e0ff));
            title.setFont (juce::FontOptions (14.0f));
            addAndMakeVisible (title);

            up.setButtonText ("^");
            up.setTooltip ("Move one step earlier in the chain");
            down.setButtonText ("v");
            down.setTooltip ("Move one step later in the chain");
            remove.setButtonText ("-");
            remove.setTooltip ("Remove this module");

            // An arrow that cannot act is disabled rather than silently doing
            // nothing, so the chain's ends are visible.
            up.setEnabled (slot > 0);
            down.setEnabled (slot + 1 < mixxynth::kMaxFxSlots
                               && ! p.readFxSlot (slot + 1).isEmpty());

            up.onClick = [this] { owner.moveModule (slot, -1); };
            down.onClick = [this] { owner.moveModule (slot, 1); };
            remove.onClick = [this] { owner.removeModule (slot); };

            addAndMakeVisible (up);
            addAndMakeVisible (down);
            addAndMakeVisible (remove);

            bypass.setButtonText ("bypass");
            bypass.setTooltip ("Bypass this module");

            if (auto* b = dynamic_cast<juce::AudioParameterBool*> (
                    p.apvts.getParameter (prefix + "bypass")))
                bypassAttachment = std::make_unique<juce::ButtonParameterAttachment> (*b, bypass);

            addAndMakeVisible (bypass);

            for (int pi = 0; pi < desc.numParams && pi < mixxynth::kMaxFxParams; ++pi)
            {
                const auto& info = desc.params[static_cast<size_t> (pi)];
                const juce::String id = prefix + "p" + juce::String (pi + 1);

                if (info.isChoice)
                {
                    auto* box = new juce::ComboBox();
                    box->addItemList (info.choices, 1);
                    box->setTooltip (info.name);

                    // The rack's slot parameters are all generic floats, so a
                    // choice is stored as index / (numItems - 1) and read back by
                    // rounding - which is exactly how the module interprets it.
                    // ComboBoxParameterAttachment needs a real
                    // AudioParameterChoice, and these deliberately are not one, so
                    // the binding is done by hand.
                    //
                    // A module may declare more than one choice (the reverb has
                    // Mode and Freeze), so these are lists rather than single
                    // members.
                    const int boxIndex = choiceBoxes.size();

                    box->onChange = [this, boxIndex]
                    {
                        auto* param = choiceParams[boxIndex];

                        if (param == nullptr || choiceCounts[boxIndex] < 2)
                            return;

                        const float v = static_cast<float> (choiceBoxes[boxIndex]->getSelectedItemIndex())
                                          / static_cast<float> (choiceCounts[boxIndex] - 1);

                        param->beginChangeGesture();
                        param->setValueNotifyingHost (param->convertTo0to1 (v));
                        param->endChangeGesture();
                    };

                    choiceBoxes.add (box);
                    choiceParams.add (p.apvts.getParameter (id));
                    choiceCounts.add (info.choices.size());

                    addAndMakeVisible (*box);
                    box->setSelectedItemIndex (indexFromParam (boxIndex), juce::dontSendNotification);
                    continue;
                }

                // Dest Off: the rack's generic params are not in the matrix's
                // destination list, so these knobs are not modulation targets and
                // a dropped chip is rejected with a flash rather than ignored.
                auto* k = new ModKnob (info.name, p.apvts.getParameter (id),
                                       mixxynth::dstOff, true);
                owner.ed.wireModTarget (*k);
                addAndMakeVisible (k);
                owned.add (k);
                knobs.add (k);
            }

            // A module that declares a graph gets a curve above its knobs, drawn from the
            // same coefficients the audio path uses so the picture cannot drift from the
            // sound.
            if (desc.drawGraph != nullptr)
            {
                hasGraphFn = true;
                graphFn = desc.drawGraph;
                graphLabel = desc.graphLabel;
                startTimerHz (20);
            }

            // A module that needs a file gets a button and a name, not a knob. The
            // file is state rather than a parameter, so it cannot be one.
            if (desc.hasFile)
            {
                loadFile.setButtonText (desc.fileLabel);
                loadFile.setTooltip ("Load an impulse response from a file");
                loadFile.onClick = [this] { owner.chooseFileFor (slot); };
                addAndMakeVisible (loadFile);

                fileName.setJustificationType (juce::Justification::centredLeft);
                fileName.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
                addAndMakeVisible (fileName);

                hasFileControl = true;
            }
        }

        // Per-card rather than static, because a module with a graph is taller than one
        // without and the page has to ask each card rather than assume they match.
        int preferredHeight() const
        {
            return 26 + 4 + (choiceBoxes.isEmpty() && ! hasFileControl ? 0 : 30)
                     + (hasGraphFn ? mixxynth::FxDescriptor::graphHeight + 6 : 0) + 78 + 10;
        }

        // Keeps the controls honest when a parameter moves from anywhere else -
        // a preset load, or the host.
        // The generic param stores a choice as index / (numItems - 1).
        int indexFromParam (int boxIndex) const
        {
            auto* param = choiceParams[boxIndex];
            const int count = choiceCounts[boxIndex];

            if (param == nullptr || count < 2)
                return 0;

            const float v = param->convertFrom0to1 (param->getValue());

            return juce::jlimit (0, count - 1,
                                 static_cast<int> (std::lround (v * static_cast<float> (count - 1))));
        }

        void poll()
        {
            for (auto* k : knobs)
                k->refreshFromParam();

            // The combos are bound by hand, so they need the same refresh.
            for (int b = 0; b < choiceBoxes.size(); ++b)
            {
                const int idx = indexFromParam (b);

                if (choiceBoxes[b]->getSelectedItemIndex() != idx)
                    choiceBoxes[b]->setSelectedItemIndex (idx, juce::dontSendNotification);
            }

            if (hasFileControl)
            {
                const auto name = owner.ed.processor().getFxSlotFileName (slot);
                const auto shown = name.isEmpty() ? juce::String ("no file") : name;

                if (fileName.getText() != shown)
                    fileName.setText (shown, juce::dontSendNotification);
            }
        }

        // For the audit: sets a combo through the same path a click takes, so the
        // write back to the parameter is exercised rather than assumed.
        void setChoiceIndex (int boxIndex, int index)
        {
            if (juce::isPositiveAndBelow (boxIndex, choiceBoxes.size()))
                choiceBoxes[boxIndex]->setSelectedItemIndex (index, juce::sendNotificationSync);
        }

        // For the audit: what this card built, and whether any control ended up
        // with no size.
        juce::String debugDump() const
        {
            int zeros = 0;

            for (auto* c : getChildren())
                if (c->isVisible() && (c->getWidth() <= 0 || c->getHeight() <= 0))
                    ++zeros;

            juce::String s;
            s << "slot=" << (slot + 1)
              << " type=" << mixxynth::fxTypeName (owner.ed.processor().readFxSlot (slot).type)
              << " combos=" << choiceBoxes.size()
              << " knobs=" << knobs.size()
              << " file=" << (hasFileControl ? 1 : 0)
              << " zeros=" << zeros;

            if (! choiceBoxes.isEmpty())
                s << " comboItems=" << choiceBoxes[0]->getNumItems()
                  << " comboIdx=" << choiceBoxes[0]->getSelectedItemIndex()
                  << " comboText=" << choiceBoxes[0]->getText();

            return s;
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (1.0f);

            g.setColour (juce::Colour (0xff1b2128));
            g.fillRoundedRectangle (b, 4.0f);

            g.setColour (juce::Colour (0xff2f3a45));
            g.drawRoundedRectangle (b, 4.0f, 1.0f);

            // The slot number, so the chain order is readable at a glance.
            g.setColour (juce::Colour (0xff5d6b78));
            g.setFont (11.0f);
            g.drawText ("slot " + juce::String (slot + 1),
                        getLocalBounds().removeFromRight (56).reduced (6, 4),
                        juce::Justification::centredRight, false);

            if (hasGraphFn && ! graphArea.isEmpty())
                drawGraphCurve (g);
        }

    private:
        // The response curve, drawn from the module's own transfer function rather than from
        // an impression of one.
        //
        // Log frequency, because that is how the ear is arranged and how every EQ is drawn;
        // a linear axis would put nine of the ten bands in the right-hand fifth of the plot.
        void drawGraphCurve (juce::Graphics& g)
        {
            auto area = graphArea.toFloat();

            g.setColour (juce::Colour (0xff0d1117));
            g.fillRoundedRectangle (area, 3.0f);

            // The same +/-18 dB window the module's controls span, so a band at its limit
            // reaches the edge of the plot rather than somewhere arbitrary.
            constexpr float rangeDb = 18.0f;
            const float midY = area.getCentreY();
            const float halfH = area.getHeight() * 0.5f - 4.0f;

            g.setColour (juce::Colour (0xff222a33));
            g.drawHorizontalLine (static_cast<int> (midY), area.getX() + 2.0f, area.getRight() - 2.0f);

            // Read the parameters fresh. The knobs are separate components, so the card has
            // no way to be told one moved - it polls instead, which is the same 20 Hz poll
            // the rest of the editor uses.
            std::array<float, mixxynth::kMaxFxParams> values {};

            for (int p = 0; p < mixxynth::kMaxFxParams; ++p)
                if (auto* param = owner.ed.processor().apvts.getParameter (
                        prefix + "p" + juce::String (p + 1)))
                    values[static_cast<size_t> (p)] = param->getValue();

            juce::Path path;

            const int steps = juce::jmax (2, static_cast<int> (area.getWidth()) / 2);

            for (int i = 0; i < steps; ++i)
            {
                const float t = static_cast<float> (i) / static_cast<float> (steps - 1);

                // 20 Hz to 20 kHz, exponentially.
                const float hz = 20.0f * std::pow (1000.0f, t);

                const float db = graphFn (values.data(), hz, owner.ed.processor().getSampleRate());

                const float x = area.getX() + 2.0f + t * (area.getWidth() - 4.0f);
                const float y = midY - juce::jlimit (-rangeDb, rangeDb, db) / rangeDb * halfH;

                if (i == 0)
                    path.startNewSubPath (x, y);
                else
                    path.lineTo (x, y);
            }

            g.setColour (juce::Colour (0xffffa63d));
            g.strokePath (path, juce::PathStrokeType (1.4f));

            g.setColour (juce::Colour (0xff5d6b78));
            g.setFont (9.0f);
            g.drawText (graphLabel, area.reduced (5.0f, 2.0f), juce::Justification::topLeft);
        }

        void timerCallback() override { repaint(); }

        bool hasGraphFn = false;
        float (*graphFn) (const float*, float, double) = nullptr;
        juce::String graphLabel;
        juce::String prefix;
        juce::Rectangle<int> graphArea;

        void resized() override
        {
            auto b = getLocalBounds().reduced (6);
            auto top = b.removeFromTop (24);

            title.setBounds (top.removeFromLeft (110));
            remove.setBounds (top.removeFromRight (26).reduced (0, 1));
            down.setBounds (top.removeFromRight (26).reduced (0, 1));
            up.setBounds (top.removeFromRight (26).reduced (0, 1));
            top.removeFromRight (56); // the slot number is painted, not a control
            bypass.setBounds (top.removeFromRight (78).reduced (0, 1));

            b.removeFromTop (4);

            if (hasGraphFn)
            {
                graphArea = b.removeFromTop (mixxynth::FxDescriptor::graphHeight);
                b.removeFromTop (6);
            }

            // Choices get their own row: a module can have several, and sharing
            // the knob row would leave the knobs too narrow to use.
            if (! choiceBoxes.isEmpty() || hasFileControl)
            {
                auto comboRow = b.removeFromTop (30);

                // The file button and its name take the right of the row, so they sit
                // beside the module's own choices rather than pushing them around.
                if (hasFileControl)
                {
                    loadFile.setBounds (comboRow.removeFromRight (120).reduced (2, 3));
                    comboRow.removeFromRight (6);
                    fileName.setBounds (comboRow.removeFromRight (200));
                    comboRow.removeFromRight (6);
                }

                if (! choiceBoxes.isEmpty())
                {
                    const int comboW = juce::jmin (150, comboRow.getWidth() / choiceBoxes.size());

                    for (auto* box : choiceBoxes)
                        box->setBounds (comboRow.removeFromLeft (comboW).reduced (2, 3));
                }
            }

            if (! knobs.isEmpty())
            {
                // Leave room for the painted slot number, or the last knob ends up
                // sitting underneath it.
                b.removeFromRight (56);

                const int colW = juce::jmax (54, b.getWidth() / knobs.size());

                for (auto* k : knobs)
                    k->setBounds (b.removeFromLeft (colW).reduced (2));
            }
        }

    private:
        FxPage& owner;
        int slot = 0;

        juce::Label title;
        juce::ToggleButton bypass;
        juce::TextButton up, down, remove;

        std::unique_ptr<juce::ButtonParameterAttachment> bypassAttachment;

        juce::OwnedArray<juce::ComboBox> choiceBoxes;
        juce::Array<juce::RangedAudioParameter*> choiceParams;
        juce::Array<int> choiceCounts;

        // Only used when the module declares a file.
        bool hasFileControl = false;
        juce::TextButton loadFile;
        juce::Label fileName;

        juce::OwnedArray<ModKnob> owned;
        juce::Array<ModKnob*> knobs;
    };

public:
    // rebuild() is public so the editor can drive it from the audit; the page
    // itself only ever reaches it through requestRebuild or poll.
    juce::String typeSignature() const
    {
        juce::String s;

        for (int i = 0; i < mixxynth::kMaxFxSlots; ++i)
            s << ed.processor().readFxSlot (i).type << ",";

        return s;
    }

    // Rebuilds the card list from the current slot types.
    //
    // Never call this from inside a card's own callback - the card would be
    // deleted under the callback that is running - so use requestRebuild for
    // anything a card triggers.
    void rebuild()
    {
        cards.clear();
        content.removeAllChildren();
        content.addAndMakeVisible (empty);

        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
        {
            if (ed.processor().readFxSlot (s).isEmpty())
                continue;

            // OwnedArray owns it; it does NOT parent it. Forgetting this leaves
            // the card constructed, sized, and completely invisible.
            auto* card = new Card (*this, s);
            content.addAndMakeVisible (*card);
            cards.add (card);
        }

        empty.setVisible (cards.isEmpty());
        signature = typeSignature();

        resized();
    }

    void requestRebuild()
    {
        juce::Component::SafePointer<FxPage> safe (this);

        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->rebuild();
        });
    }

public:
    // Called from the editor's timer. Picks up type changes that did not come
    // from this page - a preset load, or a host restoring state - and otherwise
    // just refreshes the knobs.
    void poll()
    {
        if (typeSignature() != signature)
        {
            // Never rebuild under a mouse gesture: the card being dragged would
            // be deleted mid-drag. The next tick picks it up instead.
            if (juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown())
                return;

            rebuild();
            return;
        }

        for (auto* c : cards)
            c->poll();
    }

    void showAddMenu()
    {
        juce::PopupMenu menu;

        for (int t = 1; t < mixxynth::kNumFxTypes; ++t)
            menu.addItem (t, mixxynth::fxTypeName (t));

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton),
                            [this] (int result)
                            {
                                if (result > 0)
                                    addModule (result);
                            });
    }

    void addModule (int type)
    {
        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
        {
            if (! ed.processor().readFxSlot (s).isEmpty())
                continue;

            const auto desc = mixxynth::FxRack::descriptorFor (type);

            mixxynth::FxSlotState state;
            state.type = type;

            // The parameter defaults cannot depend on a type the user can change,
            // so the module's own defaults are applied here, at the moment the
            // type is picked. Every new module therefore starts on a usable sound
            // rather than at zero.
            for (int p = 0; p < desc.numParams && p < mixxynth::kMaxFxParams; ++p)
                state.params[static_cast<size_t> (p)] = desc.params[static_cast<size_t> (p)].defaultValue;

            ed.processor().writeFxSlot (s, state);
            ed.setModStatus (juce::String (desc.name) + " added in slot " + juce::String (s + 1));

            requestRebuild();
            return;
        }

        ed.setModStatus ("The rack is full - eight modules is the maximum");
    }

    void removeModule (int slot)
    {
        auto& p = ed.processor();

        // Compact rather than leaving a hole. The chain being dense is what makes
        // "add fills the first free slot" mean "append to the end", and what makes
        // the arrows a plain adjacent swap. A hole would silently break both.
        for (int s = slot; s < mixxynth::kMaxFxSlots - 1; ++s)
            p.writeFxSlot (s, p.readFxSlot (s + 1));

        p.writeFxSlot (mixxynth::kMaxFxSlots - 1, mixxynth::FxSlotState {});

        ed.setModStatus ("Module removed from slot " + juce::String (slot + 1));

        requestRebuild();
    }

    // Swaps this slot with its neighbour. Because the chain is always dense,
    // swapping an adjacent pair IS a reorder - and every setting travels with the
    // module, because a whole slot's state moves at once.
    void moveModule (int slot, int delta)
    {
        const int target = slot + delta;

        if (target < 0 || target >= mixxynth::kMaxFxSlots)
            return;

        auto& p = ed.processor();
        const auto here = p.readFxSlot (slot);

        if (here.isEmpty())
            return;

        const auto there = p.readFxSlot (target);

        p.writeFxSlot (slot, there);
        p.writeFxSlot (target, here);

        ed.setModStatus (juce::String (mixxynth::FxRack::descriptorFor (here.type).name)
                           + (delta < 0 ? " moved earlier" : " moved later"));

        requestRebuild();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);

        addButton.setBounds (b.removeFromBottom (26).removeFromLeft (180));
        b.removeFromBottom (6);

        viewport.setBounds (b);

        const int w = juce::jmax (340, viewport.getMaximumVisibleWidth());

        int y = 0;

        for (auto* c : cards)
        {
            // Each card reports its own height: a module with a graph is taller than one
            // without, and assuming they match would overlap them.
            const int cardH = c->preferredHeight();
            c->setBounds (0, y, w, cardH);
            y += cardH + 6;
        }

        empty.setBounds (0, 0, w, 44);
        content.setSize (w, juce::jmax (44, y));
    }

    // Opens a file picker for a slot whose module wants a file, and hands the bytes
    // to the processor. Async, because a modal file chooser blocks the message
    // thread and this is called from a button click inside a card.
    void chooseFileFor (int slot)
    {
        auto& p = ed.processor();
        const auto state = p.readFxSlot (slot);
        const auto desc = mixxynth::FxRack::descriptorFor (state.type);

        if (! desc.hasFile)
            return;

        fileChooser = std::make_unique<juce::FileChooser> ("Load " + juce::String (desc.fileLabel),
                                                           juce::File(),
                                                           juce::String (desc.fileFilter));

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this, slot] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();

            if (! file.existsAsFile())
                return;

            juce::MemoryBlock data;

            if (! file.loadFileAsData (data))
                return;

            ed.processor().setFxSlotFile (slot, file.getFileName(), data);
            ed.setModStatus (file.getFileName() + " loaded into slot "
                               + juce::String (slot + 1));

            // The module picks the new impulse up on its own timer, but the card's
            // label should not wait for it.
            if (slot >= 0 && slot < cards.size())
                cards[slot]->poll();
        });
    }

    // For the audit: drives a card's choice combo, so the audit exercises the
    // same code path a click takes.
    void setCardChoice (int cardIndex, int boxIndex, int index)
    {
        if (cardIndex >= 0 && cardIndex < cards.size())
            cards[cardIndex]->setChoiceIndex (boxIndex, index);
    }

    // For the audit.
    juce::String debugDump() const
    {
        // A card can exist and be sized while never being parented, which makes
        // it invisible - so report that separately rather than assuming that
        // "a card exists" means "a card is on screen".
        int parented = 0;

        for (auto* c : cards)
            if (c->getParentComponent() == &content && c->isVisible()
                  && c->getWidth() > 0 && c->getHeight() > 0)
                ++parented;

        juce::String s;
        s << "cards=" << cards.size()
          << " parented=" << parented
          << " addVisible=" << (addButton.isVisible() ? 1 : 0)
          << " emptyVisible=" << (empty.isVisible() ? 1 : 0);

        for (auto* c : cards)
            s << " [" << c->debugDump() << "]";

        return s;
    }

private:
    MiXXynthEditor& ed;

    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<Card> cards;
    juce::TextButton addButton;
    juce::Label empty;

    // Type list at the last rebuild, so a change from anywhere is noticed.
    juce::String signature;

    // Held so the async file picker is not destroyed while it is open.
    std::unique_ptr<juce::FileChooser> fileChooser;
};

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------
// The output oscilloscope.
//
// TRIGGERED, which is the difference between a scope and a blur. Without a trigger the
// waveform is redrawn from an arbitrary point each frame, so a steady tone looks like it is
// sliding sideways and anything with a slow envelope looks like noise. It waits for the
// signal to cross zero going up and starts the trace there, so a steady note stands still.
//
// FIXED SCALE, not auto-gain. An auto-scaled scope always looks the same, which is exactly
// the thing you do not want from a scope on a synth: the whole reason to have one is to see
// that a patch is quiet, or that it is hitting the limiter. The +/-1 lines are drawn so
// full scale is a visible landmark rather than an assumption.
class MiXXynthEditor::ScopeComponent : public juce::Component,
                                       private juce::Timer
{
public:
    explicit ScopeComponent (MiXXynthProcessor& p) : proc (p)
    {
        samples.resize (static_cast<size_t> (MiXXynthProcessor::kScopeSize), 0.0f);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff0d1117));
        g.fillRoundedRectangle (area, 3.0f);

        auto plot = area.reduced (6.0f, 4.0f);
        const float midY = plot.getCentreY();

        // The zero line, and the +/-1 lines. Full scale is a landmark rather than an
        // assumption, which is the point of not auto-scaling.
        g.setColour (juce::Colour (0xff222a33));
        g.drawHorizontalLine (static_cast<int> (midY), plot.getX(), plot.getRight());

        g.setColour (juce::Colour (0xff2f3a45));
        g.drawHorizontalLine (static_cast<int> (plot.getY()), plot.getX(), plot.getRight());
        g.drawHorizontalLine (static_cast<int> (plot.getBottom()), plot.getX(), plot.getRight());

        const int n = static_cast<int> (samples.size());

        if (n < 2 || ! hasSignal)
            return;

        // The trigger: the first upward zero crossing in the first half of the window, so
        // the trace has room to run. If there is none the window is drawn as it is, which
        // is what happens on silence or on a DC-ish signal.
        int start = 0;

        for (int i = 1; i < n / 2; ++i)
        {
            if (samples[static_cast<size_t> (i - 1)] <= 0.0f && samples[static_cast<size_t> (i)] > 0.0f)
            {
                start = i;
                break;
            }
        }

        const int shown = juce::jmin (n - start, n);

        juce::Path path;
        const float scaleY = plot.getHeight() * 0.5f;

        for (int i = 0; i < shown; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * static_cast<float> (i)
                                            / static_cast<float> (juce::jmax (1, shown - 1));
            const float y = midY - juce::jlimit (-1.0f, 1.0f, samples[static_cast<size_t> (start + i)])
                                     * scaleY;

            if (i == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        g.setColour (juce::Colour (0xff35e0ff));
        g.strokePath (path, juce::PathStrokeType (1.2f));
    }

private:
    void timerCallback() override
    {
        const int n = proc.readScope (samples.data(), static_cast<int> (samples.size()));

        // Whether there is anything to draw. A scope that keeps showing the last thing it
        // saw is worse than one that shows a flat line, because it looks live.
        hasSignal = false;

        for (int i = 0; i < n; ++i)
            if (std::abs (samples[static_cast<size_t> (i)]) > 1.0e-5f)
            {
                hasSignal = true;
                break;
            }

        repaint();
    }

    MiXXynthProcessor& proc;
    std::vector<float> samples;
    bool hasSignal = false;
};

juce::Component* MiXXynthEditor::getScopeComponent() const
{
    return scope.get();
}

//
// It exists because a safety limiter is only trustworthy if you can see that it is NOT
// working. A meter that sits at zero confirms the threshold is doing nothing on ordinary
// material; one that flickers tells you the patch is running hot and the limiter is
// earning its place. Without it, "is my limiter eating transients?" is unanswerable.
class MiXXynthEditor::LimiterMeter : public juce::Component
{
public:
    void setGainReduction (float db) noexcept
    {
        // Smoothed for the eye. The audio-thread value jumps; a meter that jumped with it
        // would flicker rather than read.
        current = juce::jmax (db, current - 1.5f);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff141414));
        g.fillRoundedRectangle (area, 3.0f);

        // The scale is 12 dB, which is about the range a safety limiter should ever be
        // showing. Beyond that the number stops being informative and starts being an
        // alarm.
        constexpr float range = 12.0f;

        const float amount = juce::jlimit (0.0f, 1.0f, -current / range);

        if (amount > 0.001f)
        {
            auto fill = area.reduced (1.0f);
            fill = fill.removeFromLeft (fill.getWidth() * amount);

            // Orange while it is working, because orange is the value colour everywhere
            // else in this editor.
            g.setColour (juce::Colour (0xffe08a3c));
            g.fillRoundedRectangle (fill, 2.0f);
        }

        g.setColour (juce::Colour (0xff5a6570));
        g.setFont (juce::FontOptions (9.0f));

        g.drawText (current < -0.05f ? juce::String (current, 1) + " dB" : juce::String ("GR"),
                    getLocalBounds(), juce::Justification::centred);
    }

private:
    float current = 0.0f;
};


MiXXynthEditor::MiXXynthEditor (MiXXynthProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    mixxynth::ui::initialiseWidgetStyles();

    setResizable (true, true);
    setResizeLimits (900, 640, 2560, 1440);
    // NOTE: setSize is deliberately called at the END of this constructor.
    // Calling it first would run resized() while the child arrays are still
    // empty, leaving the chips and macro knobs with zero bounds.

    title.setText ("MiXXynth", juce::dontSendNotification);
    title.setJustificationType (juce::Justification::centredLeft);
    title.setColour (juce::Label::textColourId, juce::Colour (0xff35e0ff));
    title.setFont (juce::FontOptions (22.0f));
    addAndMakeVisible (title);

    railTitle.setText ("MODULATION", juce::dontSendNotification);
    railTitle.setJustificationType (juce::Justification::centredLeft);
    railTitle.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    rail.addAndMakeVisible (railTitle);
    addAndMakeVisible (rail);

    // The sidebar is a scrollable stack of editable modules, one per LFO and ENV,
    // plus the buttons that add and remove them.
    rail.addAndMakeVisible (modViewport);
    modViewport.setViewedComponent (&modContent, false);
    modViewport.setScrollBarsShown (true, false);

    // ---- voice section ----
    // Moved out of a tab of its own and into the sidebar, above the modulation section.
    // It scrolls with the modules rather than being pinned, so the two sections read as
    // one list and neither can be scrolled out of reach.
    //
    // The MODULATION caption moves in here too: with a section above it, a caption pinned
    // outside the scroll would label the wrong thing the moment the list scrolled.
    modContent.addAndMakeVisible (railTitle);

    voiceCaption.setText ("VOICE", juce::dontSendNotification);
    voiceCaption.setJustificationType (juce::Justification::centredLeft);
    voiceCaption.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    modContent.addAndMakeVisible (voiceCaption);

    for (const auto& spec : { std::pair<const char*, const char*> { "polyphony", "Polyphony" },
                              { "unison", "Unison" },
                              { "unisonDetune", "Unison Detune" },
                              { "unisonSpread", "Unison Spread" },
                              { "unisonPhaseRand", "Phase Random" },
                              { "pitchBendRange", "Bend Range" },
                              { "drift", "Drift" },
                              { "velToLevel", "Vel to Level" },
                              { "velToIndex", "Vel to Index" } })
    {
        // Voice settings are not modulation destinations, so every one of these is dstOff
        // - which is also what makes a dropped chip on one flash red rather than silently
        // doing nothing.
        auto* k = new ModKnob (spec.second, ranged (p, spec.first), mixxynth::dstOff, true);
        modContent.addAndMakeVisible (k);
        modTargets.push_back (k);
        voiceKnobs.add (k);
    }

    mpeToggle = makeBoolToggle (p, "mpeEnabled", "MPE");
    modContent.addAndMakeVisible (mpeToggle);

    mpeChannels = new juce::Slider (juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft);
    mpeChannels->setRange (1.0, 15.0, 1.0);
    modContent.addAndMakeVisible (mpeChannels);

    if (auto* param = dynamic_cast<juce::AudioParameterInt*> (ranged (p, "mpeMemberChannels")))
    {
        mpeChannels->setValue (param->get(), juce::dontSendNotification);
        mpeChannels->onValueChange = [this, param]
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (
                param->convertTo0to1 (static_cast<float> (mpeChannels->getValue())));
            param->endChangeGesture();
        };
    }

    channelsLabel.setText ("MPE channels", juce::dontSendNotification);
    channelsLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    modContent.addAndMakeVisible (channelsLabel);

    addLfoButton.setButtonText ("+ LFO");
    addLfoButton.onClick = [this] { addShapeModule (true); };
    modContent.addAndMakeVisible (addLfoButton);

    addEnvButton.setButtonText ("+ ENV");
    addEnvButton.onClick = [this] { addShapeModule (false); };
    modContent.addAndMakeVisible (addEnvButton);

    rebuildShapeStrips();

    // Performance sources keep the chip workflow: macros plus the MIDI inputs.
    perfChipTitle.setText ("PERFORMANCE  (drag onto a knob)", juce::dontSendNotification);
    perfChipTitle.setJustificationType (juce::Justification::centredLeft);
    perfChipTitle.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    perfChipTitle.setFont (juce::FontOptions (10.5f));
    modContent.addAndMakeVisible (perfChipTitle);

    for (int i = mixxynth::srcMacro1; i < mixxynth::kNumModSources; ++i)
    {
        auto* c = new SourceChip (mixxynth::modSourceNames()[i], i);
        chips.add (c);
        modContent.addAndMakeVisible (c);
    }

    modStatus.setText ("Drag a shape's chip onto any knob to modulate it.", juce::dontSendNotification);
    modStatus.setJustificationType (juce::Justification::centredLeft);
    modStatus.setColour (juce::Label::textColourId, juce::Colour (0xff7c8a99));
    modStatus.setFont (juce::FontOptions (10.5f));
    rail.addAndMakeVisible (modStatus);

    barTitle.setText ("MACROS", juce::dontSendNotification);
    barTitle.setJustificationType (juce::Justification::centredLeft);
    barTitle.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    bar.addAndMakeVisible (barTitle);
    addAndMakeVisible (bar);

    for (int i = 0; i < mixxynth::kNumMacros; ++i)
    {
        auto* k = new ModKnob ("Macro " + juce::String (i + 1),
                               ranged (proc, "macro" + juce::String (i + 1)),
                               mixxynth::dstOff, true);
        bar.addAndMakeVisible (k);
        macroKnobs.add (k);
        modTargets.push_back (k);
    }

    auto* master = new ModKnob ("Master", ranged (proc, "masterGain"), mixxynth::dstMaster, true);
    wireModTarget (*master);
    bar.addAndMakeVisible (master);
    modTargets.push_back (master);
    macroKnobs.add (master); // owned alongside the macros

    // The output limiter, beside Master because it is the next thing in the signal path.
    //
    // The release is a knob rather than a hidden constant because it is the only part of
    // a limiter that is a matter of taste; the threshold is not adjustable, and that is
    // deliberate - a safety limiter whose threshold can be moved stops being a safety
    // limiter and becomes an effect.
    limiterKnob = new ModKnob ("Release", ranged (proc, "limiterRelease"),
                               mixxynth::dstOff, true);
    bar.addAndMakeVisible (limiterKnob);
    macroKnobs.add (limiterKnob); // owned alongside the macros

    // Registered like every other knob. modTargets is the editor's full list of knobs, not
    // only the modulation destinations - a dstOff knob still has to be there, or the audit
    // cannot see it and its parameter reads as unreachable from the UI.
    modTargets.push_back (limiterKnob);

    limiterButton.setButtonText ("LIMIT");
    limiterButton.setTooltip ("Output safety limiter");
    bar.addAndMakeVisible (limiterButton);
    limiterAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, "limiterOn", limiterButton);

    limiterMeter = std::make_unique<LimiterMeter>();
    bar.addAndMakeVisible (*limiterMeter);

    // The output scope, above everything else so it is on screen whatever tab is showing.
    scope = std::make_unique<ScopeComponent> (proc);
    addAndMakeVisible (*scope);

    // Performance bar: on-screen keyboard plus pitch and mod wheels. The
    // keyboard both mirrors incoming notes and plays the synth when clicked;
    // the wheels mirror MIDI and can be dragged to drive it.
    addAndMakeVisible (perfBar);

    keyboard = std::make_unique<juce::MidiKeyboardComponent> (
        proc.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard);
    keyboard->setAvailableRange (21, 108);
    keyboard->setLowestVisibleKey (36);
    keyboard->setKeyWidth (20.0f);
    keyboard->setScrollButtonsVisible (true);
    perfBar.addAndMakeVisible (*keyboard);

    pitchWheel = std::make_unique<mixxynth::ui::WheelComponent> ("PITCH", true);
    modWheel = std::make_unique<mixxynth::ui::WheelComponent> ("MOD", false);
    perfBar.addAndMakeVisible (*pitchWheel);
    perfBar.addAndMakeVisible (*modWheel);

    pitchWheel->onValueChanged = [this] (float v) { proc.setPitchBendFromUI (v); };
    modWheel->onValueChanged = [this] (float v) { proc.setModWheelFromUI (v); };

    // The FM routing matrix is a second view of the FM grid, and the modulation matrix is a
    // section of it, so the FM page is the only thing that has to be built for either. The
    // voice section is not a page at all - it lives in the sidebar.
    fmPage = std::make_unique<FmPage> (*this);
    fxPage = std::make_unique<FxPage> (*this);

    tabs.setOutline (0);
    tabs.addTab ("FM", juce::Colour (0xff14181d), fmPage.get(), false);
    tabs.addTab ("FX", juce::Colour (0xff14181d), fxPage.get(), false);
    addAndMakeVisible (tabs);

    setSize (1440, 900);
    startTimerHz (30);
}

MiXXynthEditor::~MiXXynthEditor() = default;

void MiXXynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b0d10));
    g.setColour (juce::Colour (0xff1b2027));
    g.fillRect (rail.getBounds());
    g.fillRect (bar.getBounds());
    g.fillRect (perfBar.getBounds());
}

void MiXXynthEditor::resized()
{
    auto b = getLocalBounds();

    title.setBounds (b.removeFromTop (34).reduced (10, 4));

    // The scope sits directly under the title and spans the full width, so it is on screen
    // whatever tab or section is showing. That is the whole reason it is here rather than
    // inside a page.
    if (scope != nullptr)
        scope->setBounds (b.removeFromTop (62).reduced (8, 2));

    // The sidebar is wide enough for the shape editors and never scrolls away,
    // so the LFOs and envelopes are visible on every tab.
    rail.setBounds (b.removeFromLeft (312));
    bar.setBounds (b.removeFromBottom (86));
    perfBar.setBounds (b.removeFromBottom (96));

    tabs.setBounds (b);

    auto railArea = rail.getLocalBounds().reduced (6);
    modStatus.setBounds (railArea.removeFromBottom (32));
    modViewport.setBounds (railArea);

    // Lay the content out inside the scroll: the voice section first, then the modulation
    // modules, the + buttons, and the performance chips.
    const int contentWidth = juce::jmax (200, modViewport.getMaximumVisibleWidth());
    const int stripH = ShapeStrip::preferredHeight();
    const int buttonH = 22;
    const int numLfos = proc.getNumLfoModules();

    // ---- voice section ----
    // Three columns. The sidebar is one column wide, and nine knobs stacked singly would
    // push the whole modulation section off the bottom of the scroll.
    int y = 0;

    voiceCaption.setBounds (0, y + 2, contentWidth, 16);
    y += 20;

    {
        constexpr int cols = 3;
        const int knobW = juce::jmax (60, contentWidth / cols);
        const int knobH = 84;

        for (int i = 0; i < voiceKnobs.size(); ++i)
            voiceKnobs[i]->setBounds ((i % cols) * knobW,
                                      y + (i / cols) * knobH,
                                      knobW - 4, knobH - 8);

        y += ((voiceKnobs.size() + cols - 1) / cols) * knobH;
    }

    {
        auto mpeRow = juce::Rectangle<int> (0, y, contentWidth, 24);

        if (mpeToggle != nullptr)
            mpeToggle->setBounds (mpeRow.removeFromLeft (64).reduced (2, 0));

        channelsLabel.setBounds (mpeRow.removeFromLeft (84));

        if (mpeChannels != nullptr)
            mpeChannels->setBounds (mpeRow.removeFromLeft (112).reduced (2, 0));

        y += 30;
    }

    // ---- modulation section ----
    railTitle.setBounds (0, y + 2, contentWidth, 16);
    y += 24;

    // No LFOs yet: the button has to come before the envelopes.
    if (numLfos == 0)
    {
        addLfoButton.setBounds (4, y, contentWidth - 8, buttonH);
        y += buttonH + 6;
    }

    for (int i = 0; i < shapeStrips.size(); ++i)
    {
        shapeStrips[i]->setBounds (0, y, contentWidth, stripH);
        y += stripH + 4;

        // Directly under the last LFO.
        if (i + 1 == numLfos)
        {
            addLfoButton.setBounds (4, y, contentWidth - 8, buttonH);
            y += buttonH + 6;
        }
    }

    // Directly under the last module, since the envelopes are last.
    addEnvButton.setBounds (4, y, contentWidth - 8, buttonH);
    y += buttonH + 8;

    perfChipTitle.setBounds (0, y + 2, contentWidth, 16);
    y += 22;

    const int chipH = 22;
    const int chipW = juce::jmax (90, (contentWidth - 4) / 2);
    int column = 0;
    int rowY = y;

    for (auto* c : chips)
    {
        c->setBounds (column * (chipW + 4), rowY, chipW, chipH);
        ++column;

        if (column == 2)
        {
            column = 0;
            rowY += chipH + 2;
        }
    }

    modContent.setSize (contentWidth, rowY + chipH + 8);

    auto barArea = bar.getLocalBounds().reduced (8);
    barTitle.setBounds (barArea.removeFromLeft (64));

    // Seven ways: the four macros, Master, the limiter's Release knob, and the area the
    // limiter's switch and meter live in. Getting this divisor wrong does not misalign
    // anything - it gives the limiter controls zero bounds, because the knobs have
    // already consumed the bar.
    const int knobW = juce::jmax (64, barArea.getWidth() / 7);

    for (auto* k : macroKnobs)
        k->setBounds (barArea.removeFromLeft (knobW).reduced (2));

    // The limiter's switch and meter fill what is left beside the Master knob. The
    // divider above leaves room for them by splitting the bar six ways rather than five.
    if (limiterButton.isVisible() || limiterMeter != nullptr)
    {
        auto limiterArea = barArea.removeFromLeft (juce::jmin (barArea.getWidth(), knobW));

        limiterButton.setBounds (limiterArea.removeFromLeft (58).reduced (2, 8));

        if (limiterMeter != nullptr)
            limiterMeter->setBounds (limiterArea.reduced (2, 22));
    }

    if (keyboard != nullptr)
    {
        auto pb = perfBar.getLocalBounds().reduced (6);

        // Wheels sit to the left of the keys, as on a real synth. Putting them
        // on the right pushed them off-screen in narrower host windows.
        auto wheels = pb.removeFromLeft (96);

        if (pitchWheel != nullptr)
            pitchWheel->setBounds (wheels.removeFromLeft (46).reduced (1));

        if (modWheel != nullptr)
            modWheel->setBounds (wheels.removeFromLeft (46).reduced (1));

        keyboard->setBounds (pb);
    }
}

void MiXXynthEditor::timerCallback()
{
    refreshModRings();

    // Advance the shape playheads and pick up preset loads.
    for (int i = 0; i < shapeStrips.size(); ++i)
        shapeStrips[i]->poll (shapePhaseFor (i));

    runTabPolls();

    // The wheels follow incoming MIDI; the keyboard repaints itself from its
    // MidiKeyboardState listener. A faster timer keeps the wheels looking live.
    if (pitchWheel != nullptr)
        pitchWheel->setValueFromMidi (proc.getPitchBendValue());

    if (modWheel != nullptr)
        modWheel->setValueFromMidi (proc.getModWheelValue());

    // The limiter's gain reduction, on the same poll as everything else.
    if (limiterMeter != nullptr)
        limiterMeter->setGainReduction (proc.getLimiterGainReductionDb());
}

void MiXXynthEditor::shapeEdited (int index, const mixxynth::ModShape& shape)
{
    proc.setShape (index, shape);
}

int MiXXynthEditor::getNumShapeModules() const { return proc.getNumShapeModules(); }
int MiXXynthEditor::getNumLfoModules() const { return proc.getNumLfoModules(); }

void MiXXynthEditor::rebuildShapeStrips()
{
    shapeStrips.clear();

    for (int i = 0; i < proc.getNumShapeModules(); ++i)
    {
        auto* strip = new ShapeStrip (*this, i);
        shapeStrips.add (strip);
        modContent.addAndMakeVisible (strip);
    }

    // Re-add the buttons so they sit above the freshly created strips.
    modContent.addAndMakeVisible (addLfoButton);
    modContent.addAndMakeVisible (addEnvButton);

    addLfoButton.setEnabled (proc.canAddShapeModule (true));
    addEnvButton.setEnabled (proc.canAddShapeModule (false));

    resized();
    refreshModRings();
}

void MiXXynthEditor::addShapeModule (bool lfo)
{
    if (proc.addShapeModule (lfo) < 0)
    {
        setModStatus (lfo ? "Already at 8 LFOs" : "Already at 8 envelopes");
        return;
    }

    setModStatus (juce::String (lfo ? "Added an LFO" : "Added an envelope")
                    + "  (" + juce::String (proc.getNumShapeModules()) + " of "
                    + juce::String (mixxynth::kMaxShapes) + " modules)");

    // Rebuilding deletes the strip whose button we are inside, so defer it.
    juce::Component::SafePointer<MiXXynthEditor> safe (this);

    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->rebuildShapeStrips();
    });
}

void MiXXynthEditor::removeShapeModule (int index)
{
    const int count = proc.getNumShapeModules();

    if (index < 0 || index >= count)
        return;

    const bool wasLfo = proc.shapeIsLfo (index);
    const int removedSource = proc.shapeSourceIndex (index);
    const int numLfos = proc.getNumLfoModules();
    const int numEnvs = count - numLfos;

    // Remap the matrix BEFORE the list changes, so no slot is ever left pointing
    // at the wrong module.
    //
    // Modules are stored contiguously and a module's source index is its position,
    // so removing one shifts every LATER module of the same family down by one.
    // The other family is unaffected: an envelope's source is
    // srcEnv1 + (position - numLfos), and both terms drop by one when an LFO is
    // removed, so the value is unchanged. Same reasoning the other way round.
    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        auto* src = choice (proc, "mod" + juce::String (n + 1) + "src");

        if (src == nullptr)
            continue;

        const int s = src->getIndex();

        if (s <= mixxynth::srcOff)
            continue;

        int mapped = s;

        if (s == removedSource)
            mapped = mixxynth::srcOff; // the module it pointed at is gone
        else if (wasLfo && s > removedSource && s <= mixxynth::srcLfo1 + numLfos - 1)
            mapped = s - 1;
        else if (! wasLfo && s > removedSource && s <= mixxynth::srcEnv1 + numEnvs - 1)
            mapped = s - 1;

        if (mapped != s)
        {
            src->beginChangeGesture();
            src->setValueNotifyingHost (src->convertTo0to1 (static_cast<float> (mapped)));
            src->endChangeGesture();
        }
    }

    proc.removeShapeModule (index);

    setModStatus (juce::String (wasLfo ? "Removed an LFO" : "Removed an envelope")
                    + "  (" + juce::String (proc.getNumShapeModules()) + " of "
                    + juce::String (mixxynth::kMaxShapes) + " modules)");

    juce::Component::SafePointer<MiXXynthEditor> safe (this);

    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->rebuildShapeStrips();
    });
}

void MiXXynthEditor::fxAddModule (int type)
{
    if (fxPage != nullptr)
        fxPage->addModule (type);
}

void MiXXynthEditor::fxRemoveModule (int slot)
{
    if (fxPage != nullptr)
        fxPage->removeModule (slot);
}

void MiXXynthEditor::fxMoveModule (int slot, int delta)
{
    if (fxPage != nullptr)
        fxPage->moveModule (slot, delta);
}

void MiXXynthEditor::fxSetCardChoice (int cardIndex, int boxIndex, int choiceIndex)
{
    if (fxPage != nullptr)
        fxPage->setCardChoice (cardIndex, boxIndex, choiceIndex);
}

void MiXXynthEditor::fxRebuildCards()
{
    if (fxPage != nullptr)
        fxPage->rebuild();
}

juce::String MiXXynthEditor::fxDebug() const
{
    return fxPage != nullptr ? fxPage->debugDump() : juce::String ("no page");
}

juce::String MiXXynthEditor::modDebug (int slot) const
{
    return fmPage != nullptr ? fmPage->modDebugDump (slot) : juce::String ("no page");
}

void MiXXynthEditor::pushModulatedValues()
{
    // False when nothing is sounding, so every control falls back to its parameter rather
    // than sitting on the last value it saw and pretending to be live.
    const bool live = proc.getModulatedValuesLive();

    for (auto* t : modTargets)
    {
        auto* param = t->getParam();
        const int dest = t->getDestIndex();

        // Only a destination that something is actually driving shows a live value. The
        // colour already decides "this is modulated" from the assignments alone; setting a
        // live value on a control nothing drives would paint it blue for no reason.
        bool modulated = false;

        for (const auto& a : t->getAssignments())
            if (a.source != mixxynth::srcOff && std::abs (a.depth) > 1.0e-6f)
            {
                modulated = true;
                break;
            }

        // A drag wins over the readout. Pushing the modulated value while the user is
        // dragging would make the fill jump out from under the cursor.
        if (! live || ! modulated || t->isAdjusting()
              || param == nullptr || dest <= 0 || dest >= mixxynth::kNumModDests)
        {
            t->setLiveValue01 (-1.0f);
            continue;
        }

        // The PARAMETER does the normalising, because it knows its own skew. Filter cutoff
        // is the case that makes this matter: 5000 Hz is nowhere near the middle of a log
        // range, so converting it by hand would put the fill in the wrong place.
        t->setLiveValue01 (param->convertTo0to1 (proc.getModulatedValue (dest)));
    }
}

void MiXXynthEditor::runTabPolls()
{
    pushModulatedValues();

    if (fmPage != nullptr)
        fmPage->refreshAudibility();

    // The Snap switch, on the same poll as everything else. One place decides what the button
    // says AND how the controls behave, so the two cannot disagree - which they would if the
    // label were set from the click and the behaviour from the parameter.
    {
        const auto* snapParam = proc.apvts.getRawParameterValue ("snapValues");
        const bool snap = snapParam != nullptr && snapParam->load() > 0.5f;

        mixxynth::SnapValues::setEnabled (snap);

        if (fmPage != nullptr)
            fmPage->setSnapLabel (snap);
    }

    // The FX page watches the slot types so a preset load repopulates the rack.
    if (fxPage != nullptr)
        fxPage->poll();

    // The modulation matrix's rows are hand-bound, so they need polling too - otherwise a
    // slot created by dragging a chip onto a knob never shows up in the list.
    if (fmPage != nullptr)
        fmPage->pollModRows();
}

void MiXXynthEditor::rollFmDice()
{
    if (fmPage != nullptr)
        fmPage->rollDice();
}

void MiXXynthEditor::setFmView (int v)
{
    if (fmPage != nullptr)
        fmPage->setView (v);
}

void MiXXynthEditor::setFmAdsrPage (int page)
{
    if (fmPage != nullptr)
        fmPage->setAdsrPage (page);
}

std::vector<mixxynth::ui::AdsrEditor*> MiXXynthEditor::getAdsrEditors() const
{
    std::vector<mixxynth::ui::AdsrEditor*> out;

    if (fmPage != nullptr)
        for (auto* e : fmPage->envelopeEditors())
            out.push_back (e);

    return out;
}

void MiXXynthEditor::setModStatus (const juce::String& text)
{
    modStatus.setText (text, juce::dontSendNotification);
}

float MiXXynthEditor::shapePhaseFor (int index) const
{
    return proc.getShapePhase (index);
}

juce::String MiXXynthEditor::sidebarDebug() const
{
    juce::String s;
    s << "strips=" << shapeStrips.size()
      << " lfos=" << proc.getNumLfoModules()
      << " modules=" << proc.getNumShapeModules()
      << " chips=" << chips.size()
      << " rail=" << rail.getBounds().toString()
      << " viewport=" << modViewport.getBounds().toString()
      << " content=" << modContent.getBounds().toString();

    int minH = 100000;
    bool allVisible = true;
    bool allInRail = true;

    for (auto* st : shapeStrips)
    {
        minH = juce::jmin (minH, st->getHeight());
        allVisible = allVisible && st->isVisible();

        // The strip is a child of the scrolled content, so its x is relative to
        // that; the rail check is really about the content being inside the rail.
        allInRail = allInRail && st->getWidth() > 0;
    }

    s << " minStripH=" << (shapeStrips.isEmpty() ? 0 : minH)
      << " allVisible=" << (allVisible ? 1 : 0)
      << " allSized=" << (allInRail ? 1 : 0)
      << " scrolls=" << (modContent.getHeight() > modViewport.getHeight() ? 1 : 0);

    return s;
}

void MiXXynthEditor::assignModSlot (int srcIndex, int destIndex)
{
    if (srcIndex <= mixxynth::srcOff || destIndex == mixxynth::dstOff)
        return;

    int target = -1;
    int freeSlot = -1;

    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto* s = choice (proc, "mod" + id + "src");
        auto* d = choice (proc, "mod" + id + "dst");

        if (s == nullptr || d == nullptr)
            continue;

        if (s->getIndex() == srcIndex && d->getIndex() == destIndex)
        {
            target = n;
            break;
        }

        if (freeSlot < 0 && s->getIndex() == mixxynth::srcOff)
            freeSlot = n;
    }

    if (target < 0)
        target = freeSlot >= 0 ? freeSlot : 0;

    const juce::String id = juce::String (target + 1);

    if (auto* s = choice (proc, "mod" + id + "src"))
    {
        s->beginChangeGesture();
        s->setValueNotifyingHost (s->convertTo0to1 (static_cast<float> (srcIndex)));
        s->endChangeGesture();
    }

    if (auto* d = choice (proc, "mod" + id + "dst"))
    {
        d->beginChangeGesture();
        d->setValueNotifyingHost (d->convertTo0to1 (static_cast<float> (destIndex)));
        d->endChangeGesture();
    }

    if (auto* dep = ranged (proc, "mod" + id + "depth"))
    {
        // getValue() is normalised, so denormalise before testing for zero.
        const float current = dep->convertFrom0to1 (dep->getValue());

        if (std::abs (current) < 0.0001f)
        {
            dep->beginChangeGesture();
            dep->setValueNotifyingHost (dep->convertTo0to1 (0.35f));
            dep->endChangeGesture();
        }
    }

    refreshModRings();
}

void MiXXynthEditor::clearModDest (int destIndex)
{
    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto* d = choice (proc, "mod" + id + "dst");

        if (d == nullptr || d->getIndex() != destIndex)
            continue;

        if (auto* dep = ranged (proc, "mod" + id + "depth"))
        {
            dep->beginChangeGesture();
            dep->setValueNotifyingHost (dep->convertTo0to1 (0.0f));
            dep->endChangeGesture();
        }

        d->beginChangeGesture();
        d->setValueNotifyingHost (0.0f);
        d->endChangeGesture();

        if (auto* s = choice (proc, "mod" + id + "src"))
        {
            s->beginChangeGesture();
            s->setValueNotifyingHost (0.0f);
            s->endChangeGesture();
        }
    }

    refreshModRings();
}

void MiXXynthEditor::refreshModRings()
{
    // Collect every slot's (source, depth) per destination, so each knob can
    // draw one arc per source and let the ring be dragged to change the amount.
    std::array<std::vector<mixxynth::ui::ModTarget::Assignment>, mixxynth::kNumModDests> byDest;

    // Cheap signature so the 30 Hz timer does not rebuild 121 assignment vectors
    // when nothing has moved.
    juce::uint64 sig = 1469598103934665603ull;

    auto mix = [&sig] (int v)
    {
        sig ^= static_cast<juce::uint64> (static_cast<juce::uint32> (v));
        sig *= 1099511628211ull;
    };

    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto* s = choice (proc, "mod" + id + "src");
        auto* d = choice (proc, "mod" + id + "dst");

        if (s == nullptr || d == nullptr)
            continue;

        const int src = s->getIndex();
        const int dest = d->getIndex();

        float depth = 0.0f;

        if (auto* dep = ranged (proc, "mod" + id + "depth"))
            depth = dep->convertFrom0to1 (dep->getValue());

        mix (src);
        mix (dest);
        mix (juce::roundToInt (depth * 100000.0f));

        if (src == mixxynth::srcOff || dest == mixxynth::dstOff)
            continue;

        if (dest < 0 || dest >= mixxynth::kNumModDests)
            continue;

        byDest[static_cast<size_t> (dest)].push_back ({ src, depth });
    }

    bool anyAdjusting = false;

    for (auto* k : modTargets)
    {
        if (k->isAdjusting())
        {
            anyAdjusting = true;
            break;
        }
    }

    if (! anyAdjusting && modSignatureValid && sig == modSignature)
        return;

    modSignature = sig;
    modSignatureValid = true;

    // While a ring is being dragged the knob owns its own value, so leave it be
    // rather than fighting the user for the parameter.
    for (auto* k : modTargets)
    {
        const int dest = k->getDestIndex();

        if (dest < 0 || dest >= mixxynth::kNumModDests || k->isAdjusting())
            continue;

        k->setModAssignments (byDest[static_cast<size_t> (dest)]);
    }
}

void MiXXynthEditor::wireModTarget (mixxynth::ui::ModTarget& k)
{
    k.onSourceDropped = [this] (int s, int d) { assignModSlot (s, d); };
    k.onClearRequested = [this] (int d) { clearModDest (d); };
    k.onModDepthChanged = [this] (int s, int d, float depth) { setModDepthFor (s, d, depth); };
    k.onModRemoveRequested = [this] (int s, int d) { removeModSlot (s, d); };
    k.onStatus = [this] (const juce::String& t) { setModStatus (t); };
}

// Finds the slot linking (src,dest), creating one if the pair is not assigned yet.
int MiXXynthEditor::findModSlot (int srcIndex, int destIndex) const
{
    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto* s = choice (proc, "mod" + id + "src");
        auto* d = choice (proc, "mod" + id + "dst");

        if (s != nullptr && d != nullptr
              && s->getIndex() == srcIndex && d->getIndex() == destIndex)
            return n;
    }

    return -1;
}

void MiXXynthEditor::setModDepthFor (int srcIndex, int destIndex, float depth)
{
    if (srcIndex <= mixxynth::srcOff || destIndex == mixxynth::dstOff)
        return;

    int slot = findModSlot (srcIndex, destIndex);

    if (slot < 0)
    {
        // Not assigned yet: create the pair (which also picks a free slot).
        assignModSlot (srcIndex, destIndex);
        slot = findModSlot (srcIndex, destIndex);
    }

    if (slot < 0)
        return;

    if (auto* dep = ranged (proc, "mod" + juce::String (slot + 1) + "depth"))
    {
        dep->beginChangeGesture();
        dep->setValueNotifyingHost (dep->convertTo0to1 (juce::jlimit (-1.0f, 1.0f, depth)));
        dep->endChangeGesture();
    }

    refreshModRings();
}

void MiXXynthEditor::removeModSlot (int srcIndex, int destIndex)
{
    const int slot = findModSlot (srcIndex, destIndex);

    if (slot < 0)
        return;

    const juce::String id = juce::String (slot + 1);

    if (auto* dep = ranged (proc, "mod" + id + "depth"))
    {
        dep->beginChangeGesture();
        dep->setValueNotifyingHost (dep->convertTo0to1 (0.0f));
        dep->endChangeGesture();
    }

    for (const char* suffix : { "src", "dst" })
    {
        if (auto* p = choice (proc, "mod" + id + suffix))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (0.0f);
            p->endChangeGesture();
        }
    }

    setModStatus ("Removed "
                  + juce::String (mixxynth::modSourceNames()[juce::jlimit (0, mixxynth::kNumModSources - 1, srcIndex)])
                  + " modulation");

    refreshModRings();
}
