#include "PluginEditor.h"

#include "EditorTheme.h"
#include "ModMatrix.h"
#include "PluginProcessor.h"

using mixxynth::ui::ModKnob;

namespace
{
    const char* const sourceNames[] = { "Off", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
                                        "Env 1", "Env 2", "Env 3", "MSEG",
                                        "Macro 1", "Macro 2", "Macro 3", "Macro 4",
                                        "Velocity", "Mod Wheel", "Aftertouch", "Key Track" };

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

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);

            addKnob (ranged (p, "op" + n + "ratio"), mixxynth::dstOp1Ratio + i, "Op" + n + " Ratio");
            addKnob (ranged (p, "op" + n + "level"), mixxynth::dstOp1Level + i, "Op" + n + " Level");
            addKnob (ranged (p, "op" + n + "feedback"), mixxynth::dstOff, "Op" + n + " Fb");
            addKnob (ranged (p, "op" + n + "attack"), mixxynth::dstOff, "Op" + n + " A");
            addKnob (ranged (p, "op" + n + "decay"), mixxynth::dstOff, "Op" + n + " D");
            addKnob (ranged (p, "op" + n + "sustain"), mixxynth::dstOff, "Op" + n + " S");
            addKnob (ranged (p, "op" + n + "release"), mixxynth::dstOff, "Op" + n + " R");
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("Drag a source chip onto any knob to modulate it; right-click a knob to clear its modulation.",
                          getLocalBounds().removeFromTop (18), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (18);

        auto top = b.removeFromTop (24);
        algoLabel.setBounds (top.removeFromLeft (80));
        algo.setBounds (top.removeFromLeft (230));

        b.removeFromTop (4);

        auto indexRow = b.removeFromTop (76);

        if (! knobs.isEmpty())
            knobs[0]->setBounds (indexRow.removeFromLeft (92));

        const int opCount = MiXXFmVoice::numOps;
        const int colW = juce::jmax (56, b.getWidth() / opCount);

        for (int i = 0; i < opCount; ++i)
        {
            auto col = b.removeFromLeft (colW).reduced (1);
            const int rows = 7;

            for (int k = 0; k < rows; ++k)
            {
                const int idx = 1 + i * rows + k;

                if (idx < knobs.size())
                {
                    auto cell = col.removeFromTop (juce::jmax (44, col.getHeight() / (rows - k)));
                    knobs[idx]->setBounds (cell.reduced (2));
                }
            }
        }
    }

private:
    void addKnob (juce::RangedAudioParameter* param, int dest, const juce::String& label)
    {
        auto* k = new ModKnob (label, param, dest, true);
        k->onSourceDropped = [this] (int s, int d) { ed.assignModSlot (s, d); };
        k->onClearRequested = [this] (int d) { ed.clearModDest (d); };
        addAndMakeVisible (k);
        ed.modKnobs.push_back (k);
        owned.add (k);
        knobs.add (k);
    }

    MiXXynthEditor& ed;
    juce::Label algoLabel;
    juce::ComboBox algo;
    juce::OwnedArray<ModKnob> owned;
    juce::Array<ModKnob*> knobs;
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

                const juce::String label = "Op" + juce::String (s + 1) + ">" + juce::String (d + 1);
                const juce::String id = "mtx" + juce::String (s + 1) + juce::String (d + 1);

                auto* k = new ModKnob (label, ranged (p, id), matrixDestFor (s, d), true);
                k->onSourceDropped = [this] (int src, int dst) { ed.assignModSlot (src, dst); };
                k->onClearRequested = [this] (int dst) { ed.clearModDest (dst); };
                addAndMakeVisible (k);
                ed.modKnobs.push_back (k);
                owned.add (k);
                knobs.add (k);
            }
        }

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);
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
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("Routing depths (modulator > carrier). In Free algorithm mode these define the patch; "
                          "an algorithm preset overrides them. Carrier switches choose what reaches the output.",
                          getLocalBounds().removeFromTop (18), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (18);

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
    MiXXynthEditor& ed;
    juce::OwnedArray<ModKnob> owned;
    juce::Array<ModKnob*> knobs;
    juce::OwnedArray<juce::ToggleButton> carrierToggles;
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

    private:
        juce::Label label;
        juce::ComboBox src, dst;
        ModKnob* depth = nullptr;
        juce::TextButton clear;
    };
}

// ---------------------------------------------------------------------------
// Mod page: 32 slot rows plus the LFO / envelope / MSEG source editors
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
        }

        content.setSize (420, mixxynth::kNumModSlots * rowHeight);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        // LFO editors.
        for (int n = 0; n < mixxynth::kNumLfos; ++n)
        {
            const juce::String id = juce::String (n + 1);

            auto* l = new juce::Label();
            l->setText ("LFO " + id, juce::dontSendNotification);
            lfoLabels.add (l);
            addAndMakeVisible (l);

            auto* c = new juce::ComboBox();
            lfoShapes.add (c);
            addAndMakeVisible (c);

            if (auto* shapeParam = choice (p, "lfo" + id + "shape"))
            {
                for (int i = 0; i < shapeParam->choices.size(); ++i)
                    c->addItem (shapeParam->choices[i], i + 1);

                c->setSelectedId (shapeParam->getIndex() + 1, juce::dontSendNotification);
                c->onChange = [c, shapeParam]
                {
                    shapeParam->beginChangeGesture();
                    shapeParam->setValueNotifyingHost (
                        shapeParam->convertTo0to1 (static_cast<float> (c->getSelectedId() - 1)));
                    shapeParam->endChangeGesture();
                };
            }

            auto* rate = new ModKnob ("Rate", ranged (p, "lfo" + id + "rate"), mixxynth::dstOff, true);
            lfoRates.add (rate);
            addAndMakeVisible (rate);

            auto* bip = new juce::ToggleButton ("bipolar");
            lfoBipolar.add (bip);
            addAndMakeVisible (bip);

            if (auto* param = dynamic_cast<juce::AudioParameterBool*> (ranged (p, "lfo" + id + "bipolar")))
            {
                bip->setToggleState (param->get(), juce::dontSendNotification);
                bip->onClick = [bip, param]
                {
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (bip->getToggleState() ? 1.0f : 0.0f);
                    param->endChangeGesture();
                };
            }
        }

        // Modulation envelopes.
        for (int n = 0; n < mixxynth::kNumModEnvs; ++n)
        {
            const juce::String id = juce::String (n + 1);

            auto* l = new juce::Label();
            l->setText ("Env " + id, juce::dontSendNotification);
            envLabels.add (l);
            addAndMakeVisible (l);

            const char* const suffix[4] = { "a", "d", "s", "r" };
            const char* const names[4] = { "A", "D", "S", "R" };

            for (int k = 0; k < 4; ++k)
            {
                auto* kn = new ModKnob (names[k], ranged (p, "menv" + id + suffix[k]), mixxynth::dstOff, true);
                envKnobs.add (kn);
                addAndMakeVisible (kn);
            }
        }

        // MSEG-lite steps.
        for (int i = 0; i < mixxynth::kNumMsegSteps; ++i)
        {
            auto* s = new juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox);
            msegSliders.add (s);
            addAndMakeVisible (s);

            if (auto* param = dynamic_cast<juce::AudioParameterFloat*> (ranged (p, "mseg" + juce::String (i + 1))))
            {
                s->setRange (param->range.start, param->range.end, 0.001);
                s->setValue (param->get(), juce::dontSendNotification);
                s->onValueChange = [s, param]
                {
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (s->getValue())));
                    param->endChangeGesture();
                };
            }
        }

        msegRate = new ModKnob ("MSEG Rate", ranged (p, "msegRate"), mixxynth::dstOff, true);
        addAndMakeVisible (msegRate);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (11.5f);
        g.drawFittedText ("32 slots. Drag a source chip from the left rail onto any knob, or set source/destination here. "
                          "Right-click a knob to clear its modulation.",
                          getLocalBounds().removeFromTop (18), juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        b.removeFromTop (18);

        auto right = b.removeFromRight (juce::jmax (300, b.getWidth() / 2));
        viewport.setBounds (b.reduced (0, 2));

        auto contentArea = right.reduced (4);
        const int rowH = 46;

        for (int n = 0; n < mixxynth::kNumLfos; ++n)
        {
            auto row = contentArea.removeFromTop (rowH);
            lfoLabels[n]->setBounds (row.removeFromLeft (48));
            lfoShapes[n]->setBounds (row.removeFromLeft (86).reduced (2, 8));
            lfoRates[n]->setBounds (row.removeFromLeft (62));
            lfoBipolar[n]->setBounds (row.removeFromLeft (80).reduced (2, 8));
        }

        contentArea.removeFromTop (4);

        for (int n = 0; n < mixxynth::kNumModEnvs; ++n)
        {
            auto row = contentArea.removeFromTop (rowH);
            envLabels[n]->setBounds (row.removeFromLeft (48));

            for (int k = 0; k < 4; ++k)
                envKnobs[n * 4 + k]->setBounds (row.removeFromLeft (juce::jmax (44, row.getWidth() / (4 - k))));
        }

        contentArea.removeFromTop (4);

        auto msegRow = contentArea.removeFromTop (juce::jmax (70, contentArea.getHeight() - 60));
        msegRate->setBounds (msegRow.removeFromRight (70));

        for (auto* s : msegSliders)
            s->setBounds (msegRow.removeFromLeft (juce::jmax (14, msegRow.getWidth() / mixxynth::kNumMsegSteps)).reduced (1));
    }

private:
    static constexpr int rowHeight = 26;

    MiXXynthEditor& ed;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<SlotRow> rows;
    juce::OwnedArray<juce::Label> lfoLabels;
    juce::OwnedArray<juce::ComboBox> lfoShapes;
    juce::OwnedArray<ModKnob> lfoRates;
    juce::OwnedArray<juce::ToggleButton> lfoBipolar;
    juce::OwnedArray<juce::Label> envLabels;
    juce::OwnedArray<ModKnob> envKnobs;
    juce::OwnedArray<juce::Slider> msegSliders;
    ModKnob* msegRate = nullptr;
};

// ---------------------------------------------------------------------------
// FX page: the fixed FX tail arrives in M4
// ---------------------------------------------------------------------------
class MiXXynthEditor::FxPage : public juce::Component
{
public:
    explicit FxPage (MiXXynthEditor&) {}

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff8b98a5));
        g.setFont (14.0f);
        g.drawFittedText ("FX tail (Hyper, Chorus, Delay, Reverb, Distortion, EQ, Limiter) lands in M4.",
                          getLocalBounds(), juce::Justification::centred, 2);
    }
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

    railTitle.setText ("MOD SOURCES", juce::dontSendNotification);
    railTitle.setJustificationType (juce::Justification::centred);
    railTitle.setColour (juce::Label::textColourId, juce::Colour (0xff8b98a5));
    rail.addAndMakeVisible (railTitle);
    addAndMakeVisible (rail);

    for (int i = 1; i < mixxynth::kNumModSources; ++i)
    {
        auto* c = new SourceChip (sourceNames[i], i);
        chips.add (c);
        rail.addAndMakeVisible (c);
    }

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
    }

    auto* master = new ModKnob ("Master", ranged (proc, "masterGain"), mixxynth::dstMaster, true);
    master->onSourceDropped = [this] (int s, int d) { assignModSlot (s, d); };
    master->onClearRequested = [this] (int d) { clearModDest (d); };
    bar.addAndMakeVisible (master);
    modKnobs.push_back (master);
    macroKnobs.add (master); // owned alongside the macros

    fmPage = std::make_unique<FmPage> (*this);
    matrixPage = std::make_unique<MatrixPage> (*this);
    modPage = std::make_unique<ModPage> (*this);
    fxPage = std::make_unique<FxPage> (*this);

    tabs.setOutline (0);
    tabs.addTab ("FM", juce::Colour (0xff14181d), fmPage.get(), false);
    tabs.addTab ("Matrix", juce::Colour (0xff14181d), matrixPage.get(), false);
    tabs.addTab ("Mod", juce::Colour (0xff14181d), modPage.get(), false);
    tabs.addTab ("FX", juce::Colour (0xff14181d), fxPage.get(), false);
    addAndMakeVisible (tabs);

    setSize (1440, 900);
    startTimerHz (12);
}

MiXXynthEditor::~MiXXynthEditor() = default;

void MiXXynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b0d10));
    g.setColour (juce::Colour (0xff1b2027));
    g.fillRect (rail.getBounds());
    g.fillRect (bar.getBounds());
}

void MiXXynthEditor::resized()
{
    auto b = getLocalBounds();

    title.setBounds (b.removeFromTop (34).reduced (10, 4));

    rail.setBounds (b.removeFromLeft (140));
    bar.setBounds (b.removeFromBottom (86));

    tabs.setBounds (b);

    auto railArea = rail.getLocalBounds().reduced (6);
    railTitle.setBounds (railArea.removeFromTop (18));

    for (auto* c : chips)
        c->setBounds (railArea.removeFromTop (24).reduced (2, 1));

    auto barArea = bar.getLocalBounds().reduced (8);
    barTitle.setBounds (barArea.removeFromLeft (64));

    const int knobW = juce::jmax (70, barArea.getWidth() / 5);

    for (auto* k : macroKnobs)
        k->setBounds (barArea.removeFromLeft (knobW).reduced (2));
}

void MiXXynthEditor::timerCallback()
{
    refreshModRings();
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
        if (std::abs (dep->getValue()) < 0.0001f)
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
    std::array<float, mixxynth::kNumModDests> totals {};
    totals.fill (0.0f);

    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto* s = choice (proc, "mod" + id + "src");
        auto* d = choice (proc, "mod" + id + "dst");

        if (s == nullptr || d == nullptr)
            continue;

        if (s->getIndex() == mixxynth::srcOff || d->getIndex() == mixxynth::dstOff)
            continue;

        const int dest = d->getIndex();

        if (dest >= 0 && dest < mixxynth::kNumModDests)
            if (auto* dep = ranged (proc, "mod" + id + "depth"))
                totals[static_cast<size_t> (dest)] += dep->getValue();
    }

    for (auto* k : modKnobs)
    {
        const int dest = k->getDestIndex();

        if (dest >= 0 && dest < mixxynth::kNumModDests)
            k->setModDepth (totals[static_cast<size_t> (dest)]);
    }
}
