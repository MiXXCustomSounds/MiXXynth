#include "PluginEditor.h"

#include "Algorithms.h"
#include "EditorTheme.h"
#include "ModMatrix.h"
#include "PluginProcessor.h"

using mixxynth::ui::ModKnob;

namespace
{
    const char* const sourceNames[] = { "Off", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
                                        "Env 1", "Env 2", "Env 3",
                                        "Macro 1", "Macro 2", "Macro 3", "Macro 4",
                                        "Velocity", "Mod Wheel", "Aftertouch", "Key Track",
                                        "Timbre" };

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
        const bool isLfo = index < mixxynth::kNumLfos;
        const int src = isLfo ? (mixxynth::srcLfo1 + index)
                              : (mixxynth::srcEnv1 + (index - mixxynth::kNumLfos));

        chip = std::make_unique<SourceChip> (sourceNames[src], src);
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
        return index < mixxynth::kNumLfos ? "LFO " + juce::String (index + 1)
                                          : "Env " + juce::String (index - mixxynth::kNumLfos + 1);
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
    juce::TextButton resetButton;
    juce::Slider rateBar, smoothBar, phaseBar, delayBar;
    juce::TextButton syncButton, bipolarButton, retrigButton;
    juce::ComboBox divBox;
};



// ---------------------------------------------------------------------------
// FM page: algorithm + FM index + per-op ratio/level/feedback/ADSR
// ---------------------------------------------------------------------------
class MiXXynthEditor::FmPage : public juce::Component
{
public:
    explicit FmPage (MiXXynthEditor& e) : ed (e)
    {
        auto& p = e.processor();

        algoLabel.setText ("Algorithm", juce::dontSendNotification);
        algoLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (algoLabel);

        if (auto* algoParam = choice (p, "algorithm"))
        {
            for (int i = 0; i < algoParam->choices.size(); ++i)
                algo.addItem (algoParam->choices[i], i + 1);

            algo.setSelectedId (algoParam->getIndex() + 1, juce::dontSendNotification);
            algo.onChange = [this, algoParam]
            {
                algoParam->beginChangeGesture();
                algoParam->setValueNotifyingHost (
                    algoParam->convertTo0to1 (static_cast<float> (algo.getSelectedId() - 1)));
                algoParam->endChangeGesture();
            };
        }

        addAndMakeVisible (algo);
        addKnob (ranged (p, "fmIndex"), mixxynth::dstFmIndex, "FM Index");

        diceButton.setButtonText ("Dice");
        diceButton.setTooltip ("Randomise every knob on this page");
        diceButton.onClick = [this] { rollDice(); };
        addAndMakeVisible (diceButton);

        audibility.setJustificationType (juce::Justification::centredLeft);
        audibility.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (audibility);

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);

            // Per-operator enable. A disabled operator outputs silence, which
            // also stops it modulating anything downstream.
            auto* t = new juce::ToggleButton ("Op" + n);
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

            // Wave first, so the column reads: shape, then its settings.
            addKnob (ranged (p, "op" + n + "wave"), mixxynth::dstOff, "Op" + n + " Wave");
            addKnob (ranged (p, "op" + n + "ratio"), mixxynth::dstOp1Ratio + i, "Op" + n + " Ratio");
            addKnob (ranged (p, "op" + n + "level"), mixxynth::dstOp1Level + i, "Op" + n + " Level");
            addKnob (ranged (p, "op" + n + "feedback"), mixxynth::dstOff, "Op" + n + " Fb");
            addKnob (ranged (p, "op" + n + "attack"), mixxynth::dstOff, "Op" + n + " A");
            addKnob (ranged (p, "op" + n + "decay"), mixxynth::dstOff, "Op" + n + " D");
            addKnob (ranged (p, "op" + n + "sustain"), mixxynth::dstOff, "Op" + n + " S");
            addKnob (ranged (p, "op" + n + "release"), mixxynth::dstOff, "Op" + n + " R");
        }

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

    // Keeps each Wave knob's label showing the shape it is currently at, so the
    // morph is readable instead of a mystery dial.
    void refreshWaveLabels()
    {
        constexpr int knobsPerOp = 8;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const int idx = 1 + i * knobsPerOp;

            if (idx >= knobs.size())
                break;

            float wave = 0.0f;

            if (auto* p = ranged (ed.processor(), "op" + juce::String (i + 1) + "wave"))
                wave = p->convertFrom0to1 (p->getValue());

            knobs[idx]->setLabelText ("Op" + juce::String (i + 1) + " "
                                        + mixxynth::OpWaveTables::shapeName (wave));
        }
    }

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
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("Op toggles enable or silence an operator. Wave sweeps sine > triangle > saw > square; "
                          "the knob label shows where you are. Drag a source chip onto any knob to modulate it.",
                          getLocalBounds().removeFromTop (18), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (18);

        auto top = b.removeFromTop (24);
        algoLabel.setBounds (top.removeFromLeft (80));
        algo.setBounds (top.removeFromLeft (230));
        top.removeFromLeft (10);
        diceButton.setBounds (top.removeFromLeft (72).reduced (2, 0));

        audibility.setBounds (b.removeFromTop (16));

        b.removeFromTop (4);

        // FM Index gets its own narrow column; the operator grid takes the rest.
        auto indexColumn = b.removeFromLeft (116);

        if (! knobs.isEmpty())
            knobs[0]->setBounds (indexColumn.removeFromTop (juce::jmin (indexColumn.getWidth(), 120)));

        // One operator per row with its eight knobs across, rather than eight rows
        // per operator. The earlier six-column layout left each dial about 31 px,
        // which is what made them unusable.
        const int rows = 8;
        const int rowH = juce::jmax (64, b.getHeight() / MiXXFmVoice::numOps);
        const int labelW = 60;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            auto row = b.removeFromTop (rowH);

            if (i < opOnToggles.size())
                opOnToggles[i]->setBounds (row.removeFromLeft (labelW).reduced (4, 2));

            const int colW = juce::jmax (48, row.getWidth() / rows);

            for (int k = 0; k < rows; ++k)
            {
                const int idx = 1 + i * rows + k;

                if (idx < knobs.size())
                    knobs[idx]->setBounds (row.removeFromLeft (colW).reduced (2));
            }
        }
    }

private:
    void addKnob (juce::RangedAudioParameter* param, int dest, const juce::String& label)
    {
        auto* k = new ModKnob (label, param, dest, true);
        ed.wireModKnob (*k);
        addAndMakeVisible (k);
        ed.modKnobs.push_back (k);
        owned.add (k);
        knobs.add (k);
    }

    MiXXynthEditor& ed;
    juce::Label algoLabel;
    juce::ComboBox algo;
    juce::TextButton diceButton;
    juce::Label audibility;
    juce::Random rng;
    juce::OwnedArray<ModKnob> owned;
    juce::Array<ModKnob*> knobs;
    juce::OwnedArray<juce::ToggleButton> opOnToggles;
};

// ---------------------------------------------------------------------------
// Matrix page: the 30 off-diagonal routing depths + 6 carrier switches
// ---------------------------------------------------------------------------
class MiXXynthEditor::MatrixPage : public juce::Component
{
public:
    explicit MatrixPage (MiXXynthEditor& e) : ed (e)
    {
        auto& p = e.processor();

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        {
            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                if (s == d)
                    continue;

                const juce::String label = "Op" + juce::String (s + 1) + " > " + juce::String (d + 1);
                const juce::String id = "mtx" + juce::String (s + 1) + juce::String (d + 1);

                auto* k = new ModKnob (label, ranged (p, id), matrixDestFor (s, d), true);
                ed.wireModKnob (*k);
                addAndMakeVisible (k);
                ed.modKnobs.push_back (k);
                owned.add (k);
                knobs.add (k);
                mtxIds.add (id);
            }
        }

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);
            carrierIds.add ("carrier" + n);

            auto* t = new juce::ToggleButton ("Op" + n + " carrier");
            carrierToggles.add (t);
            addAndMakeVisible (t);

            if (auto* param = dynamic_cast<juce::AudioParameterBool*> (ranged (p, "carrier" + n)))
            {
                t->setToggleState (param->get(), juce::dontSendNotification);
                t->onClick = [t, param]
                {
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (t->getToggleState() ? 1.0f : 0.0f);
                    param->endChangeGesture();
                };
            }
        }

        copyButton.setButtonText ("Copy algorithm to matrix");
        copyButton.onClick = [this]
        {
            copyAlgorithmToMatrix (algorithmIndex());
            setAlgorithmFree();
            refreshSnapshots();
            refreshMode();
            ed.setModStatus ("Algorithm copied into the matrix; routing is now Free");
        };
        addAndMakeVisible (copyButton);

        freeButton.setButtonText ("Free mode");
        freeButton.onClick = [this] { setAlgorithmFree(); refreshMode(); };
        addAndMakeVisible (freeButton);

        modeLabel.setJustificationType (juce::Justification::centredLeft);
        modeLabel.setFont (juce::FontOptions (11.5f));
        addAndMakeVisible (modeLabel);

        refreshSnapshots();
        refreshMode();
    }

    // Driven from the editor's timer. The routing knobs used to be dead whenever
    // an algorithm preset was selected, because the processor overwrites the
    // matrix from the algorithm table. Now editing a routing knob (or a carrier
    // switch) while an algorithm is in charge seeds the matrix from that
    // algorithm, re-applies the edit on top, and switches to Free mode.
    void poll()
    {
        const int algo = algorithmIndex();

        // A preset or project load replaces every parameter at once. Resync the
        // snapshots instead of reading that as a routing edit and switching the
        // patch into Free mode behind the user's back.
        if (ed.processor().getStateLoadCount() != lastStateLoad)
        {
            lastStateLoad = ed.processor().getStateLoadCount();
            refreshSnapshots();
            refreshMode();
            return;
        }

        if (algo != lastAlgo)
        {
            // Leaving an algorithm for Free mode would otherwise drop to silence,
            // so seed the matrix from the algorithm we are leaving.
            if (algo >= mixxynth::kNumAlgorithms && lastAlgo < mixxynth::kNumAlgorithms
                  && matrixIsSilent())
                copyAlgorithmToMatrix (lastAlgo);

            lastAlgo = algo;
            refreshSnapshots();
            refreshMode();
            return;
        }

        if (algo >= mixxynth::kNumAlgorithms)
            return; // already Free: edits are live, nothing to migrate

        int changedMtx = -1;
        float changedMtxValue = 0.0f;
        int changedCarrier = -1;
        bool changedCarrierValue = false;

        for (int i = 0; i < mtxIds.size(); ++i)
        {
            const float v = readParam (mtxIds[i]);

            if (! juce::approximatelyEqual (v, lastMtx[static_cast<size_t> (i)]))
            {
                changedMtx = i;
                changedMtxValue = v;
            }
        }

        for (int i = 0; i < carrierIds.size(); ++i)
        {
            const float v = readParam (carrierIds[i]);

            if (! juce::approximatelyEqual (v, lastCarrier[static_cast<size_t> (i)]))
            {
                changedCarrier = i;
                changedCarrierValue = v > 0.5f;
            }
        }

        if (changedMtx < 0 && changedCarrier < 0)
            return;

        copyAlgorithmToMatrix (algo);
        setAlgorithmFree();

        // Re-apply the edit that triggered the switch, on top of the copy.
        if (changedMtx >= 0)
            writeParam (mtxIds[changedMtx], changedMtxValue);

        if (changedCarrier >= 0)
            writeParam (carrierIds[changedCarrier], changedCarrierValue ? 1.0f : 0.0f);

        refreshSnapshots();
        refreshMode();

        ed.setModStatus ("Routing edit switched the algorithm to Free mode");
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);

        auto banner = b.removeFromTop (26);
        copyButton.setBounds (banner.removeFromRight (176).reduced (2, 0));
        freeButton.setBounds (banner.removeFromRight (86).reduced (2, 0));
        modeLabel.setBounds (banner.reduced (2, 0));

        b.removeFromTop (4);

        auto carriers = b.removeFromBottom (26);

        for (auto* t : carrierToggles)
            t->setBounds (carriers.removeFromLeft (juce::jmax (70, carriers.getWidth() / 6)).reduced (2));

        const int colW = juce::jmax (60, b.getWidth() / MiXXFmVoice::numOps);
        const int rowH = juce::jmax (50, b.getHeight() / MiXXFmVoice::numOps);

        int k = 0;

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        {
            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                if (s == d)
                    continue;

                if (k < knobs.size())
                    knobs[k]->setBounds (juce::Rectangle<int> (b.getX() + d * colW, b.getY() + s * rowH,
                                                               colW, rowH).reduced (3));

                ++k;
            }
        }
    }

private:
    static constexpr int numMtx = MiXXFmVoice::numOps * (MiXXFmVoice::numOps - 1);

    int algorithmIndex()
    {
        if (auto* a = choice (ed.processor(), "algorithm"))
            return a->getIndex();

        return 0;
    }

    float readParam (const juce::String& id)
    {
        if (auto* p = ranged (ed.processor(), id))
            return p->convertFrom0to1 (p->getValue());

        return 0.0f;
    }

    void writeParam (const juce::String& id, float realValue)
    {
        if (auto* p = ranged (ed.processor(), id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (realValue));
            p->endChangeGesture();
        }
    }

    void setAlgorithmFree()
    {
        if (auto* a = choice (ed.processor(), "algorithm"))
        {
            a->beginChangeGesture();
            a->setValueNotifyingHost (a->convertTo0to1 (static_cast<float> (mixxynth::kNumAlgorithms)));
            a->endChangeGesture();
        }
    }

    void copyAlgorithmToMatrix (int algoIndex)
    {
        if (algoIndex < 0 || algoIndex >= mixxynth::kNumAlgorithms)
            return;

        const auto& algo = mixxynth::algorithmTable()[static_cast<size_t> (algoIndex)];

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
                if (s != d)
                    writeParam ("mtx" + juce::String (s + 1) + juce::String (d + 1),
                                algo.mod[static_cast<size_t> (s)][static_cast<size_t> (d)]);

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            writeParam ("carrier" + juce::String (i + 1),
                        algo.carrier[static_cast<size_t> (i)] ? 1.0f : 0.0f);
    }

    bool matrixIsSilent()
    {
        for (const auto& id : mtxIds)
            if (readParam (id) > 0.0001f)
                return false;

        return true;
    }

    void refreshSnapshots()
    {
        for (int i = 0; i < mtxIds.size(); ++i)
            lastMtx[static_cast<size_t> (i)] = readParam (mtxIds[i]);

        for (int i = 0; i < carrierIds.size(); ++i)
            lastCarrier[static_cast<size_t> (i)] = readParam (carrierIds[i]);

        lastAlgo = algorithmIndex();
    }

    void refreshMode()
    {
        const int algo = algorithmIndex();
        const bool free = algo >= mixxynth::kNumAlgorithms;

        modeLabel.setText (free
                             ? "Free (matrix) routing: these depths and switches define the patch."
                             : "Algorithm " + juce::String (algo + 1)
                                 + " defines the routing. Move a knob or switch to copy it here and go Free.",
                           juce::dontSendNotification);

        modeLabel.setColour (juce::Label::textColourId,
                             free ? juce::Colour (0xff4ade80) : juce::Colour (0xffffa63d));

        copyButton.setEnabled (! free);
    }

    MiXXynthEditor& ed;
    juce::OwnedArray<ModKnob> owned;
    juce::Array<ModKnob*> knobs;
    juce::OwnedArray<juce::ToggleButton> carrierToggles;
    juce::StringArray mtxIds, carrierIds;
    juce::Label modeLabel;
    juce::TextButton copyButton, freeButton;
    std::array<float, numMtx> lastMtx {};
    std::array<float, MiXXFmVoice::numOps> lastCarrier {};
    int lastAlgo = 0;
    int lastStateLoad = 0;
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

            if (auto* srcParam = choice (p, "mod" + n + "src"))
            {
                for (int i = 0; i < srcParam->choices.size(); ++i)
                    src.addItem (srcParam->choices[i], i + 1);

                src.setSelectedId (srcParam->getIndex() + 1, juce::dontSendNotification);
                src.onChange = [this, srcParam]
                {
                    srcParam->beginChangeGesture();
                    srcParam->setValueNotifyingHost (
                        srcParam->convertTo0to1 (static_cast<float> (src.getSelectedId() - 1)));
                    srcParam->endChangeGesture();
                };
            }

            if (auto* dstParam = choice (p, "mod" + n + "dst"))
            {
                for (int i = 0; i < dstParam->choices.size(); ++i)
                    dst.addItem (dstParam->choices[i], i + 1);

                dst.setSelectedId (dstParam->getIndex() + 1, juce::dontSendNotification);
                dst.onChange = [this, dstParam]
                {
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
            ed.modKnobs.push_back (row->getDepthKnob());
        }

        content.setSize (420, mixxynth::kNumModSlots * rowHeight);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

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
        addKnob (ranged (p, "vaPulseWidth"), mixxynth::dstVaPulseWidth, "Pulse Width");
        addKnob (ranged (p, "filterCutoff"), mixxynth::dstFilterCutoff, "Cutoff");
        addKnob (ranged (p, "filterReso"), mixxynth::dstFilterReso, "Resonance");
        addKnob (ranged (p, "filterDrive"), mixxynth::dstFilterDrive, "Drive");

        addChoice (p, "vaWave", wave);
        addChoice (p, "filterType", filterType);

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
        wave.setBounds (controls.removeFromLeft (130).reduced (2, 0));
        controls.removeFromLeft (16);
        filterType.setBounds (controls.removeFromLeft (130).reduced (2, 0));
        controls.removeFromLeft (16);

        if (filterOn != nullptr)
            filterOn->setBounds (controls.removeFromLeft (100).reduced (2, 0));

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
        ed.wireModKnob (*k);
        addAndMakeVisible (k);
        ed.modKnobs.push_back (k);
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
    juce::ComboBox wave, filterType;
    juce::ToggleButton* filterOn = nullptr;
    juce::Slider* octave = nullptr;
    juce::Label octaveLabel;
    juce::Label hint;
    juce::OwnedArray<juce::ToggleButton> toggles;
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
        ed.modKnobs.push_back (k);
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
// FX page: the fixed FX tail arrives in M4
// ---------------------------------------------------------------------------
class MiXXynthEditor::FxPage : public juce::Component
{
public:
    explicit FxPage (MiXXynthEditor&)
    {
        message.setText ("FX tail (Hyper, Chorus, Delay, Reverb, Distortion, EQ, Limiter) lands in M4.",
                         juce::dontSendNotification);
        message.setJustificationType (juce::Justification::centred);
        message.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
        addAndMakeVisible (message);
    }

    void resized() override { message.setBounds (getLocalBounds().reduced (20)); }

private:
    juce::Label message;
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

    // The sidebar is a scrollable stack of editable shapes, one per LFO and ENV.
    rail.addAndMakeVisible (modViewport);
    modViewport.setViewedComponent (&modContent, false);
    modViewport.setScrollBarsShown (true, false);

    for (int i = 0; i < mixxynth::kNumShapes; ++i)
    {
        auto* strip = new ShapeStrip (*this, i);
        shapeStrips.add (strip);
        modContent.addAndMakeVisible (strip);
    }

    // Performance sources keep the chip workflow: macros plus the MIDI inputs.
    perfChipTitle.setText ("PERFORMANCE  (drag onto a knob)", juce::dontSendNotification);
    perfChipTitle.setJustificationType (juce::Justification::centredLeft);
    perfChipTitle.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    perfChipTitle.setFont (juce::FontOptions (10.5f));
    modContent.addAndMakeVisible (perfChipTitle);

    for (int i = mixxynth::srcMacro1; i < mixxynth::kNumModSources; ++i)
    {
        auto* c = new SourceChip (sourceNames[i], i);
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
        modKnobs.push_back (k);
    }

    auto* master = new ModKnob ("Master", ranged (proc, "masterGain"), mixxynth::dstMaster, true);
    wireModKnob (*master);
    bar.addAndMakeVisible (master);
    modKnobs.push_back (master);
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

    fmPage = std::make_unique<FmPage> (*this);
    matrixPage = std::make_unique<MatrixPage> (*this);
    modPage = std::make_unique<ModPage> (*this);
    vaPage = std::make_unique<VaPage> (*this);
    voicePage = std::make_unique<VoicePage> (*this);
    fxPage = std::make_unique<FxPage> (*this);

    tabs.setOutline (0);
    tabs.addTab ("FM", juce::Colour (0xff14181d), fmPage.get(), false);
    tabs.addTab ("Matrix", juce::Colour (0xff14181d), matrixPage.get(), false);
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

    // Lay the strips out inside the scrolled content.
    const int contentWidth = juce::jmax (200, modViewport.getMaximumVisibleWidth());
    const int stripH = ShapeStrip::preferredHeight();

    int y = 0;

    for (auto* s : shapeStrips)
    {
        s->setBounds (0, y, contentWidth, stripH);
        y += stripH + 4;
    }

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

void MiXXynthEditor::runTabPolls()
{
    // The matrix page watches for routing edits that need to switch the patch
    // into Free mode, and the VA page keeps its state readout current.
    if (matrixPage != nullptr)
        matrixPage->poll();

    if (vaPage != nullptr)
        vaPage->refreshHint();

    if (fmPage != nullptr)
    {
        fmPage->refreshWaveLabels();
        fmPage->refreshAudibility();
    }
}

void MiXXynthEditor::rollFmDice()
{
    if (fmPage != nullptr)
        fmPage->rollDice();
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
    std::array<std::vector<mixxynth::ui::ModKnob::Assignment>, mixxynth::kNumModDests> byDest;

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

    for (auto* k : modKnobs)
    {
        if (k->isAdjustingRing())
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
    for (auto* k : modKnobs)
    {
        const int dest = k->getDestIndex();

        if (dest < 0 || dest >= mixxynth::kNumModDests || k->isAdjustingRing())
            continue;

        k->setModAssignments (byDest[static_cast<size_t> (dest)]);
    }
}

void MiXXynthEditor::wireModKnob (mixxynth::ui::ModKnob& k)
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
