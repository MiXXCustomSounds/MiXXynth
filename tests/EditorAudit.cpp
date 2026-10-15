// Editor audit: verifies every knob is wired to the parameter and modulation
// destination it is supposed to be, that the destination name list agrees with
// the ModDest enum, that every parameter is reachable from the UI, and that the
// drag-and-drop modulation slot logic behaves.
//
// This is the automated half of the editor audit; the interactive half (drag
// every knob in the standalone) is recorded in docs/EDITOR-AUDIT.md.
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/Algorithms.h"
#include "../Source/ModKnob.h"
#include "../Source/ModMatrix.h"
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

namespace
{
    int failures = 0;

    void check (bool cond, const juce::String& name, const juce::String& detail = {})
    {
        if (! cond)
            ++failures;

        std::printf ("%s %-44s %s\n", cond ? "PASS" : "FAIL", name.toRawUTF8(),
                     detail.toRawUTF8());
    }

    // Expected modulation destination for a given parameter ID.
    // Anything not listed here must be dstOff (not modulatable).
    int expectedDest (const juce::String& id)
    {
        if (id == "fmIndex")     return mixxynth::dstFmIndex;
        if (id == "masterGain")  return mixxynth::dstMaster;
        if (id == "vaLevel")     return mixxynth::dstVaLevel;
        if (id == "vaDetune")    return mixxynth::dstVaDetune;
        if (id == "vaPulseWidth") return mixxynth::dstVaPulseWidth;
        if (id == "filterCutoff") return mixxynth::dstFilterCutoff;
        if (id == "filterReso")  return mixxynth::dstFilterReso;
        if (id == "filterDrive") return mixxynth::dstFilterDrive;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);

            if (id == "op" + n + "ratio") return mixxynth::dstOp1Ratio + i;
            if (id == "op" + n + "level") return mixxynth::dstOp1Level + i;
        }

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        {
            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                if (s == d)
                    continue;

                if (id == "mtx" + juce::String (s + 1) + juce::String (d + 1))
                    return mixxynth::dstMtxFirst + (s * (MiXXFmVoice::numOps - 1)) + (d > s ? d - 1 : d);
            }
        }

        return mixxynth::dstOff;
    }

    // Parameters driven by controls other than ModKnob (combos, toggles, sliders).
    std::set<juce::String> nonKnobUiParams()
    {
        std::set<juce::String> s { "algorithm", "vaWave", "vaOctave", "filterType", "filterOn",
                                   "mpeEnabled", "mpeMemberChannels" };

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            s.insert ("carrier" + juce::String (i + 1));

        // Per-operator enable is a toggle, not a knob.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            s.insert ("op" + juce::String (i + 1) + "on");

        for (int n = 0; n < mixxynth::kNumModSlots; ++n)
        {
            const juce::String id = juce::String (n + 1);
            s.insert ("mod" + id + "src");
            s.insert ("mod" + id + "dst");
        }

        return s;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    MiXXynthProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    check (editor != nullptr, "editor constructs", "");

    if (editor == nullptr)
        return 1;

    editor->setSize (1440, 900);

    auto* miXXEditor = dynamic_cast<MiXXynthEditor*> (editor.get());
    check (miXXEditor != nullptr, "editor is a MiXXynthEditor", "");

    if (miXXEditor == nullptr)
        return 1;

    // ---- 1. Destination name list agrees with the ModDest enum ----
    if (auto* dstParam = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("mod1dst")))
    {
        const auto& names = dstParam->choices;

        check (names.size() == mixxynth::kNumModDests, "destination count == kNumModDests",
               juce::String (names.size()) + " vs " + juce::String (mixxynth::kNumModDests));

        auto nameAt = [&names] (int i)
        {
            return juce::String (names[juce::jlimit (0, names.size() - 1, i)]);
        };

        check (nameAt (mixxynth::dstOff) == "Off", "name[Off]", nameAt (mixxynth::dstOff));
        check (nameAt (mixxynth::dstFmIndex) == "FM Index", "name[FmIndex]", nameAt (mixxynth::dstFmIndex));
        check (nameAt (mixxynth::dstOp1Ratio) == "Op1 Ratio", "name[Op1Ratio]", nameAt (mixxynth::dstOp1Ratio));
        check (nameAt (mixxynth::dstOp6Level) == "Op6 Level", "name[Op6Level]", nameAt (mixxynth::dstOp6Level));
        check (nameAt (mixxynth::dstMaster) == "Master", "name[Master]", nameAt (mixxynth::dstMaster));
        check (nameAt (mixxynth::dstVaLevel) == "VA Level", "name[VaLevel]", nameAt (mixxynth::dstVaLevel));
        check (nameAt (mixxynth::dstFilterDrive) == "Filter Drive", "name[FilterDrive]", nameAt (mixxynth::dstFilterDrive));
        check (nameAt (mixxynth::dstMtxFirst) == "Op1 > Op2", "name[first matrix cell]", nameAt (mixxynth::dstMtxFirst));
        check (nameAt (mixxynth::kNumModDests - 1) == "Op6 > Op5", "name[last matrix cell]", nameAt (mixxynth::kNumModDests - 1));
    }
    else
    {
        check (false, "mod1dst is a choice parameter", "");
    }

    // ---- 1b. Source name list agrees with the ModSource enum ----
    if (auto* srcParam = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("mod1src")))
    {
        const auto& names = srcParam->choices;

        check (names.size() == mixxynth::kNumModSources, "source count == kNumModSources",
               juce::String (names.size()) + " vs " + juce::String (mixxynth::kNumModSources));

        auto nameAt = [&names] (int i)
        {
            return juce::String (names[juce::jlimit (0, names.size() - 1, i)]);
        };

        check (nameAt (mixxynth::srcOff) == "Off", "src[Off]", nameAt (mixxynth::srcOff));
        check (nameAt (mixxynth::srcLfo1) == "LFO 1", "src[Lfo1]", nameAt (mixxynth::srcLfo1));
        check (nameAt (mixxynth::srcEnv1) == "Env 1", "src[Env1]", nameAt (mixxynth::srcEnv1));
        check (nameAt (mixxynth::srcMacro1) == "Macro 1", "src[Macro1]", nameAt (mixxynth::srcMacro1));
        check (nameAt (mixxynth::srcKeyTrack) == "Key Track", "src[KeyTrack]", nameAt (mixxynth::srcKeyTrack));
        check (nameAt (mixxynth::srcTimbre) == "Timbre", "src[Timbre]", nameAt (mixxynth::srcTimbre));
        check (names.size() == mixxynth::kNumModSources, "source name count matches kNumModSources",
               juce::String (names.size()) + " vs " + juce::String (mixxynth::kNumModSources));
    }
    else
    {
        check (false, "mod1src is a choice parameter", "");
    }

    // ---- 2. Every knob's parameter and destination ----
    std::map<juce::String, int> knobDest;
    juce::StringArray nullParams, badDests, mismatches;

    for (auto* k : miXXEditor->modKnobs)
    {
        if (k->getParam() == nullptr)
        {
            nullParams.add (k->getLabelText());
            continue;
        }

        const auto id = k->getParamID();
        const int dest = k->getDestIndex();

        if (dest < 0 || dest >= mixxynth::kNumModDests)
            badDests.add (id);

        if (knobDest.count (id) != 0 && knobDest[id] != dest)
            mismatches.add (id + " duplicated");

        knobDest[id] = dest;

        if (dest != expectedDest (id))
            mismatches.add (id + " -> " + juce::String (dest) + " expected "
                            + juce::String (expectedDest (id)));
    }

    check (nullParams.isEmpty(), "every knob has a parameter", nullParams.joinIntoString (", "));
    check (badDests.isEmpty(), "every knob dest in range", badDests.joinIntoString (", "));
    check (mismatches.isEmpty(), "every knob dest matches expectation", mismatches.joinIntoString (", "));

    // ---- 3. Every parameter reachable from the UI ----
    const auto nonKnob = nonKnobUiParams();
    juce::StringArray unreachable;
    int totalParams = 0;

    for (auto* p : proc.getParameters())
    {
        ++totalParams;

        auto* withId = dynamic_cast<juce::RangedAudioParameter*> (p);

        if (withId == nullptr)
            continue;

        const auto id = withId->getParameterID();

        if (knobDest.count (id) == 0 && nonKnob.count (id) == 0)
            unreachable.add (id);
    }

    check (unreachable.isEmpty(), "every parameter reachable from UI", unreachable.joinIntoString (", "));
    std::printf ("     params=%d knobs=%d\n", totalParams,
                 static_cast<int> (miXXEditor->modKnobs.size()));

    // ---- 4. Non-modulatable knobs are correctly identified ----
    int wrongTargetFlags = 0;
    int modTargetKnobs = 0, offKnobs = 0;

    for (auto* k : miXXEditor->modKnobs)
    {
        if (k->isModTarget() != (k->getDestIndex() != mixxynth::dstOff))
            ++wrongTargetFlags;

        (k->isModTarget() ? modTargetKnobs : offKnobs)++;
    }

    check (wrongTargetFlags == 0, "isModTarget matches dest", juce::String (wrongTargetFlags));
    std::printf ("     modulatable knobs=%d  non-modulatable knobs=%d\n", modTargetKnobs, offKnobs);

    // ---- 5. Drag-and-drop slot logic ----
    auto slotSrc = [&proc] (int n)
    {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (
                proc.apvts.getParameter ("mod" + juce::String (n + 1) + "src")))
            return c->getIndex();
        return -1;
    };

    auto slotDst = [&proc] (int n)
    {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (
                proc.apvts.getParameter ("mod" + juce::String (n + 1) + "dst")))
            return c->getIndex();
        return -1;
    };

    auto slotDepth = [&proc] (int n)
    {
        if (auto* r = proc.apvts.getRawParameterValue ("mod" + juce::String (n + 1) + "depth"))
            return r->load();
        return 0.0f;
    };

    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
        miXXEditor->clearModDest (slotDst (n));

    miXXEditor->assignModSlot (mixxynth::srcLfo1, mixxynth::dstFmIndex);
    check (slotSrc (0) == mixxynth::srcLfo1 && slotDst (0) == mixxynth::dstFmIndex,
           "drop assigns source and destination",
           juce::String (slotSrc (0)) + "/" + juce::String (slotDst (0)));
    check (std::abs (slotDepth (0)) > 0.0001f, "drop gives a non-zero depth",
           juce::String (slotDepth (0)));

    miXXEditor->assignModSlot (mixxynth::srcLfo1, mixxynth::dstFmIndex);
    check (slotDst (1) == mixxynth::dstOff, "same pair reuses its slot", juce::String (slotDst (1)));

    miXXEditor->assignModSlot (mixxynth::srcMacro2, mixxynth::dstFilterCutoff);
    check (slotSrc (1) == mixxynth::srcMacro2 && slotDst (1) == mixxynth::dstFilterCutoff,
           "second drop takes the next free slot",
           juce::String (slotSrc (1)) + "/" + juce::String (slotDst (1)));

    miXXEditor->assignModSlot (mixxynth::srcOff, mixxynth::dstMaster);
    check (slotDst (2) == mixxynth::dstOff, "Off source is refused", juce::String (slotDst (2)));

    miXXEditor->assignModSlot (mixxynth::srcLfo2, mixxynth::dstOff);
    check (slotDst (2) == mixxynth::dstOff, "Off destination is refused", juce::String (slotDst (2)));

    miXXEditor->clearModDest (mixxynth::dstFmIndex);
    check (slotDst (0) == mixxynth::dstOff && std::abs (slotDepth (0)) < 0.0001f,
           "clear empties its slot", juce::String (slotDst (0)));
    check (slotDst (1) == mixxynth::dstFilterCutoff, "clear leaves other slots alone",
           juce::String (slotDst (1)));

    miXXEditor->clearModDest (mixxynth::dstFilterCutoff);
    check (slotDst (1) == mixxynth::dstOff, "clear empties the second slot too",
           juce::String (slotDst (1)));

    // ---- 6. Depth ring bookkeeping ----
    auto masterRing = [&miXXEditor] ()
    {
        float v = 0.0f;

        for (auto* k : miXXEditor->modKnobs)
            if (k->getDestIndex() == mixxynth::dstMaster)
                v = k->getModDepth();

        return v;
    };

    miXXEditor->assignModSlot (mixxynth::srcLfo3, mixxynth::dstMaster);
    miXXEditor->refreshModRings();
    check (std::abs (masterRing()) > 0.0001f, "depth ring reflects assigned slot",
           juce::String (masterRing()));

    miXXEditor->clearModDest (mixxynth::dstMaster);
    miXXEditor->refreshModRings();
    check (std::abs (masterRing()) < 0.0001f, "depth ring clears with its slot",
           juce::String (masterRing()));

    // ---- 7. Modulation amount: the draggable depth ring's backend ----
    // Backs the Serum/Vital gesture of dragging the ring around a knob to set how
    // much a source modulates it.
    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
        miXXEditor->clearModDest (slotDst (n));

    miXXEditor->setModDepthFor (mixxynth::srcLfo1, mixxynth::dstFmIndex, 0.75f);
    check (slotSrc (0) == mixxynth::srcLfo1 && slotDst (0) == mixxynth::dstFmIndex,
           "setModDepthFor creates the pair", juce::String (slotSrc (0)) + "/" + juce::String (slotDst (0)));
    check (std::abs (slotDepth (0) - 0.75f) < 0.001f, "setModDepthFor sets the amount",
           juce::String (slotDepth (0)));

    miXXEditor->setModDepthFor (mixxynth::srcLfo1, mixxynth::dstFmIndex, -0.4f);
    check (std::abs (slotDepth (0) + 0.4f) < 0.001f, "setModDepthFor can go negative",
           juce::String (slotDepth (0)));
    check (slotDst (1) == mixxynth::dstOff, "setModDepthFor does not add a second slot",
           juce::String (slotDst (1)));

    // Dragging the ring of a knob that has no source yet must create one.
    miXXEditor->setModDepthFor (mixxynth::srcEnv2, mixxynth::dstVaPulseWidth, 0.5f);
    check (std::abs (slotDepth (0) - 0.5f) > 0.001f && slotDst (1) == mixxynth::dstVaPulseWidth,
           "setModDepthFor can target pulse width", juce::String (slotDst (1)));

    // Removing one pair must leave the other alone.
    miXXEditor->removeModSlot (mixxynth::srcLfo1, mixxynth::dstFmIndex);
    check (slotDst (0) == mixxynth::dstOff, "removeModSlot clears its own slot",
           juce::String (slotDst (0)));
    check (slotDst (1) == mixxynth::dstVaPulseWidth, "removeModSlot leaves other slots alone",
           juce::String (slotDst (1)));

    miXXEditor->clearModDest (mixxynth::dstVaPulseWidth);
    miXXEditor->refreshModRings();

    // Pulse Width is now a modulation destination, which the page's own text
    // claims and the DSP honours.
    {
        auto nameAt = [&proc] (int i)
        {
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (
                    proc.apvts.getParameter ("mod1dst")))
                return juce::String (c->choices[juce::jlimit (0, c->choices.size() - 1, i)]);

            return juce::String {};
        };

        check (nameAt (mixxynth::dstVaPulseWidth) == "VA Pulse Width",
               "VA Pulse Width is a modulation destination", nameAt (mixxynth::dstVaPulseWidth));
    }

    // ---- 8. Matrix page: routing edits must actually take effect ----
    // The routing knobs used to be inert whenever an algorithm preset was in
    // charge, because the processor overwrites the matrix from the algorithm
    // table. Editing one now seeds the matrix and switches to Free mode.
    {
        auto* algoParam = dynamic_cast<juce::AudioParameterChoice*> (
            proc.apvts.getParameter ("algorithm"));

        auto setChoice = [] (juce::AudioParameterChoice* p, int index)
        {
            if (p != nullptr)
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (index)));
                p->endChangeGesture();
            }
        };

        auto readReal = [&proc] (const juce::String& id)
        {
            if (auto* r = proc.apvts.getRawParameterValue (id))
                return r->load();

            return 0.0f;
        };

        // Start on DX7 algorithm 1 with an empty matrix.
        setChoice (algoParam, 0);

        for (int s = 1; s <= 6; ++s)
        {
            for (int d = 1; d <= 6; ++d)
            {
                if (s == d)
                    continue;

                if (auto* mtx = dynamic_cast<juce::AudioParameterFloat*> (
                        proc.apvts.getParameter ("mtx" + juce::String (s) + juce::String (d))))
                {
                    mtx->beginChangeGesture();
                    mtx->setValueNotifyingHost (0.0f);
                    mtx->endChangeGesture();
                }
            }
        }

        // Let the page settle its snapshot before the edit we want it to notice.
        miXXEditor->runTabPolls();
        miXXEditor->runTabPolls();

        {
            auto* mtx = dynamic_cast<juce::AudioParameterFloat*> (
                proc.apvts.getParameter ("mtx12"));

            if (mtx != nullptr)
            {
                mtx->beginChangeGesture();
                mtx->setValueNotifyingHost (mtx->convertTo0to1 (0.5f));
                mtx->endChangeGesture();
            }
        }

        miXXEditor->runTabPolls();

        check (algoParam != nullptr && algoParam->getIndex() == mixxynth::kNumAlgorithms,
               "routing edit switches the algorithm to Free mode",
               juce::String (algoParam != nullptr ? algoParam->getIndex() : -1));
        check (std::abs (readReal ("mtx12") - 0.5f) < 0.001f,
               "the edit that triggered the switch survives", juce::String (readReal ("mtx12")));

        // Algorithm 1 routes Op2 into Op1, so the matrix must have been seeded.
        check (readReal ("mtx21") > 0.5f, "the algorithm was copied into the matrix",
               juce::String (readReal ("mtx21")));

        // A state load must not be mistaken for a routing edit. Without the
        // state-load guard this switched the patch to Free mode behind the
        // user's back every time a preset was loaded.
        setChoice (algoParam, 0);
        miXXEditor->runTabPolls();
        miXXEditor->runTabPolls();

        juce::MemoryBlock saved;
        proc.getStateInformation (saved);

        if (auto* mtx = dynamic_cast<juce::AudioParameterFloat*> (
                proc.apvts.getParameter ("mtx34")))
        {
            mtx->beginChangeGesture();
            mtx->setValueNotifyingHost (0.9f);
            mtx->endChangeGesture();
        }

        proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        miXXEditor->runTabPolls();

        check (algoParam != nullptr && algoParam->getIndex() < mixxynth::kNumAlgorithms,
               "loading a state does not force Free mode",
               juce::String (algoParam != nullptr ? algoParam->getIndex() : -1));
    }

    // ---- 9. FM init patch is all zero, and the dice always produces sound ----
    {
        // Defaults, read through the parameter API rather than by constructing a
        // second processor.
        auto defReal = [&proc] (const juce::String& id)
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)))
                return p->convertFrom0to1 (p->getDefaultValue());

            return -999.0f;
        };

        bool allZero = std::abs (defReal ("fmIndex")) < 1.0e-6f;

        for (int i = 1; i <= 6; ++i)
        {
            const auto n = juce::String (i);

            for (const char* suffix : { "ratio", "level", "feedback", "attack", "decay", "sustain", "release", "wave" })
                if (std::abs (defReal ("op" + n + suffix)) > 1.0e-6f)
                    allZero = false;
        }

        check (allZero, "FM init patch is all zero",
               juce::String (defReal ("fmIndex")) + " / " + juce::String (defReal ("op1level")));

        auto readReal = [&proc] (const juce::String& id)
        {
            if (auto* r = proc.apvts.getRawParameterValue (id))
                return r->load();

            return 0.0f;
        };

        // The dice has to be able to make sound from that silent init patch.
        auto* algoParam = dynamic_cast<juce::AudioParameterChoice*> (
            proc.apvts.getParameter ("algorithm"));

        if (algoParam != nullptr)
        {
            algoParam->beginChangeGesture();
            algoParam->setValueNotifyingHost (algoParam->convertTo0to1 (0.0f));
            algoParam->endChangeGesture();
        }

        miXXEditor->rollFmDice();

        bool inRange = true;

        for (int i = 1; i <= 6; ++i)
        {
            const auto n = juce::String (i);

            inRange = inRange && readReal ("op" + n + "ratio") > 0.0f;
            inRange = inRange && readReal ("op" + n + "level") > 0.0f;
            inRange = inRange && readReal ("op" + n + "wave") >= 0.0f
                                && readReal ("op" + n + "wave") <= 1.0f;
            inRange = inRange && readReal ("op" + n + "feedback") <= 0.95f;
        }

        check (inRange, "dice: every op gets a usable ratio, level and wave",
               inRange ? "ok" : "out of range");
        check (readReal ("fmIndex") >= 0.0f && readReal ("fmIndex") <= 12.0f,
               "dice: fm index stays in range", juce::String (readReal ("fmIndex")));

        // At least one carrier must be able to sustain a note, or the roll reads
        // as a bug.
        bool audible = false;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            if (! mixxynth::algorithmTable()[0].carrier[static_cast<size_t> (i)])
                continue;

            const auto n = juce::String (i + 1);

            if (readReal ("op" + n + "level") > 0.3f
                  && readReal ("op" + n + "sustain") > 0.3f
                  && readReal ("op" + n + "ratio") > 0.0f)
                audible = true;
        }

        check (audible, "dice: the roll is always audible", audible ? "ok" : "silent");
    }

    // ---- 10. Layout: no visible control is left with zero bounds ----
    // This is the regression guard for the bug where setSize() ran before the
    // child arrays were populated, leaving controls at 0x0 and invisible.
    //
    // EVERY tab page is walked, not just the active one. The earlier version only
    // walked visible components, which is how the Mod tab's 32 slot rows sat at
    // 0x0 unnoticed: that tab was never active while the audit ran.
    juce::StringArray zeroSized;
    int visited = 0;

    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (child == nullptr || ! child->isVisible())
                continue;

            ++visited;

            if (child->getWidth() <= 0 || child->getHeight() <= 0)
                zeroSized.add (child->getName().isEmpty() ? juce::String ("<unnamed>")
                                                          : child->getName());

            walk (*child);
        }
    };

    walk (*miXXEditor);

    for (int i = 0; i < miXXEditor->getNumTabs(); ++i)
        if (auto* page = miXXEditor->getTabContentComponent (i))
            walk (*page);

    check (zeroSized.isEmpty(), "no visible control has zero bounds",
           zeroSized.joinIntoString (", "));
    std::printf ("     visible child components=%d\n", visited);
    std::printf ("     %s\n", miXXEditor->perfBarDebug().toRawUTF8());

    // Every tab must exist and be populated.
    check (miXXEditor->getNumTabs() == 6, "six tabs", juce::String (miXXEditor->getNumTabs()));

    for (int i = 0; i < miXXEditor->getNumTabs(); ++i)
    {
        auto* page = miXXEditor->getTabContentComponent (i);
        const auto name = miXXEditor->getTabNames()[i];

        check (page != nullptr && ! page->getChildren().isEmpty(),
               "tab " + juce::String (i) + " (" + name + ") has content",
               juce::String (page != nullptr ? page->getChildren().size() : 0));
    }

    // The modulation sidebar: one editable shape per LFO and ENV, always visible
    // because it is a sibling of the tabs rather than a tab's child.
    {
        const auto dbg = miXXEditor->sidebarDebug();
        std::printf ("     %s\n", dbg.toRawUTF8());

        check (dbg.contains ("strips=" + juce::String (mixxynth::kNumShapes)),
               "sidebar has one strip per shape", dbg.upToFirstOccurrenceOf (" chips=", false, false));

        const int minStripH = dbg.fromFirstOccurrenceOf ("minStripH=", false, false)
                                 .upToFirstOccurrenceOf (" ", false, false).getIntValue();
        const int allVisible = dbg.fromFirstOccurrenceOf ("allVisible=", false, false)
                                  .upToFirstOccurrenceOf (" ", false, false).getIntValue();
        const int allSized = dbg.fromFirstOccurrenceOf ("allSized=", false, false)
                                .upToFirstOccurrenceOf (" ", false, false).getIntValue();

        check (minStripH >= 100, "every shape strip has a usable height", juce::String (minStripH));
        check (allVisible == 1, "every shape strip is visible", juce::String (allVisible));
        check (allSized == 1, "every shape strip is sized", juce::String (allSized));
    }

    std::printf (failures == 0 ? "EDITOR AUDIT OK\n" : "EDITOR AUDIT FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
