#include "PluginEditor.h"

#include "AdsrEditor.h"
#include "Algorithms.h"
#include "EditorTheme.h"
#include "ModMatrix.h"
#include "PluginProcessor.h"
#include "SquareCell.h"

using mixxynth::ui::ModKnob;
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
        auto& proc = ed.processor();
        const bool isLfo = proc.shapeIsLfo (index);
        const int src = proc.shapeSourceIndex (index);

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
        const auto& proc = ed.processor();

        return proc.shapeIsLfo (index)
                 ? "LFO " + juce::String (index + 1)
                 : "Env " + juce::String (index - proc.getNumLfoModules() + 1);
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

    // A two-segment switch for choosing which reading of the grid you are looking
    // at. Drawn rather than built from TextButtons, so it sits in the same visual
    // language as the cells it switches between.
    class ViewSwitch : public juce::Component
    {
    public:
        ViewSwitch (const juce::String& first, const juce::String& second)
        {
            names[0] = first;
            names[1] = second;
        }

        void setSelected (int index, juce::NotificationType n = juce::sendNotificationSync)
        {
            index = juce::jlimit (0, 1, index);

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
            setSelected (e.getPosition().x < getWidth() / 2 ? 0 : 1);
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();

            g.setColour (juce::Colour (0xff161c22));
            g.fillRoundedRectangle (b, 4.0f);

            for (int i = 0; i < 2; ++i)
            {
                const auto seg = getLocalBounds().withWidth (getWidth() / 2)
                                                  .withX (i * (getWidth() / 2));

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
        juce::String names[2];
        int selected = 0;
    };
}

// FM page: algorithm + FM index + per-op ratio/level/feedback/ADSR
// ---------------------------------------------------------------------------
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

        // The envelope column keeps its word: it is not one of the four value
        // columns the icons replace, and "ADSR" is four letters, not a glyph.
        adsrHeader.setText ("ADSR", juce::dontSendNotification);
        adsrHeader.setJustificationType (juce::Justification::centred);
        adsrHeader.setColour (juce::Label::textColourId, juce::Colour (0xff7c8896));
        adsrHeader.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        addAndMakeVisible (adsrHeader);

        // FM Index is one value for the whole stack rather than a per-operator one,
        // so it sits in the header row's first cell, out of the operator rows' way.
        indexCell = addCell (ranged (p, "fmIndex"), mixxynth::dstFmIndex, "FM Index");

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

            // The morph is a continuous sweep through sine > triangle > saw > square,
            // so the cell names the shape it is at rather than showing a bare number.
            // Step 4 replaces this text with the actual waveform.
            shape->setValueTextFunction ([] (float v)
            {
                return juce::String (mixxynth::OpWaveTables::shapeName (v));
            });

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
            addAndMakeVisible (env);
        }

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
        const bool routing = viewSwitch.getSelected() == 1;

        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText (routing
                            ? "Each cell sends its row's operator into its column's operator. The diagonal is that "
                              "operator's own output, not feedback - feedback is the Fb column back in Values."
                            : "Drag a cell up or down to set its value. Drop a modulation source onto one to modulate it, "
                              "then drag the white line to set how far the modulation reaches.",
                          getLocalBounds().removeFromTop (18), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (18);   // the hint line, drawn in paint()

        // Toolbar: the die, the view switch, then the audibility warning.
        auto toolbar = b.removeFromTop (30);
        diceButton.setBounds (toolbar.removeFromLeft (30).reduced (2));
        toolbar.removeFromLeft (8);
        viewSwitch.setBounds (toolbar.removeFromLeft (150).reduced (0, 2));
        toolbar.removeFromLeft (8);
        audibility.setBounds (toolbar);

        b.removeFromTop (6);

        const bool routing = viewSwitch.getSelected() == 1;

        constexpr int numRows = MiXXFmVoice::numOps + 1;  // header row + one per operator
        constexpr int gap = 3;

        // The geometry is computed ONCE, from the wider of the two views, and both are
        // laid out from that same origin. That is what makes the views align: the
        // operator column and the first five value columns sit at identical x in both,
        // so switching views moves nothing. Sizing each view to its own content instead
        // made them different widths, so centring put their left edges in different
        // places and the shared operator cells jumped sideways on every switch.
        //
        //   values:  operator, shape, ratio, level, fdbk, envelope (three squares wide)
        //   routing: operator, then one column per target operator
        //
        // The routing view is one square narrower, so it leaves a square of unused
        // space on the right rather than re-centring.
        constexpr int valueCols = 6;
        constexpr int valueSquares = 8;   // the envelope counts as three
        constexpr int routingCols = 7;

        const int numCols = routing ? routingCols : valueCols;

        // Squares are sized by whichever constraint binds first: the room there is, or
        // the room the squares need. The values view is the wider of the two, so it is
        // always the binding one. This is what lets the grid scale with the window
        // without ever distorting, and why the minimum window size has to be large
        // enough for the squares to stay legible.
        const int rowH = (b.getHeight() - gap * (numRows - 1)) / numRows;

        const int square = juce::jmax (22, juce::jmin (rowH,
                                      (b.getWidth() - gap * (valueCols - 1)) / valueSquares));

        const int tailW = square * 3;   // the envelope column, values view only

        // The footprint is the values view's, whichever view is showing.
        const int gridW = 5 * square + tailW + gap * (valueCols - 1);
        const int gridH = numRows * square + (numRows - 1) * gap;

        const int originX = b.getX() + juce::jmax (0, (b.getWidth() - gridW) / 2);
        const int originY = b.getY() + juce::jmax (0, (b.getHeight() - gridH) / 2);

        // Columns are contiguous: each starts one gap after the previous one ends. The
        // wider last column's position already accounts for its width, so it must NOT
        // be given an extra offset as well - that bug floated the envelope column away
        // from the grid and pushed the whole thing off centre.
        auto cellAt = [&] (int row, int col)
        {
            const int w = (! routing && col == valueCols - 1) ? tailW : square;

            return juce::Rectangle<int> (originX + col * (square + gap),
                                         originY + row * (square + gap), w, square);
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

        adsrHeader.setBounds (cellAt (0, valueCols - 1));

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

            // The envelope takes the whole rectangular cell.
            if (i < adsrEditors.size())
                adsrEditors[i]->setBounds (cellAt (row, valueCols - 1).reduced (1));
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

    // Shows one view's contents and hides the other's. The operator cells and the
    // toolbar are shared, because both views have the same six rows: row 3 is Op3
    // whether you are reading values or routing, and that shared axis is the whole
    // reason the two grids belong in one place.
    void applyView (int v)
    {
        const bool routing = (v == 1);

        for (auto* t : outCells)
            t->setVisible (routing);

        for (auto* c : routingOwned)
            c->setVisible (routing);

        for (auto* h : routingHeaders)
            h->setVisible (routing);

        routingFrom.setVisible (routing);

        // indexCell is cells[0], so this covers it.
        for (auto* c : cells)
            c->setVisible (! routing);

        for (auto* icon : headerIcons)
            icon->setVisible (! routing);

        adsrHeader.setVisible (! routing);

        for (auto* e : adsrEditors)
            e->setVisible (! routing);

        resized();
        repaint();
    }

    MiXXynthEditor& ed;
    DiceButton diceButton;
    ViewSwitch viewSwitch { "Values", "Routing" };
    juce::Label audibility;
    juce::Label adsrHeader;
    juce::OwnedArray<GridIcon> headerIcons;

    // cells[0] is FM Index; the operator rows follow as four per operator.
    SquareCell* indexCell = nullptr;

    // One envelope per operator, in operator order.
    juce::OwnedArray<AdsrEditor> adsrEditors;

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

    juce::Random rng;
    juce::OwnedArray<SquareCell> ownedCells;
    juce::Array<SquareCell*> cells;
    juce::OwnedArray<OperatorCell> opOnToggles;
};

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
            depth = new ModKnob ("Depth", depthParam, mixxynth::dstOff, true);
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
            label.setBounds (b.removeFromLeft (52));
            src.setBounds (b.removeFromLeft (110).reduced (2));
            dst.setBounds (b.removeFromLeft (150).reduced (2));
            depth->setBounds (b.removeFromLeft (72));
            clear.setBounds (b.removeFromLeft (26).reduced (2, 6));
        }

        std::function<void()> onChanged;

        ModKnob* getDepthKnob() const { return depth; }

    private:
        juce::Label label;
        juce::ComboBox src, dst;
        juce::AudioParameterChoice* srcParam = nullptr;
        juce::AudioParameterChoice* dstParam = nullptr;
        ModKnob* depth = nullptr;
        juce::TextButton clear;
    };
}

// ---------------------------------------------------------------------------
// Mod page: the 32 matrix slot rows. The LFO and envelope shapes are edited in
// the always-visible sidebar, not here.
// ---------------------------------------------------------------------------
class MiXXynthEditor::ModPage : public juce::Component
{
public:
    explicit ModPage (MiXXynthEditor& e) : ed (e)
    {
        auto& p = e.processor();

        for (int n = 0; n < mixxynth::kNumModSlots; ++n)
        {
            auto* row = new SlotRow (p, n);
            row->onChanged = [this] { ed.refreshModRings(); };
            content.addAndMakeVisible (row);
            rows.add (row);
            ed.modTargets.push_back (row->getDepthKnob());
        }

        content.setSize (420, mixxynth::kNumModSlots * rowHeight);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

    }

    // The rows are bound by hand, so they need an explicit refresh. Without this a
    // slot assigned by dragging a chip onto a knob leaves this tab still showing
    // Off, which is exactly what it used to do - the parameters were always
    // correct, the tab just never looked again.
    void poll()
    {
        for (auto* row : rows)
            row->poll();
    }

    // For the audit.
    juce::String debugDump (int slot) const
    {
        if (slot < 0 || slot >= rows.size())
            return "out of range";

        return rows[slot]->debugDump();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("32 slots. Drag a source onto any knob - use the chip on any shape in the left sidebar, "
                          "or a performance chip - or set source and destination here. Right-click a knob to clear "
                          "its modulation. The four LFOs and three envelopes are drawn and edited in the sidebar, "
                          "and stay visible on every tab.",
                          getLocalBounds().removeFromTop (46), juce::Justification::centredLeft, 3);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (46);
        viewport.setBounds (b);

        // The rows were created and added but never given bounds, so the entire
        // tab rendered empty. Lay them out inside the scrolled content, sizing
        // the content to the viewport so the rows use the full width.
        content.setSize (juce::jmax (320, viewport.getMaximumVisibleWidth()),
                         mixxynth::kNumModSlots * rowHeight);

        auto contentArea = content.getLocalBounds();

        for (auto* row : rows)
            row->setBounds (contentArea.removeFromTop (rowHeight));
    }

private:
    static constexpr int rowHeight = 26;

    MiXXynthEditor& ed;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<SlotRow> rows;
};

// ---------------------------------------------------------------------------
// VA / Filter page (M3b)
// ---------------------------------------------------------------------------
class MiXXynthEditor::VaPage : public juce::Component
{
public:
    explicit VaPage (MiXXynthEditor& e) : ed (e)
    {
        auto& p = e.processor();

        addKnob (ranged (p, "vaLevel"), mixxynth::dstVaLevel, "VA Level");
        addKnob (ranged (p, "vaDetune"), mixxynth::dstVaDetune, "VA Detune");

        // The shape morph, through the same tables the operators use. The destination is
        // the old pulse width slot, which is the same idea on the new axis - and keeping
        // that slot means an existing modulation routing still points somewhere sensible.
        addKnob (ranged (p, "vaShape"), mixxynth::dstVaPulseWidth, "Shape");

        // Their own levels rather than shape-list entries, so they blend with any shape.
        addKnob (ranged (p, "vaSubLevel"), mixxynth::dstOff, "Sub");
        addKnob (ranged (p, "vaNoiseLevel"), mixxynth::dstOff, "Noise");

        addKnob (ranged (p, "filterCutoff"), mixxynth::dstFilterCutoff, "Cutoff");
        addKnob (ranged (p, "filterReso"), mixxynth::dstFilterReso, "Resonance");
        addKnob (ranged (p, "filterDrive"), mixxynth::dstFilterDrive, "Drive");

        addChoice (p, "filterType", filterType);

        // The VA's amplitude envelope. Same component the operator envelopes use, so the
        // two cannot drift apart - and it is a real ADSR now rather than a fixed gate.
        vaEnvEditor = new mixxynth::ui::AdsrEditor (ranged (p, "vaAttack"),
                                                    ranged (p, "vaDecay"),
                                                    ranged (p, "vaSustain"),
                                                    ranged (p, "vaRelease"));
        vaEnvOwned.add (vaEnvEditor);
        addAndMakeVisible (vaEnvEditor);

        filterOn = addToggle (p, "filterOn", "Filter On");

        octave = new juce::Slider (juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft);
        octave->setRange (-2.0, 2.0, 1.0);
        addAndMakeVisible (octave);

        if (auto* param = dynamic_cast<juce::AudioParameterInt*> (ranged (p, "vaOctave")))
        {
            octave->setValue (param->get(), juce::dontSendNotification);
            octave->onValueChange = [this, param]
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (
                    param->convertTo0to1 (static_cast<float> (octave->getValue())));
                param->endChangeGesture();
            };
        }

        octaveLabel.setText ("Octave", juce::dontSendNotification);
        addAndMakeVisible (octaveLabel);

        hint.setJustificationType (juce::Justification::centredLeft);
        hint.setFont (juce::FontOptions (11.0f));
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff4ade80));
        addAndMakeVisible (hint);
        refreshHint();
    }

    // Driven from the editor's timer: spells out what the VA layer and filter are
    // actually doing, so a page full of knobs is never silently inert.
    void refreshHint()
    {
        const float va = juce::jlimit (0.0f, 1.0f, readParam ("vaLevel"));
        const bool filterActive = readParam ("filterOn") > 0.5f;
        const float cut = readParam ("filterCutoff");
        const float reso = readParam ("filterReso");
        const int type = static_cast<int> (readParam ("filterType"));

        static const char* const typeNames[] = { "Low Pass", "High Pass", "Band Pass" };

        const int vaPercent = juce::roundToInt (va * 100.0f);

        juce::String s;
        s << (vaPercent <= 0 ? juce::String ("FM only (VA Level is 0)")
                             : "VA " + juce::String (vaPercent) + "% / FM " + juce::String (100 - vaPercent) + "%")
          << "   |   Filter: ";

        if (filterActive)
            s << typeNames[juce::jlimit (0, 2, type)] << " " << juce::String (cut, 0) << " Hz, Q "
              << juce::String (reso, 2);
        else
            s << "bypassed (turn Filter On)";

        hint.setText (s, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("VA Level ADDS the VA oscillator on top of the FM stack (0 = FM only). "
                          "The VA has its own gate envelope. The filter is per voice and all six knobs are "
                          "modulation destinations.",
                          getLocalBounds().removeFromTop (32), juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10);
        b.removeFromTop (34);

        auto row = b.removeFromTop (96);

        for (auto* k : knobs)
            k->setBounds (row.removeFromLeft (juce::jmax (80, row.getWidth() / 4)).reduced (4));

        row = b.removeFromTop (96);

        for (auto* k : knobs2)
            k->setBounds (row.removeFromLeft (juce::jmax (80, row.getWidth() / 4)).reduced (4));

        b.removeFromTop (10);
        auto controls = b.removeFromTop (26);
        octaveLabel.setBounds (controls.removeFromLeft (56));
        octave->setBounds (controls.removeFromLeft (120).reduced (2, 0));
        controls.removeFromLeft (16);
        filterType.setBounds (controls.removeFromLeft (130).reduced (2, 0));
        controls.removeFromLeft (16);

        if (filterOn != nullptr)
            filterOn->setBounds (controls.removeFromLeft (100).reduced (2, 0));

        // The VA's amplitude envelope gets the rest of the page. It wants width far more
        // than height, which is the same reason the operator envelope column is wider than
        // a square.
        if (vaEnvEditor != nullptr)
            vaEnvEditor->setBounds (b.removeFromTop (juce::jmin (150, b.getHeight()))
                                     .removeFromLeft (juce::jmax (260, b.getWidth() / 2)));

        b.removeFromTop (8);
        hint.setBounds (b.removeFromTop (20));
    }

private:
    float readParam (const juce::String& id)
    {
        if (auto* p = ranged (ed.processor(), id))
            return p->convertFrom0to1 (p->getValue());

        return 0.0f;
    }

    void addKnob (juce::RangedAudioParameter* param, int dest, const juce::String& label)
    {
        auto* k = new ModKnob (label, param, dest, true);
        ed.wireModTarget (*k);
        addAndMakeVisible (k);
        ed.modTargets.push_back (k);
        owned.add (k);

        if (knobs.size() < 4)
            knobs.add (k);
        else
            knobs2.add (k);
    }

    void addChoice (MiXXynthProcessor& p, const juce::String& id, juce::ComboBox& box)
    {
        addAndMakeVisible (box);

        if (auto* param = choice (p, id))
        {
            for (int i = 0; i < param->choices.size(); ++i)
                box.addItem (param->choices[i], i + 1);

            box.setSelectedId (param->getIndex() + 1, juce::dontSendNotification);
            box.onChange = [&box, param]
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (
                    param->convertTo0to1 (static_cast<float> (box.getSelectedId() - 1)));
                param->endChangeGesture();
            };
        }
    }

    juce::ToggleButton* addToggle (MiXXynthProcessor& p, const juce::String& id, const juce::String& text)
    {
        auto* t = new juce::ToggleButton (text);
        toggles.add (t);
        addAndMakeVisible (t);

        if (auto* param = dynamic_cast<juce::AudioParameterBool*> (ranged (p, id)))
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

    MiXXynthEditor& ed;
    juce::OwnedArray<ModKnob> owned;
    juce::Array<ModKnob*> knobs, knobs2;
    juce::ComboBox filterType;
    juce::ToggleButton* filterOn = nullptr;
    juce::Slider* octave = nullptr;
    juce::Label octaveLabel;
    juce::Label hint;
    juce::OwnedArray<juce::ToggleButton> toggles;

    // The VA's amplitude envelope, edited with the same component the operator envelopes
    // use so the two look and behave alike. Not a ModTarget: it only ever controls the VA
    // layer's own amplitude, so it is owned here rather than registered for modulation.
    juce::OwnedArray<mixxynth::ui::AdsrEditor> vaEnvOwned;
    mixxynth::ui::AdsrEditor* vaEnvEditor = nullptr;
};

// ---------------------------------------------------------------------------
// Voice page: polyphony and unison
// ---------------------------------------------------------------------------
class MiXXynthEditor::VoicePage : public juce::Component
{
public:
    explicit VoicePage (MiXXynthEditor& e) : ed (e)
    {
        auto& p = e.processor();

        addKnob (ranged (p, "polyphony"), "Polyphony");
        addKnob (ranged (p, "unison"), "Unison");
        addKnob (ranged (p, "unisonDetune"), "Unison Detune");
        addKnob (ranged (p, "unisonSpread"), "Unison Spread");
        addKnob (ranged (p, "unisonPhaseRand"), "Phase Random");
        addKnob (ranged (p, "pitchBendRange"), "Bend Range");

        mpeToggle = makeBoolToggle (p, "mpeEnabled", "MPE");
        addAndMakeVisible (mpeToggle);

        mpeChannels = new juce::Slider (juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft);
        mpeChannels->setRange (1.0, 15.0, 1.0);
        addAndMakeVisible (mpeChannels);

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
        addAndMakeVisible (channelsLabel);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("Polyphony is the maximum number of notes held at once; unison stacks detuned, "
                          "stereo-spread copies per note. They share a 64-voice pool, so a large unison "
                          "reduces how many notes fit.",
                          getLocalBounds().removeFromTop (34), juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10);
        b.removeFromTop (38);

        auto row = b.removeFromTop (110);

        // Divide by the actual knob count, so adding one does not squash the row.
        const int knobCount = juce::jmax (1, knobs.size());
        const int cellW = juce::jmax (80, b.getWidth() / knobCount);

        for (auto* k : knobs)
            k->setBounds (row.removeFromLeft (cellW).reduced (6));

        b.removeFromTop (8);
        auto mpeRow = b.removeFromTop (26);

        if (mpeToggle != nullptr)
            mpeToggle->setBounds (mpeRow.removeFromLeft (90).reduced (2, 0));

        mpeRow.removeFromLeft (12);
        channelsLabel.setBounds (mpeRow.removeFromLeft (100));

        if (mpeChannels != nullptr)
            mpeChannels->setBounds (mpeRow.removeFromLeft (120).reduced (2, 0));
    }

private:
    void addKnob (juce::RangedAudioParameter* param, const juce::String& label)
    {
        // Voice settings are not modulation destinations.
        auto* k = new ModKnob (label, param, mixxynth::dstOff, true);
        addAndMakeVisible (k);
        ed.modTargets.push_back (k);
        owned.add (k);
        knobs.add (k);
    }

    MiXXynthEditor& ed;
    juce::OwnedArray<ModKnob> owned;
    juce::Array<ModKnob*> knobs;
    juce::ToggleButton* mpeToggle = nullptr;
    juce::Slider* mpeChannels = nullptr;
    juce::Label channelsLabel;
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
    class Card : public juce::Component
    {
    public:
        Card (FxPage& o, int slotIndex) : owner (o), slot (slotIndex)
        {
            auto& proc = o.ed.processor();
            const auto state = proc.readFxSlot (slot);
            const auto desc = mixxynth::FxRack::descriptorFor (state.type);
            const juce::String prefix = "fx" + juce::String (slot + 1);

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
                               && ! proc.readFxSlot (slot + 1).isEmpty());

            up.onClick = [this] { owner.moveModule (slot, -1); };
            down.onClick = [this] { owner.moveModule (slot, 1); };
            remove.onClick = [this] { owner.removeModule (slot); };

            addAndMakeVisible (up);
            addAndMakeVisible (down);
            addAndMakeVisible (remove);

            bypass.setButtonText ("bypass");
            bypass.setTooltip ("Bypass this module");

            if (auto* b = dynamic_cast<juce::AudioParameterBool*> (
                    proc.apvts.getParameter (prefix + "bypass")))
                bypassAttachment = std::make_unique<juce::ButtonParameterAttachment> (*b, bypass);

            addAndMakeVisible (bypass);

            for (int p = 0; p < desc.numParams && p < mixxynth::kMaxFxParams; ++p)
            {
                const auto& info = desc.params[static_cast<size_t> (p)];
                const juce::String id = prefix + "p" + juce::String (p + 1);

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
                    choiceParams.add (proc.apvts.getParameter (id));
                    choiceCounts.add (info.choices.size());

                    addAndMakeVisible (*box);
                    box->setSelectedItemIndex (indexFromParam (boxIndex), juce::dontSendNotification);
                    continue;
                }

                // Dest Off: the rack's generic params are not in the matrix's
                // destination list, so these knobs are not modulation targets and
                // a dropped chip is rejected with a flash rather than ignored.
                auto* k = new ModKnob (info.name, proc.apvts.getParameter (id),
                                       mixxynth::dstOff, true);
                owner.ed.wireModTarget (*k);
                addAndMakeVisible (k);
                owned.add (k);
                knobs.add (k);
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

        static int preferredHeight() { return 26 + 4 + 30 + 78 + 10; }

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
        }

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
        auto& proc = ed.processor();

        // Compact rather than leaving a hole. The chain being dense is what makes
        // "add fills the first free slot" mean "append to the end", and what makes
        // the arrows a plain adjacent swap. A hole would silently break both.
        for (int s = slot; s < mixxynth::kMaxFxSlots - 1; ++s)
            proc.writeFxSlot (s, proc.readFxSlot (s + 1));

        proc.writeFxSlot (mixxynth::kMaxFxSlots - 1, mixxynth::FxSlotState {});

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

        auto& proc = ed.processor();
        const auto here = proc.readFxSlot (slot);

        if (here.isEmpty())
            return;

        const auto there = proc.readFxSlot (target);

        proc.writeFxSlot (slot, there);
        proc.writeFxSlot (target, here);

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
        const int cardH = Card::preferredHeight();

        int y = 0;

        for (auto* c : cards)
        {
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
        auto& proc = ed.processor();
        const auto state = proc.readFxSlot (slot);
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

    // The FM routing matrix is a second view of the FM grid now rather than a tab of
    // its own, so the FM page is the only thing that has to be built for it.
    fmPage = std::make_unique<FmPage> (*this);
    modPage = std::make_unique<ModPage> (*this);
    vaPage = std::make_unique<VaPage> (*this);
    voicePage = std::make_unique<VoicePage> (*this);
    fxPage = std::make_unique<FxPage> (*this);

    tabs.setOutline (0);
    tabs.addTab ("FM", juce::Colour (0xff14181d), fmPage.get(), false);
    tabs.addTab ("VA/Filt", juce::Colour (0xff14181d), vaPage.get(), false);
    tabs.addTab ("Voice", juce::Colour (0xff14181d), voicePage.get(), false);
    tabs.addTab ("Mod", juce::Colour (0xff14181d), modPage.get(), false);
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

    // The sidebar is wide enough for the shape editors and never scrolls away,
    // so the LFOs and envelopes are visible on every tab.
    rail.setBounds (b.removeFromLeft (312));
    bar.setBounds (b.removeFromBottom (86));
    perfBar.setBounds (b.removeFromBottom (96));

    tabs.setBounds (b);

    auto railArea = rail.getLocalBounds().reduced (6);
    railTitle.setBounds (railArea.removeFromTop (18));
    modStatus.setBounds (railArea.removeFromBottom (32));
    modViewport.setBounds (railArea);

    // Lay the modules out inside the scrolled content: LFOs, then the + LFO
    // button, then the envelopes, then + ENV, then the performance chips.
    const int contentWidth = juce::jmax (200, modViewport.getMaximumVisibleWidth());
    const int stripH = ShapeStrip::preferredHeight();
    const int buttonH = 22;
    const int numLfos = proc.getNumLfoModules();

    int y = 0;

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

    const int knobW = juce::jmax (70, barArea.getWidth() / 5);

    for (auto* k : macroKnobs)
        k->setBounds (barArea.removeFromLeft (knobW).reduced (2));

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
    return modPage != nullptr ? modPage->debugDump (slot) : juce::String ("no page");
}

void MiXXynthEditor::runTabPolls()
{
    // The VA page keeps its state readout current.
    if (vaPage != nullptr)
        vaPage->refreshHint();

    if (fmPage != nullptr)
        fmPage->refreshAudibility();

    // The FX page watches the slot types so a preset load repopulates the rack.
    if (fxPage != nullptr)
        fxPage->poll();

    // The Mod tab's rows are hand-bound, so they need polling too - otherwise a
    // modulation slot created by dragging a chip onto a knob never shows up here.
    if (modPage != nullptr)
        modPage->poll();
}

void MiXXynthEditor::rollFmDice()
{
    if (fmPage != nullptr)
        fmPage->rollDice();
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
