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

        // Unbuffered: a crash would otherwise discard everything printed so far,
        // which makes a segfault impossible to locate.
        std::fflush (stdout);
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
        // Read the parameter's own range rather than hard-coding it, so this
        // check follows the range instead of silently passing.
        float fmIndexMax = 0.0f;

        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter ("fmIndex")))
            fmIndexMax = p->getNormalisableRange().end;

        check (readReal ("fmIndex") > 0.0f && readReal ("fmIndex") <= fmIndexMax,
               "dice: fm index stays in range",
               juce::String (readReal ("fmIndex")) + " of " + juce::String (fmIndexMax));

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

    // ---- 10. FM Index range guard ----
    // FM Index is applied as a phase offset in CYCLES, so index 1.0 is already a
    // modulation index of about 6.3 radians. The musical range is roughly 0..0.6
    // and anything past ~2 is noise. The old 0..12 range put all the useful
    // change in the bottom 5% of the knob; these checks stop that returning.
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter ("fmIndex")))
    {
        const auto& r = p->getNormalisableRange();

        check (r.end <= 3.0f, "fm index max stays in the musical range",
               juce::String (r.end));
        check (r.convertFrom0to1 (0.5f) < 0.8f, "fm index midpoint is in the useful range",
               juce::String (r.convertFrom0to1 (0.5f)));
        check (r.convertFrom0to1 (0.25f) < 0.3f, "fm index quarter point is fine control",
               juce::String (r.convertFrom0to1 (0.25f)));
    }
    else
    {
        check (false, "fmIndex is a ranged parameter", "");
    }

    // ---- 11. Operator waveform reaches the audio output end-to-end ----
    // This goes through the real processor, so it covers the parameter ->
    // VoiceParams path as well as the DSP. The smoke test only drives the voice
    // directly, so a wiring mistake between the parameter and the voice would
    // slip past it.
    {
        proc.prepareToPlay (48000.0, 512);

        auto setReal = [&proc] (const juce::String& id, float realValue)
        {
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 (realValue));
                p->endChangeGesture();
            }
        };

        // A patch that actually sounds. Algorithm 1 has Op1 as a carrier.
        setReal ("algorithm", 0.0f);
        setReal ("fmIndex", 0.0f);
        setReal ("op1ratio", 1.0f);
        setReal ("op1level", 0.8f);
        setReal ("op1attack", 0.001f);
        setReal ("op1decay", 0.5f);
        setReal ("op1sustain", 1.0f);
        setReal ("op1release", 0.1f);
        setReal ("vaLevel", 0.0f); // VA off, so only the FM operator is heard
        setReal ("masterGain", 0.8f);

        auto render = [&proc, &setReal] (float wave)
        {
            setReal ("op1wave", wave);

            juce::AudioBuffer<float> buf (2, 4096);
            buf.clear();

            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            proc.processBlock (buf, midi);

            return buf;
        };

        const auto sineBuf = render (0.0f);
        const auto sawBuf = render (0.667f);
        const auto squareBuf = render (1.0f);

        auto diff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            double d = 0.0;

            for (int i = 0; i < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++i)
                d += std::abs (a.getSample (0, i) - b.getSample (0, i));

            return d;
        };

        auto rms = [] (const juce::AudioBuffer<float>& b)
        {
            double acc = 0.0;

            for (int i = 0; i < b.getNumSamples(); ++i)
                acc += static_cast<double> (b.getSample (0, i)) * b.getSample (0, i);

            return std::sqrt (acc / juce::jmax (1, b.getNumSamples()));
        };

        const double sineRms = rms (sineBuf);
        const double sineVsSaw = diff (sineBuf, sawBuf);
        const double sawVsSquare = diff (sawBuf, squareBuf);

        check (sineRms > 0.01, "end-to-end: the test patch is audible", juce::String (sineRms));
        check (sineVsSaw > 1.0, "end-to-end: the wave knob changes the output",
               juce::String (sineVsSaw));
        check (sawVsSquare > 1.0, "end-to-end: saw and square differ",
               juce::String (sawVsSquare));
    }

    // ---- 12. External parameter changes notify the widget ----
    // Backs the bug where pressing the dice randomised the values but the dials
    // only redrew once you moused over them. Only setValueFromGUI and
    // setValueFromModel used to notify, so any change from outside the widget -
    // the dice, a preset load, host automation, undo - left it stale.
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*> (
            proc.apvts.getParameter ("fmIndex"));

        if (param == nullptr)
        {
            check (false, "fmIndex exists for the source test", "");
        }
        else
        {
            struct CountingListener : sst::jucegui::data::Continuous::DataListener
            {
                void dataChanged() override { ++count; }
                void sourceVanished (sst::jucegui::data::Continuous*) override {}
                int count = 0;
            };

            mixxynth::ui::ContinuousSource src (param);
            CountingListener listener;
            src.addGUIDataListener (&listener);

            // Change the parameter the way anything outside the widget would.
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
            param->endChangeGesture();

            check (listener.count > 0, "external parameter changes repaint the widget",
                   juce::String (listener.count));

            src.removeGUIDataListener (&listener);
        }
    }

    // ---- 13. Knob drag space matches the dial ----
    // sst-jucegui's continuous editor drags in REAL value space scaled by
    // getMax() - getMin() (150 px for the whole range), while the dial is drawn
    // from getValue01(). For a skewed or log range those disagree, which made the
    // filter cutoff move ~133 Hz per pixel while its dial moved logarithmically.
    // The adapter reports normalised 0..1 so both share one space.
    {
        auto* cutoff = dynamic_cast<juce::RangedAudioParameter*> (
            proc.apvts.getParameter ("filterCutoff"));

        if (cutoff == nullptr)
        {
            check (false, "filterCutoff exists for the drag test", "");
        }
        else
        {
            mixxynth::ui::ContinuousSource src (cutoff);

            check (std::abs ((src.getMax() - src.getMin()) - 1.0f) < 1.0e-6f,
                   "knob drag range is normalised",
                   juce::String (src.getMax() - src.getMin()));

            // Equal normalised travel must give equal octaves, which is the
            // property that makes the drag feel even across the whole knob.
            const auto& r = cutoff->getNormalisableRange();
            const float low = r.convertFrom0to1 (0.20f) / r.convertFrom0to1 (0.10f);
            const float high = r.convertFrom0to1 (0.70f) / r.convertFrom0to1 (0.60f);

            check (std::abs (low - high) < 0.25f,
                   "cutoff gives equal octaves per equal travel",
                   juce::String (low) + " vs " + juce::String (high));
        }
    }

    // ---- 14. Layout: no visible control is left with zero bounds ----
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

        check (dbg.contains ("strips=" + juce::String (miXXEditor->getNumShapeModules())),
               "sidebar has one strip per module", dbg.upToFirstOccurrenceOf (" chips=", false, false));

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

    // ---- 15. LFO/ENV modules: add, remove, and slot remapping ----
    // Deliberately last: adding or removing defers the sidebar rebuild to the
    // message loop, which this audit does not pump, so the layout checks above
    // want to run while the default two-module sidebar is still in place.
    {
        auto slotSrcAt = [&proc] (int n)
        {
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (
                    proc.apvts.getParameter ("mod" + juce::String (n + 1) + "src")))
                return c->getIndex();

            return -1;
        };

        auto setSlotSrc = [&proc] (int n, int src)
        {
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (
                    proc.apvts.getParameter ("mod" + juce::String (n + 1) + "src")))
            {
                c->beginChangeGesture();
                c->setValueNotifyingHost (c->convertTo0to1 (static_cast<float> (src)));
                c->endChangeGesture();
            }
        };

        // The default patch is one of each.
        check (proc.getNumShapeModules() == 2 && proc.getNumLfoModules() == 1,
               "modules: default is one LFO and one ENV",
               juce::String (proc.getNumLfoModules()) + " LFO / "
                 + juce::String (proc.getNumShapeModules() - proc.getNumLfoModules()) + " ENV");

        proc.clearShapeModules();
        check (proc.getNumShapeModules() == 0, "modules: removable down to zero",
               juce::String (proc.getNumShapeModules()));

        check (proc.addShapeModule (true) == 0, "modules: first LFO lands at index 0", "");
        check (proc.addShapeModule (false) == 1, "modules: first ENV sits after the LFOs", "");
        check (proc.addShapeModule (true) == 1,
               "modules: a second LFO inserts after the first", "");
        check (proc.getNumLfoModules() == 2, "modules: two LFOs",
               juce::String (proc.getNumLfoModules()));
        check (proc.getNumShapeModules() == 3, "modules: three total",
               juce::String (proc.getNumShapeModules()));

        // Slot 0 -> LFO 2, slot 1 -> LFO 1.
        setSlotSrc (0, mixxynth::srcLfo2);
        setSlotSrc (1, mixxynth::srcLfo1);

        // Removing LFO 1 must leave slot 0 following the module it pointed at
        // (which is now LFO 1), and clear slot 1 because its module is gone.
        miXXEditor->removeShapeModule (0);

        check (proc.getNumLfoModules() == 1, "modules: LFO count drops",
               juce::String (proc.getNumLfoModules()));
        check (slotSrcAt (0) == mixxynth::srcLfo1,
               "modules: a link follows its module across a removal",
               juce::String (slotSrcAt (0)));
        check (slotSrcAt (1) == mixxynth::srcOff,
               "modules: links to the removed module clear", juce::String (slotSrcAt (1)));

        // Removing an ENV must not disturb LFO links.
        setSlotSrc (2, mixxynth::srcLfo1);
        miXXEditor->removeShapeModule (1); // the only ENV
        check (slotSrcAt (2) == mixxynth::srcLfo1,
               "modules: removing an ENV leaves LFO links alone",
               juce::String (slotSrcAt (2)));

        // Capacity.
        proc.clearShapeModules();

        for (int i = 0; i < mixxynth::kMaxLfos; ++i)
            proc.addShapeModule (true);

        check (proc.getNumLfoModules() == mixxynth::kMaxLfos, "modules: eight LFOs fit",
               juce::String (proc.getNumLfoModules()));
        check (proc.addShapeModule (true) < 0, "modules: a ninth LFO is refused", "");
        check (! proc.canAddShapeModule (true), "modules: canAdd reports a full family", "");

        for (int i = 0; i < mixxynth::kMaxModEnvs; ++i)
            proc.addShapeModule (false);

        check (proc.getNumShapeModules() == mixxynth::kMaxShapes, "modules: sixteen in total",
               juce::String (proc.getNumShapeModules()));
        check (proc.addShapeModule (false) < 0, "modules: a seventeenth is refused", "");
    }

    std::printf (failures == 0 ? "EDITOR AUDIT OK\n" : "EDITOR AUDIT FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
