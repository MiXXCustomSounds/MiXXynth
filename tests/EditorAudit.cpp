// Editor audit: verifies every knob is wired to the parameter and modulation
// destination it is supposed to be, that the destination name list agrees with
// the ModDest enum, that every parameter is reachable from the UI, and that the
// drag-and-drop modulation slot logic behaves.
//
// This is the automated half of the editor audit; the interactive half (drag
// every knob in the standalone) is recorded in docs/EDITOR-AUDIT.md.
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/AdsrEditor.h"
#include "../Source/Algorithms.h"
#include "../Source/ModKnob.h"
#include "../Source/ModMatrix.h"
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"
#include "../Source/SquareCell.h"
#include "../Source/Fx/FxRack.h"
#include "../Source/Fx/WeirdVerbFx.h"

// The M6 checks below name this type directly; everything else in this file is
// reached through the editor, so nothing else needs a using declaration.
using mixxynth::ui::SquareCell;
using mixxynth::ui::AdsrEditor;

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
        if (id == "vaShape") return mixxynth::dstVaPulseWidth;
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
        std::set<juce::String> s { "algorithm", "vaOctave", "filterType", "filterOn",
                                   "mpeEnabled", "mpeMemberChannels" };

        // The VA's amplitude envelope is edited by the graphical envelope, not a knob. It
        // is deliberately not a modulation destination: it only ever controls the VA
        // layer's own amplitude.
        s.insert ("vaAttack");
        s.insert ("vaDecay");
        s.insert ("vaSustain");
        s.insert ("vaRelease");

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            s.insert ("carrier" + juce::String (i + 1));

        // Per-operator enable is a toggle, not a knob.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            s.insert ("op" + juce::String (i + 1) + "on");

        // Per-operator ADSR is edited by the graphical envelope, which is not a
        // ModTarget - it is deliberately not a modulation destination, since it only
        // ever controls its own operator.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);
            s.insert ("op" + n + "attack");
            s.insert ("op" + n + "decay");
            s.insert ("op" + n + "sustain");
            s.insert ("op" + n + "release");
        }

        // Per-operator FILTER. The on/off toggle, the mode combo, the graphical filter
        // envelope and the routing switch are all real controls that are not ModTargets. The
        // filter's cutoff, resonance and drive ARE cells, so those three are counted already.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n = juce::String (i + 1);
            s.insert ("op" + n + "fltOn");
            s.insert ("op" + n + "fltType");
            s.insert ("op" + n + "fltAttack");
            s.insert ("op" + n + "fltDecay");
            s.insert ("op" + n + "fltSustain");
            s.insert ("op" + n + "fltRelease");
            s.insert ("op" + n + "fltRoute");
        }

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

    // The FM section lost its algorithm selector, so the FM page pins the parameter
    // to Free (matrix) as it builds. A hidden parameter still steering routing would
    // be the exact confusion that removal was meant to end, so this is checked here,
    // immediately, before anything else touches the algorithm.
    if (auto* algo = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("algorithm")))
        check (algo->getIndex() == mixxynth::kFreeAlgorithm,
               "the algorithm parameter is pinned to Free (matrix)",
               juce::String (algo->getIndex()) + " of " + juce::String (mixxynth::kFreeAlgorithm));

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

    for (auto* k : miXXEditor->modTargets)
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

        // The rack's slot parameters are deliberately exempt. A slot's controls
        // only exist once that slot is in use, and the generic p7/p8 are spare
        // for most modules, so "has a control right now" cannot hold for them.
        // Section 16 checks them properly instead: it activates a slot and
        // verifies every parameter the module declares is wired to a control.
        if (id.startsWith ("fx"))
            continue;

        if (knobDest.count (id) == 0 && nonKnob.count (id) == 0)
            unreachable.add (id);
    }

    check (unreachable.isEmpty(), "every parameter reachable from UI", unreachable.joinIntoString (", "));
    std::printf ("     params=%d knobs=%d\n", totalParams,
                 static_cast<int> (miXXEditor->modTargets.size()));

    // ---- 4. Non-modulatable knobs are correctly identified ----
    int wrongTargetFlags = 0;
    int modTargetKnobs = 0, offKnobs = 0;

    for (auto* k : miXXEditor->modTargets)
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

    // ---- The Mod tab must follow a drag-and-drop ----
    // Everything above was already true while the bug was live: the parameters were
    // always correct. The tab went on showing Off because its rows bound their
    // combos once in the constructor and never looked at the parameters again, so
    // the check has to be on what the ROW displays, not on the parameters.
    miXXEditor->runTabPolls();

    {
        const auto row = miXXEditor->modDebug (0);

        check (row.contains ("srcId=" + juce::String (mixxynth::srcLfo1 + 1)),
               "the Mod tab follows a drag-and-drop", row);
        check (row.contains ("dstId=" + juce::String (mixxynth::dstFmIndex + 1)),
               "the Mod tab shows the dropped destination", row);
        check (row.contains ("srcText="
                              + juce::String (mixxynth::modSourceNames()[mixxynth::srcLfo1])),
               "the Mod tab names the source", row);
    }

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

        for (auto* k : miXXEditor->modTargets)
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

    // ---- A knob with two sources must have BOTH arcs draggable ----
    // The old code always grabbed assignments.front(), so a second source on the
    // same knob could only be changed through the right-click menu. That is the
    // "hidden in the menu" half of the complaint, and it is a geometry claim, so it
    // is checked as geometry rather than by screenshot.
    {
        for (int n = 0; n < mixxynth::kNumModSlots; ++n)
            miXXEditor->clearModDest (slotDst (n));

        // The arc geometry is a ModKnob property, and the FM page's controls are
        // SquareCells now, so this deliberately targets a control that is still a knob:
        // the master gain, which is a real modulation destination too.
        miXXEditor->assignModSlot (mixxynth::srcLfo1, mixxynth::dstMaster);
        miXXEditor->assignModSlot (mixxynth::srcMacro1, mixxynth::dstMaster);
        miXXEditor->refreshModRings();

        mixxynth::ui::ModKnob* knob = nullptr;

        for (auto* k : miXXEditor->modTargets)
            if (k->getParamID() == "masterGain" && k->getNumAssignments() == 2)
                knob = dynamic_cast<mixxynth::ui::ModKnob*> (k);

        check (knob != nullptr, "the master knob collected both sources", "");

        if (knob != nullptr)
        {
            check (knob->ringIndexAtForTest (knob->arcPointForTest (0)) == 0,
                   "the first modulation arc is grabbable", "");
            check (knob->ringIndexAtForTest (knob->arcPointForTest (1)) == 1,
                   "the second modulation arc is grabbable too", "");

            // The arc is anchored to the knob's value, so it has to MOVE when the
            // value moves. It is drawn by the ModKnob, outside the sst knob child's
            // bounds, so a parent that does not repaint leaves it frozen - which is
            // exactly the bug this replaced, and it was invisible while the arc
            // swept from a fixed angle instead.
            // Start from a known low value rather than from whatever the audit left
            // the master gain at. Otherwise this measures 0.9 against a value an earlier
            // section parked at 1.0, and reports the arc moving backwards.
            auto setCutoff = [&proc] (float v)
            {
                if (auto* p = proc.apvts.getParameter ("masterGain"))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (v);
                    p->endChangeGesture();
                }
            };

            setCutoff (0.2f);
            const auto before = knob->arcAnglesForTest (0);

            setCutoff (0.9f);
            const auto after = knob->arcAnglesForTest (0);

            check (! juce::approximatelyEqual (before.first, after.first)
                     && after.first > before.first,
                   "the modulation arc follows the knob's value",
                   juce::String (before.first, 3) + " -> " + juce::String (after.first, 3));
        }

        for (int n = 0; n < mixxynth::kNumModSlots; ++n)
            miXXEditor->clearModDest (slotDst (n));

        miXXEditor->refreshModRings();
    }

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

        check (nameAt (mixxynth::dstVaPulseWidth) == "VA Shape",
               "VA Shape is a modulation destination", nameAt (mixxynth::dstVaPulseWidth));
    }

    // ---- 8. (removed) FM routing edits and the algorithm ----------------
    // This section tested MatrixPage::poll(), which watched for a routing edit while a
    // DX7 algorithm was in charge and switched the patch to Free mode. The routing
    // matrix is a view of the FM grid now, and the algorithm parameter is pinned to
    // Free as the page builds, so there is no algorithm to be in charge and the
    // behaviour cannot occur. The routing depths themselves are still covered: every
    // mtx parameter has a control and a destination, which sections 2 and 3 check.

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

    // Every tab must exist and be populated. Two: the routing matrix and the modulation
    // matrix are views and a section of the FM tab, and the voice controls live in the
    // sidebar, so only the FX rack needs a tab of its own.
    check (miXXEditor->getNumTabs() == 2, "two tabs", juce::String (miXXEditor->getNumTabs()));

    // The voice controls added in M6: drift and velocity response. The generic coverage
    // check cannot say WHERE a parameter's control lives, and for these three that is the
    // whole point - they are properties of a voice, not of an oscillator, so they belong
    // beside polyphony and unison rather than on the FM tab.
    {
        int found = 0;

        for (auto* k : miXXEditor->modTargets)
        {
            const auto id = k->getParamID();

            if (id == "drift" || id == "velToLevel" || id == "velToIndex")
                ++found;
        }

        check (found == 3, "drift and velocity each have a control", juce::String (found));
    }

    // Switching views must not leave the previous view's controls behind.
    //
    // The operator cells did exactly that: they were the one list applyView did not know
    // about, so a column of six operator boxes sat on top of the VA envelope and the
    // filter row - in a view that has no operators in it at all.
    //
    // This is a generic overlap check rather than an assertion about a named list,
    // because the bug WAS a list nobody remembered to add. A named assertion would have
    // had exactly the same blind spot as the code it was guarding.
    {
        auto* fm = miXXEditor->getTabContentComponent (0);

        check (fm != nullptr, "the FM tab has content to inspect",
               juce::String (fm != nullptr ? 1 : 0));

        if (fm != nullptr)
        {
            auto describe = [fm] (int idx) -> juce::String
            {
                auto* c = fm->getChildComponent (idx);

                if (c == nullptr)
                    return "#" + juce::String (idx);

                // The parameter ID is the only thing that names a control usefully here:
                // JUCE components carry no class name, and a cell or a knob is identified
                // by what it drives rather than by its label.
                if (auto* sc = dynamic_cast<mixxynth::ui::SquareCell*> (c))
                    return sc->getParamID();

                if (auto* mk = dynamic_cast<mixxynth::ui::ModKnob*> (c))
                    return mk->getParamID();

                if (auto* b = dynamic_cast<juce::Button*> (c))
                    if (b->getButtonText().isNotEmpty())
                        return b->getButtonText();

                return "#" + juce::String (idx);
            };

            // Both grid views AND both envelope pages. Page two's controls are only laid out
            // while it is showing, so a page this never visits is a page it never guards -
            // and the last thing that was laid out in one state only was a column of cells
            // drawn on top of another view.
            for (int v = 0; v < 2; ++v)
            {
                miXXEditor->setFmView (v);

                for (int pg = 0; pg < 2; ++pg)
                {
                    miXXEditor->setFmAdsrPage (pg);

                    juce::StringArray clashes;

                    for (int i = 0; i < fm->getNumChildComponents(); ++i)
                    {
                        auto* a = fm->getChildComponent (i);

                        if (a == nullptr || ! a->isVisible() || a->getBounds().isEmpty())
                            continue;

                        for (int j = i + 1; j < fm->getNumChildComponents(); ++j)
                        {
                            auto* b = fm->getChildComponent (j);

                            if (b == nullptr || ! b->isVisible() || b->getBounds().isEmpty())
                                continue;

                            if (a->getBounds().intersects (b->getBounds()))
                                clashes.add (describe (i) + "+" + describe (j));
                        }
                    }

                    check (clashes.isEmpty(), "no controls overlap in an FM view",
                           juce::String (v) + "/" + juce::String (pg) + ": "
                             + clashes.joinIntoString (", "));
                }
            }

            miXXEditor->setFmView (0);
            miXXEditor->setFmAdsrPage (0);
        }
    }

    // The VA panel belongs to the LEFT of the grid. That is a layout requirement rather
    // than an incidental consequence of the order things were constructed in, so it is
    // asserted rather than assumed.
    {
        mixxynth::ui::SquareCell* vaShape = nullptr;
        mixxynth::ui::SquareCell* indexCell = nullptr;

        for (auto* k : miXXEditor->modTargets)
        {
            auto* c = dynamic_cast<mixxynth::ui::SquareCell*> (k);

            if (c == nullptr)
                continue;

            if (c->getParamID() == "vaShape")  vaShape = c;
            if (c->getParamID() == "fmIndex")  indexCell = c;
        }

        check (vaShape != nullptr && indexCell != nullptr,
               "the VA panel and the grid both have cells",
               juce::String (vaShape != nullptr ? 1 : 0) + "/" + juce::String (indexCell != nullptr ? 1 : 0));

        if (vaShape != nullptr && indexCell != nullptr)
            check (vaShape->getRight() <= indexCell->getX(),
                   "the VA panel sits to the left of the grid",
                   juce::String (vaShape->getRight()) + " <= " + juce::String (indexCell->getX()));
    }

    // FM Index is its own column heading, so it has to name itself. Every other grid cell is
    // named by the icon above it, which is why the label is opt-in - and why this one being
    // left out was invisible in every check that looked at values rather than at words.
    {
        mixxynth::ui::SquareCell* index = nullptr;

        for (auto* k : miXXEditor->modTargets)
            if (auto* c = dynamic_cast<mixxynth::ui::SquareCell*> (k))
                if (c->getParamID() == "fmIndex")
                    index = c;

        check (index != nullptr && index->isShowingLabel(),
               "the FM Index cell names itself",
               juce::String (index != nullptr ? 1 : 0));
    }

    // Step 4: the shape cells draw the shape instead of naming it. Seven of them - one per
    // operator plus the VA's, which is the same morph and should read as the same thing.
    {
        int waveformCells = 0;

        for (auto* k : miXXEditor->modTargets)
            if (auto* c = dynamic_cast<mixxynth::ui::SquareCell*> (k))
                if (c->isShowingWaveform())
                    ++waveformCells;

        check (waveformCells == 7, "the shape cells draw waveforms", juce::String (waveformCells));
    }

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

    // ---- 16. FX rack: add, reorder, remove, capacity, and state ----
    // Placed last for the same reason as the module checks: it changes the rack,
    // and the layout checks above want the default (empty) rack.
    {
        auto slotType = [&proc] (int s)
        {
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (
                    proc.apvts.getParameter ("fx" + juce::String (s + 1) + "type")))
                return c->getIndex();

            return -1;
        };

        auto setSlotParam = [&proc] (int s, int p, float v)
        {
            if (auto* f = proc.apvts.getParameter ("fx" + juce::String (s + 1)
                                                     + "p" + juce::String (p + 1)))
            {
                f->beginChangeGesture();
                f->setValueNotifyingHost (f->convertTo0to1 (v));
                f->endChangeGesture();
            }
        };

        auto slotParam = [&proc] (int s, int p)
        {
            if (auto* f = proc.apvts.getParameter ("fx" + juce::String (s + 1)
                                                     + "p" + juce::String (p + 1)))
                return f->convertFrom0to1 (f->getValue());

            return -1.0f;
        };

        // The rack starts empty.
        int live = 0;

        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
            if (slotType (s) != mixxynth::fxOff)
                ++live;

        check (live == 0, "rack starts empty", juce::String (live));
        check (proc.fxRack().getNumInstances() == 0, "an empty rack builds no modules", "");

        // Adding fills the first free slot, so the chain stays dense.
        miXXEditor->fxAddModule (mixxynth::fxWeirdVerb);
        miXXEditor->fxRebuildCards();

        check (slotType (0) == mixxynth::fxWeirdVerb, "adding puts the module in slot 1",
               juce::String (slotType (0)));
        check (proc.getFxSlotType (0) == mixxynth::fxWeirdVerb,
               "the rack is running the module the parameter asks for", "");
        check (proc.fxRack().hasInstance (0, mixxynth::fxWeirdVerb),
               "the module instance exists", "");

        // The card must have a control per declared parameter, and none of them
        // may end up with no size.
        {
            const auto desc = mixxynth::FxRack::descriptorFor (mixxynth::fxWeirdVerb);
            const auto dump = miXXEditor->fxDebug();

            int expectedCombos = 0, expectedKnobs = 0;

            for (int p = 0; p < desc.numParams; ++p)
                (desc.params[static_cast<size_t> (p)].isChoice ? expectedCombos : expectedKnobs) += 1;

            check (dump.contains ("cards=1"), "one card per live module", dump);
            check (dump.contains ("parented=1"),
                   "the card is parented and sized, not just constructed", dump);
            check (dump.contains ("combos=" + juce::String (expectedCombos)
                                    + " knobs=" + juce::String (expectedKnobs)),
                   "the card builds one control per declared parameter", dump);
            check (dump.contains ("zeros=0"), "no FX control has zero bounds", dump);

            // A combo whose selection never lands renders blank while looking
            // perfectly healthy in the model, so check the visible text too.
            check (dump.contains ("comboIdx=0") && dump.contains ("comboText=Plate"),
                   "the mode combo shows its parameter's choice", dump);
            check (! dump.contains ("emptyVisible=1"),
                   "the empty-rack hint hides once there is a module", dump);
        }

        // The combo is bound by hand to a generic float, so both directions need
        // checking: writing the choice back to the parameter, and showing a
        // parameter that moved from somewhere else.
        {
            // Derived from the descriptor rather than hard-coded: the mode list
            // grew from three entries to five, and a test that assumes the list
            // length stops testing the binding and starts testing my arithmetic.
            const auto desc = mixxynth::FxRack::descriptorFor (mixxynth::fxWeirdVerb);
            const auto& modes = desc.params[mixxynth::WeirdVerbFx::pMode].choices;
            const int last = modes.size() - 1;

            miXXEditor->fxSetCardChoice (0, 0, last); // Mode -> last entry

            check (std::abs (slotParam (0, mixxynth::WeirdVerbFx::pMode) - 1.0f) < 1.0e-4f,
                   "choosing from the combo writes the generic parameter",
                   juce::String (slotParam (0, mixxynth::WeirdVerbFx::pMode)));

            // ...and the other way round: a change from elsewhere must show up.
            const int midIndex = last / 2;

            setSlotParam (0, mixxynth::WeirdVerbFx::pMode,
                          static_cast<float> (midIndex) / static_cast<float> (last));
            miXXEditor->fxRebuildCards();

            check (miXXEditor->fxDebug().contains ("comboIdx=" + juce::String (midIndex))
                     && miXXEditor->fxDebug().contains ("comboText=" + modes[midIndex]),
                   "a parameter change is reflected in the combo",
                   miXXEditor->fxDebug());
        }

        // A module's defaults are applied when its type is chosen, so a fresh
        // module never starts silent at zero.
        {
            const auto desc = mixxynth::FxRack::descriptorFor (mixxynth::fxWeirdVerb);
            int defaultsApplied = 0, defaultsDeclared = 0;

            for (int p = 0; p < desc.numParams; ++p)
            {
                const float want = desc.params[static_cast<size_t> (p)].defaultValue;

                if (want <= 0.0f)
                    continue;

                ++defaultsDeclared;

                if (std::abs (slotParam (0, p) - want) < 1.0e-4f)
                    ++defaultsApplied;
            }

            check (defaultsApplied == defaultsDeclared && defaultsDeclared > 0,
                   "a new module starts on its declared defaults",
                   juce::String (defaultsApplied) + "/" + juce::String (defaultsDeclared));
        }

        // A second module goes to the next slot, and the two do not share params.
        miXXEditor->fxAddModule (mixxynth::fxWeirdVerb);
        miXXEditor->fxRebuildCards();

        check (slotType (1) == mixxynth::fxWeirdVerb, "the second module takes slot 2",
               juce::String (slotType (1)));

        setSlotParam (0, mixxynth::WeirdVerbFx::pMix, 0.9f);

        check (std::abs (slotParam (0, mixxynth::WeirdVerbFx::pMix) - 0.9f) < 1.0e-4f
                 && std::abs (slotParam (1, mixxynth::WeirdVerbFx::pMix) - 0.35f) < 1.0e-3f,
               "slots hold independent settings",
               juce::String (slotParam (0, mixxynth::WeirdVerbFx::pMix)) + "/"
                 + juce::String (slotParam (1, mixxynth::WeirdVerbFx::pMix)));

        // Reorder: swapping a pair must carry the whole slot, settings included.
        miXXEditor->fxMoveModule (1, -1);

        check (std::abs (slotParam (0, mixxynth::WeirdVerbFx::pMix) - 0.35f) < 1.0e-3f
                 && std::abs (slotParam (1, mixxynth::WeirdVerbFx::pMix) - 0.9f) < 1.0e-4f,
               "a reorder carries the module's settings with it",
               juce::String (slotParam (0, mixxynth::WeirdVerbFx::pMix)) + "/"
                 + juce::String (slotParam (1, mixxynth::WeirdVerbFx::pMix)));

        // Removing empties the slot and the chain stays dense.
        miXXEditor->fxRemoveModule (0);
        miXXEditor->fxRebuildCards();

        check (slotType (0) == mixxynth::fxWeirdVerb, "removing pulls the chain up",
               juce::String (slotType (0)));
        check (slotType (1) == mixxynth::fxOff, "the freed slot is Off",
               juce::String (slotType (1)));
        check (miXXEditor->fxDebug().contains ("cards=1"), "the card list follows",
               miXXEditor->fxDebug());

        // The chain must stay dense. Both "add fills the first free slot" and the
        // adjacent-swap reorder depend on there being no holes, so a removal that
        // left one behind would break them silently.
        {
            bool dense = true, seenEmpty = false;

            for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
            {
                if (slotType (s) == mixxynth::fxOff)
                    seenEmpty = true;
                else if (seenEmpty)
                    dense = false;
            }

            check (dense, "the chain has no holes after a removal", miXXEditor->fxDebug());
        }

        // Capacity: eight slots, and a ninth is refused rather than overwriting.
        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
            if (slotType (s) == mixxynth::fxOff)
                miXXEditor->fxAddModule (mixxynth::fxWeirdVerb);

        miXXEditor->fxRebuildCards();

        auto countFilled = [&slotType] ()
        {
            int n = 0;

            for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
                if (slotType (s) != mixxynth::fxOff)
                    ++n;

            return n;
        };

        check (countFilled() == mixxynth::kMaxFxSlots, "all eight slots fill",
               juce::String (countFilled()));

        miXXEditor->fxAddModule (mixxynth::fxWeirdVerb);

        check (countFilled() == mixxynth::kMaxFxSlots,
               "a ninth module is refused, not written over", juce::String (countFilled()));

        // State round-trip: the rack rides in the APVTS, so it must survive.
        {
            setSlotParam (3, mixxynth::WeirdVerbFx::pDecay, 0.83f);

            juce::MemoryBlock saved;
            proc.getStateInformation (saved);

            for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
                proc.writeFxSlot (s, mixxynth::FxSlotState {});

            proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));

            check (countFilled() == mixxynth::kMaxFxSlots,
                   "the rack survives a state round-trip", juce::String (countFilled()));
            check (std::abs (slotParam (3, mixxynth::WeirdVerbFx::pDecay) - 0.83f) < 1.0e-4f,
                   "a slot's settings survive too",
                   juce::String (slotParam (3, mixxynth::WeirdVerbFx::pDecay)));

            // The loaded types must reach the audio side, not just the
            // parameters: a module the state asked for has to actually exist.
            check (proc.getFxSlotType (3) == mixxynth::fxWeirdVerb,
                   "a loaded state republishes the rack", juce::String (proc.getFxSlotType (3)));
        }

        // ---- The convolution module, and the file control it needs ----
        // This is the first module with a FILE, and a file is state rather than a
        // parameter, so the path it travels is different from every other control in
        // the rack and has to be checked separately.
        {
            for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
                proc.writeFxSlot (s, mixxynth::FxSlotState {});

            miXXEditor->fxAddModule (mixxynth::fxConvolution);
            miXXEditor->fxRebuildCards();

            check (slotType (0) == mixxynth::fxConvolution,
                   "the convolution module can be added", juce::String (slotType (0)));
            check (proc.fxRack().hasInstance (0, mixxynth::fxConvolution),
                   "its instance exists", "");

            const auto desc = mixxynth::FxRack::descriptorFor (mixxynth::fxConvolution);
            const auto dump = miXXEditor->fxDebug();

            check (desc.hasFile, "the convolution descriptor declares a file", "");
            check (dump.contains ("cards=1") && dump.contains ("zeros=0"),
                   "its card builds with no zero bounds", dump);
            check (dump.contains ("comboItems=" + juce::String (desc.params[0].choices.size())),
                   "its impulse combo lists every built-in plus the file option", dump);
            check (dump.contains ("file=1"),
                   "its card shows a file control", dump);
        }

        // A file survives a state round-trip, bytes and all. This is the whole point
        // of embedding: a preset has to work when the original file is gone.
        {
            juce::MemoryBlock fake;
            const char payload[] = "not really a wav, but it is bytes";
            fake.append (payload, sizeof (payload));

            proc.setFxSlotFile (0, "MyImpulse.wav", fake);

            check (proc.getFxSlotFileName (0) == "MyImpulse.wav",
                   "the file name is stored", proc.getFxSlotFileName (0));
            check (proc.getFxSlotFileData (0).getSize() == fake.getSize(),
                   "the file bytes are stored",
                   juce::String (proc.getFxSlotFileData (0).getSize()));

            juce::MemoryBlock saved;
            proc.getStateInformation (saved);

            // Wipe it, then load the state back.
            proc.setFxSlotFile (0, {}, {});
            check (proc.getFxSlotFileName (0).isEmpty(), "the file can be cleared", "");

            proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));

            check (proc.getFxSlotFileName (0) == "MyImpulse.wav",
                   "the file name survives a state round-trip",
                   proc.getFxSlotFileName (0));

            const auto restored = proc.getFxSlotFileData (0);

            check (restored.getSize() == fake.getSize()
                     && std::memcmp (restored.getData(), fake.getData(), fake.getSize()) == 0,
                   "the file BYTES survive a state round-trip, so a preset is portable",
                   juce::String (restored.getSize()));
        }

        // Leave the rack empty, as it starts.
        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
        {
            proc.writeFxSlot (s, mixxynth::FxSlotState {});
            proc.setFxSlotFile (s, {}, {});
        }
    }

    // ---- M6: the square cell -------------------------------------------------
    // The cell is not wired into the editor yet, so these drive it directly. They
    // pin the readout contract, because those three properties ARE the reason for
    // replacing a knob with a cell: the fill height is the value, the colour says
    // whether it is modulated, and the white line says how far it can be pushed.
    {
        MiXXynthProcessor cellProc;
        cellProc.prepareToPlay (44100.0, 512);

        auto* level = cellProc.apvts.getParameter ("op1level");
        SquareCell cell ("Op1 Level", level, mixxynth::dstOp1Level);
        cell.setBounds (0, 0, 60, 60);

        check (cell.isModTarget(), "a square cell with a destination is a mod target");
        check (cell.getParam() != nullptr && cell.getParam()->getParameterID() == "op1level",
               "the cell reports its parameter",
               cell.getParam() != nullptr ? cell.getParam()->getParameterID() : juce::String {});
        check (cell.getLabelText() == "Op1 Level", "the cell reports its label", cell.getLabelText());

        // Value 0 must be a black cell, and 1 a full one.
        level->setValueNotifyingHost (0.0f);

        check (std::abs (cell.value01()) < 0.001f, "value 0 reads as 0",
               juce::String (cell.value01(), 3));
        check (cell.fillRect().getHeight() < 1.0f, "value 0 fills nothing, so the cell is black",
               juce::String (cell.fillRect().getHeight(), 2));

        level->setValueNotifyingHost (1.0f);

        check (std::abs (cell.value01() - 1.0f) < 0.001f, "value 1 reads as 1",
               juce::String (cell.value01(), 3));
        check (cell.fillRect().getHeight() > 50.0f, "value 1 fills the cell",
               juce::String (cell.fillRect().getHeight(), 2));

        // Half the value must be half the fill: the fill IS the readout.
        level->setValueNotifyingHost (0.5f);

        const float halfFill = cell.fillRect().getHeight();
        const float fullFill = cell.yForValue01 (0.0f) - cell.yForValue01 (1.0f);

        check (std::abs (halfFill - fullFill * 0.5f) < 1.0f,
               "half the value is half the fill",
               juce::String (halfFill, 2) + " vs " + juce::String (fullFill * 0.5f, 2));
    }

    // The colour is the modulated/unmodulated signal, and a live value has to be
    // display only: the whole point is that it shows what the modulation is doing,
    // so it must never write the parameter it is showing.
    {
        MiXXynthProcessor cellProc;
        auto* level = cellProc.apvts.getParameter ("op1level");
        SquareCell cell ("Op1 Level", level, mixxynth::dstOp1Level);
        cell.setBounds (0, 0, 60, 60);

        level->setValueNotifyingHost (0.5f);

        check (! cell.isShowingLiveValue(), "a cell with no live value is not modulated");
        check (cell.fillColour() == juce::Colour (0xffffa53d),
               "an unmodulated cell fills with the accent",
               cell.fillColour().toDisplayString (true));

        cell.setLiveValue01 (0.75f);

        check (cell.isShowingLiveValue(), "a live value marks the cell as modulated");
        check (cell.fillColour() == juce::Colour (0xff3aa0ff),
               "a modulated cell fills with blue",
               cell.fillColour().toDisplayString (true));
        check (std::abs (cell.value01() - 0.75f) < 0.001f,
               "the live value drives the fill, so the cell moves with the modulation",
               juce::String (cell.value01(), 3));
        check (std::abs (level->getValue() - 0.5f) < 0.001f,
               "the live value does NOT overwrite the parameter",
               juce::String (level->getValue(), 3));

        cell.setLiveValue01 (-1.0f);

        check (! cell.isShowingLiveValue() && std::abs (cell.value01() - 0.5f) < 0.001f,
               "clearing the live value falls back to the parameter",
               juce::String (cell.value01(), 3));

        // Dragging: 140 px covers the whole range, and it clamps rather than wraps.
        check (SquareCell::valueFromDrag (0.5f, 100.0f, 30.0f) == 1.0f,
               "70 px up from the middle reaches the top",
               juce::String (SquareCell::valueFromDrag (0.5f, 100.0f, 30.0f), 3));
        check (SquareCell::valueFromDrag (0.5f, 100.0f, 1000.0f) == 0.0f,
               "dragging far down clamps to 0 rather than wrapping");
        check (SquareCell::valueFromDrag (0.5f, 100.0f, 100.0f) == 0.5f,
               "not moving leaves the value alone");

        // The white line sits at value + depth, and stacks one per source.
        cell.setModAssignments ({ { mixxynth::srcLfo1, 0.25f } });

        check (cell.getNumAssignments() == 1, "assignments reach the cell",
               juce::String (cell.getNumAssignments()));
        check (std::abs (cell.getModDepth() - 0.25f) < 0.001f, "the total depth is summed",
               juce::String (cell.getModDepth(), 3));

        const float atValue = cell.yForValue01 (0.5f);
        const float atDepth = cell.yForValue01 (0.5f + 0.25f);

        check (atDepth < atValue, "a positive depth draws the white line above the value",
               juce::String (atDepth, 1) + " vs " + juce::String (atValue, 1));

        cell.setModAssignments ({ { mixxynth::srcLfo1, 0.25f }, { mixxynth::srcEnv1, -0.25f } });

        check (cell.getNumAssignments() == 2, "a second source stacks on the same cell",
               juce::String (cell.getNumAssignments()));
        check (std::abs (cell.getModDepth()) < 0.001f,
               "opposite depths cancel in the total",
               juce::String (cell.getModDepth(), 3));

        // A cell with no destination refuses modulation rather than pretending.
        SquareCell plain ("Plain", cellProc.apvts.getParameter ("filterOn"), mixxynth::dstOff);

        check (! plain.isModTarget(), "a cell with no destination is not a mod target");
        check (plain.fillColour() == juce::Colour (0xffffa53d),
               "a non-target cell still fills with the accent",
               plain.fillColour().toDisplayString (true));
    }

    // ---- M6: the FM grid ------------------------------------------------------
    // The FM section is a grid of square cells now rather than 48 labelled knobs.
    // These pin the three things that make it a grid rather than a pile of
    // controls: the cells are square, they are cells rather than knobs, and the
    // shape column still says which shape it is at.
    {
        std::vector<SquareCell*> fmCells;
        std::vector<SquareCell*> allCells;
        int routingCount = 0;
        int vaCount = 0;
        int filterPageCount = 0;
        int modDepthCount = 0;

        for (auto* k : miXXEditor->modTargets)
        {
            auto* c = dynamic_cast<SquareCell*> (k);

            if (c == nullptr)
                continue;

            allCells.push_back (c);

            // Every section's cells are SquareCells, so they have to be told apart by their
            // parameters: mtxxy is a routing depth, va* and filter* are the VA panel, flt is
            // the envelope section's filter page, and everything else in the grid is a value.
            const auto id = c->getParamID();

            if (id.startsWith ("mtx"))
                ++routingCount;
            else if (id.startsWith ("va") || id.startsWith ("filter"))
                ++vaCount;
            else if (id.contains ("flt"))
                ++filterPageCount;
            else if (id.startsWith ("mod"))
                ++modDepthCount;
            else
                fmCells.push_back (c);
        }

        // The modulation matrix's depth column: one cell per slot.
        check (modDepthCount == mixxynth::kNumModSlots,
               "the modulation matrix built its depth cells",
               juce::String (modDepthCount) + " of " + juce::String (mixxynth::kNumModSlots));

        // The VA view: six oscillator cells (shape, sub, noise, level, detune, octave) and
        // three filter cells (cutoff, resonance, drive).
        check (vaCount == 9, "the VA view built its oscillator and filter cells",
               juce::String (vaCount));

        // The filter page: cutoff, resonance and drive for each operator.
        check (filterPageCount == 3 * MiXXFmVoice::numOps,
               "the filter page built its cells",
               juce::String (filterPageCount) + " of " + juce::String (3 * MiXXFmVoice::numOps));

        // One for FM Index, then shape/ratio/level/feedback for each operator.
        const int expected = 1 + 4 * MiXXFmVoice::numOps;

        check (static_cast<int> (fmCells.size()) == expected,
               "the values view built one cell per value, plus FM Index",
               juce::String (static_cast<int> (fmCells.size())) + " of " + juce::String (expected));

        // Six by six minus the diagonal: a self-send is the per-operator feedback
        // parameter, which is why the diagonal is free for the carrier switches.
        const int expectedRouting = MiXXFmVoice::numOps * (MiXXFmVoice::numOps - 1);

        check (routingCount == expectedRouting,
               "the routing view built one cell per off-diagonal pair",
               juce::String (routingCount) + " of " + juce::String (expectedRouting));

        // The whole premise is that every value cell is square. If they are not, it
        // is a table with extra steps.
        int nonSquare = 0, unsized = 0;

        for (auto* c : fmCells)
        {
            const auto b = c->getBounds();

            if (b.getWidth() <= 0 || b.getHeight() <= 0)
                ++unsized;
            else if (std::abs (b.getWidth() - b.getHeight()) > 1)
                ++nonSquare;
        }

        check (unsized == 0, "every FM cell has a size", juce::String (unsized));
        check (nonSquare == 0, "every FM value cell is square", juce::String (nonSquare));

        // FM Index used to be a knob. It is the control that proves the section
        // moved onto the grid rather than keeping one thing behind.
        SquareCell* index = nullptr;

        for (auto* c : fmCells)
            if (c->getParamID() == "fmIndex")
                index = c;

        check (index != nullptr, "FM Index is a cell, not a knob");

        // The shape column names the shape rather than showing a bare number, which
        // is what the old knob's label did and what a plain number cell would lose.
        SquareCell* shape = nullptr;

        for (auto* c : fmCells)
            if (c->getParamID() == "op1wave")
                shape = c;

        check (shape != nullptr, "the shape column has a cell per operator");

        if (shape != nullptr)
        {
            auto setWave = [&proc] (float v)
            {
                if (auto* p = proc.apvts.getParameter ("op1wave"))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (v);
                    p->endChangeGesture();
                }
            };

            setWave (0.0f);
            const auto atSine = shape->valueTextForTest();

            setWave (1.0f);
            const auto atSquare = shape->valueTextForTest();

            check (atSine.isNotEmpty() && atSquare.isNotEmpty() && atSine != atSquare,
                   "the shape cell names its shape, and the name follows the morph",
                   atSine + " -> " + atSquare);

            setWave (0.0f);
        }

        auto cellFor = [&allCells] (const juce::String& id) -> SquareCell*
        {
            for (auto* c : allCells)
                if (c->getParamID() == id)
                    return c;

            return nullptr;
        };

        // The two views have to share a column grid. Sizing each to its own content
        // made them different widths, so centring put their left edges in different
        // places and the shared operator column jumped sideways on every switch. Both
        // are laid out from one origin now, so corresponding columns share an x.
        auto* op1RatioCell = cellFor ("op1ratio");
        auto* route12 = cellFor ("mtx12");   // Op1 -> Op2, and both land in column 2

        if (op1RatioCell != nullptr && route12 != nullptr)
            check (op1RatioCell->getX() == route12->getX(),
                   "the value and routing views share a column grid",
                   juce::String (op1RatioCell->getX()) + " vs " + juce::String (route12->getX()));

        // A routing depth drops its text at both ends of the range, where the fill
        // already says what the number would: black means no connection, full means
        // everything. A level must NOT do this - level 0 is silent, and that is the
        // state the audibility warning exists to explain.
        auto* route = cellFor ("mtx12");
        auto* level = cellFor ("op1level");

        if (route != nullptr && level != nullptr)
        {
            auto setParam = [&proc] (const juce::String& id, float normalised)
            {
                if (auto* p = proc.apvts.getParameter (id))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (normalised);
                    p->endChangeGesture();
                }
            };

            setParam ("mtx12", 0.0f);
            const bool quietAtZero = route->valueTextForTest().isEmpty();

            setParam ("mtx12", 1.0f);
            const bool quietAtMax = route->valueTextForTest().isEmpty();

            setParam ("mtx12", 0.5f);
            const bool speaksInMiddle = route->valueTextForTest().isNotEmpty();

            check (quietAtZero, "a routing cell at zero drops its number");
            check (quietAtMax, "a routing cell at maximum drops its number too");
            check (speaksInMiddle, "a routing cell in the middle still shows its value",
                   route->valueTextForTest());

            setParam ("op1level", 0.0f);

            check (level->valueTextForTest().isNotEmpty(),
                   "a level at zero still reports it, because silent is a state",
                   level->valueTextForTest());

            setParam ("mtx12", 0.0f);
            setParam ("op1level", 0.0f);
        }

        // The envelope used to be the grid's last column, so this used to assert it sat one
        // cell-gap after the grid's last value column. It is its own section now, past a
        // divider, so what matters is different: its ROWS line up with the grid's, and the
        // space either side of the grid is the same number.
        auto* op1Shape = cellFor ("op1wave");
        auto* op1Ratio = cellFor ("op1ratio");
        auto* op1Fbk   = cellFor ("op1feedback");

        const auto envelopes = miXXEditor->getAdsrEditors();
        auto* op1Env = envelopes.empty() ? nullptr : envelopes.front();

        if (op1Shape != nullptr && op1Ratio != nullptr && op1Fbk != nullptr && op1Env != nullptr)
        {
            // Row alignment. Laid out from its own origin it would look perfectly fine in
            // isolation and be a whole row out against the grid, so this compares centres
            // rather than merely checking that the section is somewhere to the right.
            check (std::abs (op1Env->getBounds().getCentreY()
                               - op1Fbk->getBounds().getCentreY()) <= 1,
                   "the envelope section's rows line up with the grid's",
                   juce::String (op1Env->getBounds().getCentreY())
                     + " vs " + juce::String (op1Fbk->getBounds().getCentreY()));
        }

        if (op1Shape != nullptr && op1Ratio != nullptr)
        {
            // A divider, not a cell gap: the section is set apart from the grid rather than
            // continuing it. Measured off the grid's own last column, so it has to be wider
            // than the gap between two cells.
            const int cellGap = op1Ratio->getX() - op1Shape->getRight();
            const int dividerGapPx = op1Env != nullptr ? op1Env->getX() - op1Fbk->getRight() : 0;

            check (dividerGapPx > cellGap,
                   "the envelope section is set apart by a divider, not a cell gap",
                   juce::String (dividerGapPx) + " vs " + juce::String (cellGap));
        }

        // Symmetry, checked rather than eyeballed: the space between the VA panel and the grid
        // and the space between the grid and the envelope section are the same number.
        //
        // Measured against the VALUES view's right column, because that is the view the
        // section follows - it tracks whichever view is showing, so measuring against the
        // routing view's last column would report the section as overlapping the grid.
        {
            auto* vaNoise = cellFor ("vaNoiseLevel");
            auto* indexCell2 = cellFor ("fmIndex");

            if (vaNoise != nullptr && indexCell2 != nullptr && op1Env != nullptr && op1Fbk != nullptr)
            {
                const int leftGap  = indexCell2->getX() - vaNoise->getRight();
                const int rightGap = op1Env->getX() - op1Fbk->getRight();

                check (std::abs (leftGap - rightGap) <= 1,
                       "the grid is flanked symmetrically by its two sections",
                       juce::String (leftGap) + " px left vs " + juce::String (rightGap) + " px right");
            }
        }

        // The algorithm-pinning check is NOT here: the audit sets the algorithm to a
        // preset later on to exercise the DSP, so the pinned state only exists right
        // after the editor is built. It is checked there instead.
    }

    // ---- M6: the operator envelopes -------------------------------------------
    // The ADSR column is graphical now, and its time axis is a power curve rather
    // than a straight line. That curve is the part worth testing: if it were not
    // exactly invertible, a drag would not land on the value its handle is drawn
    // at, and the handle would slide away from the pointer as you moved it.
    {
        const auto envelopes = miXXEditor->getAdsrEditors();

        check (static_cast<int> (envelopes.size()) == MiXXFmVoice::numOps,
               "there is one envelope per operator",
               juce::String (static_cast<int> (envelopes.size())));

        int unsized = 0;

        for (auto* e : envelopes)
            if (e->getWidth() <= 0 || e->getHeight() <= 0)
                ++unsized;

        check (unsized == 0, "every envelope has a size", juce::String (unsized));

        // The axis has to round-trip, or the handle and the value disagree.
        int badRoundTrip = 0;

        for (float t : { 0.0f, 0.002f, 0.05f, 0.5f, 2.0f, 4.0f, 8.0f, 14.0f })
            if (std::abs (AdsrEditor::xToTime (AdsrEditor::timeToX (t)) - t) > 0.001f)
                ++badRoundTrip;

        check (badRoundTrip == 0, "the time axis round-trips through the power curve",
               juce::String (badRoundTrip) + " of 8");

        // The reason the axis is curved at all: 2 ms against a 14 s window is
        // 0.014% of the width, which is not a place a pointer can go. These are the
        // two things that have to be true for the curve to be worth having.
        const float shortest = AdsrEditor::timeToX (0.002f);
        const float linear = 0.002f / AdsrEditor::windowSeconds;

        check (shortest > 20.0f * linear,
               "the curve spreads a 2 ms stage far wider than a linear axis would",
               juce::String (shortest, 4) + " vs " + juce::String (linear, 6));
        check (shortest * 300.0f > 8.0f,
               "a 2 ms stage is at least 8 px across a 300 px envelope",
               juce::String (shortest * 300.0f, 2) + " px");

        if (! envelopes.empty())
        {
            auto* env = envelopes.front();

            auto setStage = [&proc] (const juce::String& id, float seconds)
            {
                if (auto* p = proc.apvts.getParameter (id))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (p->convertTo0to1 (seconds));
                    p->endChangeGesture();
                }
            };

            auto readStage = [&proc] (const juce::String& id)
            {
                if (auto* p = proc.apvts.getParameter (id))
                    return p->convertFrom0to1 (p->getValue());

                return 0.0f;
            };

            setStage ("op1attack", 0.1f);
            setStage ("op1decay", 0.4f);
            setStage ("op1sustain", 0.5f);
            setStage ("op1release", 0.8f);

            const auto shape = env->currentShape();

            check (shape.numPoints == 6 && std::abs (shape.points[1].y - 1.0f) < 0.001f,
                   "the envelope shape peaks at full level",
                   juce::String (shape.numPoints) + " points");

            // The plateau is what makes this read as an ADSR rather than an ADR.
            // Without it the curve decays straight into its release and the sustain
            // level is a point rather than a level, which is what the first version of
            // this drew.
            check (std::abs (shape.points[3].y - shape.points[2].y) < 0.001f
                     && shape.points[3].x > shape.points[2].x,
                   "the shape holds a sustain plateau before it releases",
                   juce::String (shape.points[2].x, 3) + " -> "
                     + juce::String (shape.points[3].x, 3));
            check (std::abs (shape.points[2].y - 0.5f) < 0.001f,
                   "the sustain level is the decay corner's height",
                   juce::String (shape.points[2].y, 3));

            // Dragging the attack peak right lengthens the attack.
            env->dragHandleForTest (AdsrEditor::attackPeak, { 0.5f, 1.0f });

            check (readStage ("op1attack") > 0.1f,
                   "dragging the attack peak right lengthens the attack",
                   juce::String (readStage ("op1attack"), 3));

            // The decay corner carries two values: the decay time and the sustain.
            setStage ("op1attack", 0.1f);
            env->dragHandleForTest (AdsrEditor::decayCorner, { 0.5f, 0.25f });

            check (std::abs (readStage ("op1sustain") - 0.25f) < 0.001f,
                   "dragging the decay corner sets the sustain level",
                   juce::String (readStage ("op1sustain"), 3));
            check (readStage ("op1decay") > 0.0f,
                   "dragging the decay corner right lengthens the decay",
                   juce::String (readStage ("op1decay"), 3));

            // The release cannot be dragged left past the decay corner, because that
            // would invert the envelope.
            env->dragHandleForTest (AdsrEditor::releaseEnd, { 0.0f, 0.0f });

            check (readStage ("op1release") >= 0.0f && readStage ("op1release") < 0.01f,
                   "the release clamps to zero rather than inverting the envelope",
                   juce::String (readStage ("op1release"), 4));

            // And a stage clamps at its own parameter maximum, not past it.
            env->dragHandleForTest (AdsrEditor::attackPeak, { 1.0f, 1.0f });

            check (std::abs (readStage ("op1attack") - 2.0f) < 0.01f,
                   "the attack clamps at its parameter maximum",
                   juce::String (readStage ("op1attack"), 3));

            // Leave the envelope where the rest of the audit expects the init patch.
            setStage ("op1attack", 0.0f);
            setStage ("op1decay", 0.0f);
            setStage ("op1sustain", 0.0f);
            setStage ("op1release", 0.0f);
        }
    }

    std::printf (failures == 0 ? "EDITOR AUDIT OK\n" : "EDITOR AUDIT FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
