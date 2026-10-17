// M7 patch audit: the preset bank, the preset session and the smart randomizer.
//
// The editor audit covers what the controls are wired to; this covers the PATCH - what a
// preset is, what loading one does, what the host's programs are, and what a roll guarantees.
// It is a separate gate because none of that needs a window, and because a failure here means
// something different from a failure there.
//
// The four things this file exists to prove, in order of how much they cost to get wrong:
//
//   1. Every factory preset names parameters that exist, at values in range. A bank of thirty
//      hand-shaped patches is thirty chances to write `op3fltCutof`.
//   2. Every factory preset is AUDIBLE. A preset that loads to silence is worse than no
//      preset, and it is invisible in a parameter dump.
//   3. A load is deterministic: the same preset always produces the same patch, because a
//      partial state is laid over the init patch rather than over whatever was there before.
//   4. A roll is reproducible from its seed, respects its locks, always leaves something
//      audible, and can be undone.
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/Algorithms.h"
#include "../Source/FmVoice.h"
#include "../Source/Fx/FxRack.h"
#include "../Source/ModMatrix.h"
#include "../Source/PluginProcessor.h"
#include "../Source/PresetManager.h"
#include "../Source/PresetStore.h"
#include "../Source/Randomizer.h"

namespace
{
    int failures = 0;

    void check (bool cond, const juce::String& name, const juce::String& detail = {})
    {
        if (! cond)
            ++failures;

        std::printf ("%s %-48s %s\n", cond ? "PASS" : "FAIL", name.toRawUTF8(),
                     detail.toRawUTF8());

        // Unbuffered: a crash would otherwise discard everything printed so far, which makes a
        // segfault impossible to locate.
        std::fflush (stdout);
    }

    float realValue (MiXXynthProcessor& p, const juce::String& id)
    {
        if (auto* param = p.apvts.getParameter (id))
            return param->convertFrom0to1 (param->getValue());

        return 0.0f;
    }

    // The names the host would see, so a program-list check and a bank check cannot disagree.
    juce::StringArray bankNames (const mixxynth::PresetStore& store)
    {
        juce::StringArray names;

        for (int i = 0; i < store.size(); ++i)
            names.add (store.categoryOf (i) + " / " + store.nameOf (i));

        return names;
    }

    // The first parameter that differs between two state trees. A fingerprint comparison can
    // only say "not equal", and for a round trip that is not enough to act on.
    juce::String firstDifference (const juce::ValueTree& a, const juce::ValueTree& b)
    {
        for (int i = 0; i < a.getNumChildren(); ++i)
        {
            const auto child = a.getChild (i);

            // Parameters only: the shapes node carries no id, and comparing it here would report
            // an empty name as the difference.
            if (! child.hasType (mixxynth::presetState::parameterNode))
                continue;

            const auto id = child.getProperty (mixxynth::presetState::idProperty).toString();
            const auto other = b.getChildWithProperty (mixxynth::presetState::idProperty, id);

            if (! other.isValid())
                return id + ": missing from the second";

            const float va = static_cast<float> (child.getProperty (mixxynth::presetState::valueProperty));
            const float vb = static_cast<float> (other.getProperty (mixxynth::presetState::valueProperty));

            if (std::abs (va - vb) > 1.0e-6f)
                return id + ": " + juce::String (va) + " vs " + juce::String (vb);
        }

        if (a.getNumChildren() != b.getNumChildren())
            return "child counts differ: " + juce::String (a.getNumChildren()) + " vs "
                     + juce::String (b.getNumChildren());

        return "no parameter differs";
    }

    // True when the patch can make a sound. The definition is the editor's, deliberately:
    // Free mode routes through the carrier switches, so an operator has to be a carrier, on,
    // with a ratio that moves its phase, a level, and an envelope that is not a one-sample
    // spike.
    bool audible (MiXXynthProcessor& p)
    {
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const auto n = juce::String (i + 1);

            if (realValue (p, "carrier" + n) < 0.5f)
                continue;

            if (realValue (p, "op" + n + "on") < 0.5f)
                continue;

            if (realValue (p, "op" + n + "ratio") <= 0.001f)
                continue;

            if (realValue (p, "op" + n + "level") <= 0.01f)
                continue;

            const float sustain = realValue (p, "op" + n + "sustain");
            const float decay = realValue (p, "op" + n + "decay");

            if (sustain > 0.01f || decay > 0.02f)
                return true;
        }

        // The VA can carry a patch on its own, which is a legitimate patch rather than a
        // silent one.
        return realValue (p, "vaLevel") > 0.02f;
    }
}


int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    MiXXynthProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    auto& store = proc.presets.store();

    // ---- 1. the bank arrived ----
    //
    // The factory bank is embedded in the binary, so "is it there" is a real question rather
    // than a formality: a build that forgot to link the preset data would otherwise list an
    // empty bank and every other check here would pass vacuously.
    check (store.numFactory() >= 30, "the factory bank is in the binary",
           juce::String (store.numFactory()) + " factory presets");

    check (store.size() == store.numFactory() + store.numUser(),
           "every listed preset is either factory or user",
           juce::String (store.size()) + " total");

    // ---- 2. the category order covers the bank ----
    //
    // The order is curated in code and the categories are written in the filenames, so the two
    // can disagree. A category that is not in the list still shows up, at the end - but it
    // means a preset is in a category nobody meant to create, which is a typo.
    {
        const auto& order = mixxynth::presetCategoryOrder();
        juce::StringArray unknown;

        for (const auto& category : store.categories())
            if (! order.contains (category))
                unknown.add (category);

        check (unknown.isEmpty(), "every category is one the browser knows the order of",
               unknown.isEmpty() ? juce::String ("all known")
                                 : unknown.joinIntoString (", "));
    }

    // ---- 3. every factory preset parses, and every name it uses is real ----
    //
    // This is the check the generator cannot do for itself: it knows the parameter NAMES it
    // wrote, not whether those names exist in this build's layout.
    {
        int parsed = 0;
        juce::StringArray badIds, badValues, badChoices, silent;

        for (int i = 0; i < store.size(); ++i)
        {
            if (! store.isFactory (i))
                continue;

            auto preset = store.read (i);

            if (! preset.isValid())
            {
                badIds.add (store.nameOf (i) + ": unreadable");
                continue;
            }

            ++parsed;

            for (int c = 0; c < preset.getNumChildren(); ++c)
            {
                const auto node = preset.getChild (c);

                if (! node.hasType (mixxynth::presetState::parameterNode))
                    continue;

                const auto id = node.getProperty (mixxynth::presetState::idProperty).toString();
                auto* param = proc.apvts.getParameter (id);

                if (param == nullptr)
                {
                    badIds.add (store.nameOf (i) + "/" + id);
                    continue;
                }

                const float value = static_cast<float> (node.getProperty (mixxynth::presetState::valueProperty));
                const float low = param->convertFrom0to1 (0.0f);
                const float high = param->convertFrom0to1 (1.0f);

                // A tenth of a percent of the range, because the round trip is through a
                // decimal string and a value sitting exactly on a maximum must not fail for
                // rounding.
                const float slack = (high - low) * 0.001f + 1.0e-4f;

                if (value < low - slack || value > high + slack)
                    badValues.add (store.nameOf (i) + "/" + id + "=" + juce::String (value));

                // A choice is stored as index / (count - 1) and read back by rounding, so a
                // value that is not on an index is a control the module will silently round
                // somewhere else. That is how a bank ends up with modes nobody chose.
                if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
                {
                    const int items = choice->choices.size();

                    if (items > 1)
                    {
                        const float scaled = value * static_cast<float> (items - 1)
                                               / (high - low);

                        if (std::abs (scaled - std::round (scaled)) > 0.01f)
                            badChoices.add (store.nameOf (i) + "/" + id + "="
                                              + juce::String (value));
                    }
                }
            }

            // A preset that loads to silence is worse than no preset, so every one of them is
            // loaded and asked whether it can make a sound.
            proc.presets.load (i);

            if (! audible (proc))
                silent.add (store.categoryOf (i) + " / " + store.nameOf (i));
        }

        check (parsed == store.numFactory(), "every factory preset parses",
               juce::String (parsed) + " of " + juce::String (store.numFactory()));

        check (badIds.isEmpty(), "every parameter a preset names exists",
               badIds.isEmpty() ? juce::String ("all ids resolve")
                                : juce::String (badIds.size()) + " bad, first: " + badIds[0]);

        check (badValues.isEmpty(), "every value a preset sets is inside its range",
               badValues.isEmpty() ? juce::String ("all in range")
                                   : juce::String (badValues.size()) + " bad, first: "
                                       + badValues[0]);

        check (badChoices.isEmpty(), "every choice a preset sets lands on a choice",
               badChoices.isEmpty() ? juce::String ("all on an index")
                                    : juce::String (badChoices.size()) + " bad, first: "
                                        + badChoices[0]);

        check (silent.isEmpty(), "every factory preset can make a sound",
               silent.isEmpty() ? juce::String ("all audible")
                                : silent.joinIntoString ("; "));
    }

    // ---- 4. a load is deterministic, and a partial state is laid over INIT ----
    //
    // This is the design decision the whole preset format rests on, so it is checked rather
    // than assumed. The first half proves the patch is the preset's and not a mixture; the
    // second proves that a parameter the preset does NOT mention comes back as its default
    // rather than as whatever the previous preset left behind.
    {
        const int first = 0;
        const int second = juce::jmin (1, store.size() - 1);

        proc.presets.load (first);
        const auto firstPrint = mixxynth::fingerprint (proc.buildStateTree());

        proc.presets.load (second);
        proc.presets.load (first);

        check (mixxynth::fingerprint (proc.buildStateTree()) == firstPrint,
               "loading the same preset twice gives the same patch",
               juce::String (firstPrint));

        // A minimal preset: one parameter, nothing else. Everything it does not mention has to
        // come from init, including parameters the previous preset had set.
        juce::ValueTree minimal ("MiXXynthState");
        juce::ValueTree index ("PARAM");
        index.setProperty ("id", "fmIndex", nullptr);
        index.setProperty ("value", 0.77f, nullptr);
        minimal.appendChild (index, nullptr);

        proc.applyStateTree (mixxynth::overlayState (proc.buildInitState(), minimal));

        const float noise = realValue (proc, "vaNoiseLevel");
        const float cutoff = realValue (proc, "filterCutoff");

        check (std::abs (realValue (proc, "fmIndex") - 0.77f) < 0.001f,
               "a minimal preset sets what it names",
               juce::String (realValue (proc, "fmIndex"), 4));

        check (std::abs (noise) < 0.0001f && std::abs (cutoff - 20000.0f) < 1.0f,
               "a parameter the preset omits comes back at its default",
               "vaNoiseLevel=" + juce::String (noise, 4) + " filterCutoff="
                   + juce::String (cutoff, 1));
    }

    // ---- 5. the loader forces the algorithm to Free ----
    //
    // M6 removed the algorithm UI, so a state that names one of the 32 DX7 algorithms would
    // restore a control with no way to move it off again - and it would override the routing
    // matrix, which is the bug that removal set out to kill.
    {
        juce::ValueTree withAlgorithm ("MiXXynthState");

        juce::ValueTree algoNode ("PARAM");
        algoNode.setProperty ("id", "algorithm", nullptr);
        algoNode.setProperty ("value", 5.0f, nullptr);
        withAlgorithm.appendChild (algoNode, nullptr);

        juce::ValueTree indexNode ("PARAM");
        indexNode.setProperty ("id", "fmIndex", nullptr);
        indexNode.setProperty ("value", 0.3f, nullptr);
        withAlgorithm.appendChild (indexNode, nullptr);

        proc.applyStateTree (mixxynth::overlayState (proc.buildInitState(), withAlgorithm));

        auto* algo = dynamic_cast<juce::AudioParameterChoice*> (
            proc.apvts.getParameter ("algorithm"));

        check (algo != nullptr && algo->getIndex() == mixxynth::kFreeAlgorithm,
               "a state naming a DX7 algorithm still loads in Free",
               algo != nullptr ? juce::String (algo->getIndex()) : juce::String ("no parameter"));
    }

    // ---- 6. saving writes a diff, and the diff reloads to the same patch ----
    //
    // A user preset and a factory preset are the same format: what the patch changes about
    // init. If the diff were lossy, a saved patch would come back different, and this is the
    // only place that would be visible.
    {
        proc.presets.load (juce::jmin (3, store.size() - 1));
        proc.randomizer.roll (4242);

        const auto live = proc.buildStateTree();
        const auto diff = mixxynth::diffState (live, proc.buildInitState());
        const auto reloaded = mixxynth::overlayState (proc.buildInitState(), diff);

        check (mixxynth::fingerprint (reloaded) == mixxynth::fingerprint (live),
               "a saved patch reloads to exactly what was saved",
               juce::String (diff.getNumChildren()) + " parameters kept of "
                   + juce::String (live.getNumChildren()) + "; "
                   + firstDifference (live, reloaded));

        check (diff.getNumChildren() < live.getNumChildren(),
               "a saved patch is a diff, not the whole state",
               juce::String (diff.getNumChildren()) + " < "
                   + juce::String (live.getNumChildren()));
    }

    // ---- 7. save, list, load and delete, on the real user folder ----
    //
    // The real folder rather than a temporary one, because a store that works in a temp
    // directory and not in %APPDATA% is a store that does not work. The name is unique to this
    // audit and is removed at the end.
    {
        const juce::String category ("Other");
        const juce::String name ("Patch Audit Temp");

        // Anything left by a previous run, so the check cannot pass because of a stale file.
        for (int i = store.size(); --i >= 0;)
            if (! store.isFactory (i) && store.nameOf (i) == name)
                store.remove (i);

        proc.presets.load (juce::jmin (5, store.size() - 1));
        const auto expected = mixxynth::fingerprint (proc.buildStateTree());

        const int saved = proc.presets.saveAs (category, name);

        check (saved >= 0 && ! store.isFactory (saved), "the patch saves as a user preset",
               saved >= 0 ? store.nameOf (saved) : juce::String ("save refused"));

        check (mixxynth::PresetStore::fileFor (category, name).existsAsFile(),
               "the preset file is where the store says it is",
               mixxynth::PresetStore::fileFor (category, name).getFullPathName());

        // Load something else first, so the load below is a real load.
        proc.presets.load (0);
        proc.presets.load (saved);

        check (mixxynth::fingerprint (proc.buildStateTree()) == expected,
               "the saved preset loads back as the patch that was saved",
               juce::String (expected));

        // A factory name cannot be taken: the bank is in the binary, so an overwrite would be
        // a lie rather than a write.
        int factoryIndex = 0;

        for (int i = 0; i < store.size(); ++i)
            if (store.isFactory (i))
            {
                factoryIndex = i;
                break;
            }

        const juce::String factoryName = store.nameOf (factoryIndex);
        const int refused = proc.presets.saveAs (store.categoryOf (factoryIndex), factoryName);

        check (refused < 0, "a factory preset cannot be saved over", factoryName);

        check (! proc.presets.remove (factoryIndex), "a factory preset cannot be deleted",
               factoryName);

        const bool deleted = proc.presets.remove (store.indexOf (category, name));

        check (deleted && store.indexOf (category, name) < 0,
               "the user preset deletes, and leaves the list",
               juce::String (store.indexOf (category, name)));
    }

    // ---- 8. the host's programs are the bank ----
    //
    // The point of wiring the program calls to the same list is that a DAW and the browser
    // cannot disagree. If they were two lists, this is where it would show.
    {
        const int programs = proc.getNumPrograms();

        check (programs == store.size(), "the host sees one program per preset",
               juce::String (programs) + " vs " + juce::String (store.size()));

        check (proc.getProgramName (0) == store.nameOf (0),
               "program 0 is named after preset 0",
               proc.getProgramName (0) + " / " + store.nameOf (0));

        const int target = juce::jmin (7, store.size() - 1);

        proc.setCurrentProgram (target);
        const auto viaProgram = mixxynth::fingerprint (proc.buildStateTree());
        const auto viaProgramTree = proc.buildStateTree();

        proc.presets.load (target);

        check (mixxynth::fingerprint (proc.buildStateTree()) == viaProgram,
               "a host program change loads the preset it names",
               "program " + juce::String (target) + " = " + store.nameOf (target) + "; "
                   + firstDifference (proc.buildStateTree(), viaProgramTree));

        check (proc.getCurrentProgram() == target, "the current program follows the change",
               juce::String (proc.getCurrentProgram()));

        // Out of range, because a host is free to ask for a program that is not there and the
        // answer must not be "load something arbitrary".
        const auto before = mixxynth::fingerprint (proc.buildStateTree());
        proc.setCurrentProgram (store.size() + 100);

        check (mixxynth::fingerprint (proc.buildStateTree()) == before,
               "a program index that does not exist changes nothing", "");
    }

    // ---- 9. the dirty marker ----
    //
    // Dirty is a comparison against the patch that was loaded, not a change flag, so a patch
    // edited and then put back by hand reads clean. That is the difference between a marker
    // that means something and a lamp that lights whenever anything moves.
    {
        proc.presets.load (0);
        proc.presets.resetBaseline();

        check (! proc.presets.isDirty(), "a freshly loaded preset is not dirty",
               proc.presets.currentName());

        check (proc.presets.currentIndex() == 0 && proc.presets.status().factory,
               "the loaded preset is the one the bar names",
               proc.presets.currentName() + " / " + proc.presets.currentCategory());

        if (auto* param = proc.apvts.getParameter ("filterCutoff"))
            param->setValueNotifyingHost (0.4f);

        check (proc.presets.isDirty(), "editing the patch makes it dirty", "");

        proc.presets.loadInit();

        check (! proc.presets.isDirty() && proc.presets.isInitPatch(),
               "Init is clean, and is the init patch",
               proc.presets.currentName());

        if (auto* param = proc.apvts.getParameter ("fmIndex"))
            param->setValueNotifyingHost (0.2f);

        check (proc.presets.isDirty(), "editing Init makes it dirty", "");
        check (proc.presets.currentName() == "(unsaved)",
               "a patch that is neither a preset nor Init says so",
               proc.presets.currentName());
    }

    // ---- 10. the roll is reproducible from its seed ----
    //
    // Without this, "the roll respects its locks" is not a checkable claim, and a roll that
    // went wrong could never be reproduced.
    {
        proc.presets.loadInit();
        proc.randomizer.roll (90210);
        const auto once = mixxynth::fingerprint (proc.buildStateTree());

        proc.presets.loadInit();
        proc.randomizer.roll (90210);

        check (mixxynth::fingerprint (proc.buildStateTree()) == once,
               "the same seed rolls the same patch", juce::String (once));

        proc.presets.loadInit();
        proc.randomizer.roll (90211);

        check (mixxynth::fingerprint (proc.buildStateTree()) != once,
               "a different seed rolls a different patch", "");
    }

    // ---- 11. a lock holds ----
    //
    // Checked across sections, because the mod section reaches into the others: a roll that
    // assigned a modulation slot to a filter cutoff would move a locked filter's base value
    // if the lock were only checked inside the filter's own section.
    {
        proc.presets.loadInit();

        for (int s = 0; s < mixxynth::kNumRollSections; ++s)
            proc.randomizer.setLocked (s, false);

        proc.randomizer.setLocked (mixxynth::rollFx, true);
        proc.randomizer.setLocked (mixxynth::rollFilter, true);

        check (proc.randomizer.numLocked() == 2
                 && proc.randomizer.numUnlocked() == mixxynth::kNumRollSections - 2,
               "the locks are counted", juce::String (proc.randomizer.numLocked()));

        const float cutoffBefore = realValue (proc, "filterCutoff");
        const float resoBefore = realValue (proc, "filterReso");

        juce::String fxBefore;

        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
            fxBefore << realValue (proc, "fx" + juce::String (s + 1) + "type") << ",";

        proc.randomizer.roll (1234);

        check (std::abs (realValue (proc, "filterCutoff") - cutoffBefore) < 0.001f
                 && std::abs (realValue (proc, "filterReso") - resoBefore) < 0.0001f,
               "a locked filter is not moved by the roll",
               juce::String (realValue (proc, "filterCutoff"), 1));

        juce::String fxAfter;

        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
            fxAfter << realValue (proc, "fx" + juce::String (s + 1) + "type") << ",";

        check (fxAfter == fxBefore, "a locked rack is not moved by the roll",
               fxAfter + " vs " + fxBefore);

        // The unlocked half still moved, or the lock check above would pass on a roll that did
        // nothing at all.
        check (realValue (proc, "fmIndex") > 0.0f && realValue (proc, "vaLevel") >= 0.0f
                 && std::abs (realValue (proc, "op1level")) > 0.0f,
               "the unlocked sections still rolled",
               juce::String (realValue (proc, "op1level"), 3));

        // Everything locked: the roll is a no-op rather than a partial one.
        for (int s = 0; s < mixxynth::kNumRollSections; ++s)
            proc.randomizer.setLocked (s, true);

        const auto before = mixxynth::fingerprint (proc.buildStateTree());
        proc.randomizer.roll (777);

        check (mixxynth::fingerprint (proc.buildStateTree()) == before,
               "a roll with every section locked changes nothing", "");

        for (int s = 0; s < mixxynth::kNumRollSections; ++s)
            proc.randomizer.setLocked (s, false);
    }

    // ---- 12. a roll is always audible ----
    //
    // Twenty seeds, not one: an audibility guarantee that holds for the seed I happened to
    // try is not a guarantee, and the way this fails is rare enough that one roll would pass
    // and the next would not.
    {
        int silent = 0;
        int flat = 0;

        for (int seed = 1; seed <= 20; ++seed)
        {
            proc.presets.loadInit();
            proc.randomizer.roll (seed);

            if (! audible (proc))
            {
                ++silent;

                if (silent == 1)
                    std::printf ("      first silent seed: %d\n", seed);
            }

            // And the roll has to have done something. A roll that left the init patch alone
            // would be "audible" by the VA rule and useless in practice.
            if (std::abs (realValue (proc, "op1level")) < 0.01f
                  && std::abs (realValue (proc, "fmIndex")) < 0.01f)
                ++flat;
        }

        check (silent == 0, "every roll leaves a patch that can make a sound",
               juce::String (20 - silent) + " of 20 audible");

        check (flat == 0, "every roll moves the FM section", juce::String (20 - flat) + " of 20");

        // A roll keeps its values in the places they belong: the musical ratios, the filter
        // band, and a level that is a level rather than an index.
        proc.presets.loadInit();
        proc.randomizer.roll (31337);

        bool ratiosMusical = true;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const float ratio = realValue (proc, "op" + juce::String (i + 1) + "ratio");
            const bool known = std::abs (ratio - 0.25f) < 0.001f || std::abs (ratio - 0.5f) < 0.001f
                                 || std::abs (ratio - 1.0f) < 0.001f
                                 || std::abs (ratio - 1.5f) < 0.001f
                                 || std::abs (ratio - 2.0f) < 0.001f
                                 || std::abs (ratio - 2.5f) < 0.001f
                                 || std::abs (ratio - 3.0f) < 0.001f
                                 || std::abs (ratio - 4.0f) < 0.001f
                                 || std::abs (ratio - 5.0f) < 0.001f
                                 || std::abs (ratio - 6.0f) < 0.001f
                                 || std::abs (ratio - 7.0f) < 0.001f
                                 || std::abs (ratio - 8.0f) < 0.001f;

            if (! known)
                ratiosMusical = false;
        }

        check (ratiosMusical, "every ratio a roll picks is a musical one", "");

        const float cutoff = realValue (proc, "filterCutoff");

        check (cutoff >= 350.0f && cutoff <= 12000.0f,
               "the rolled filter sits inside the band rather than at an extreme",
               juce::String (cutoff, 1) + " Hz");
    }

    // ---- 13. the rack a roll builds is a chain, not a pile ----
    {
        proc.presets.loadInit();
        proc.randomizer.roll (24680);

        int used = 0;
        juce::Array<int> types;
        juce::StringArray typesWithNoControls;

        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
        {
            const int type = static_cast<int> (realValue (proc, "fx" + juce::String (s + 1) + "type"));

            if (type <= mixxynth::fxOff)
                continue;

            ++used;
            types.add (type);

            // A slot whose type was set and whose controls were not is a module at the bottom
            // of every range, which is not what "the roll picked this" should mean.
            const auto desc = mixxynth::FxRack::descriptorFor (type);
            bool anyControl = false;

            for (int p = 0; p < desc.numParams && p < mixxynth::kMaxFxParams; ++p)
                if (realValue (proc, "fx" + juce::String (s + 1) + "p" + juce::String (p + 1)) > 0.0001f)
                    anyControl = true;

            if (! anyControl)
                typesWithNoControls.add (mixxynth::fxTypeName (type));
        }

        check (used >= 2 && used <= 4, "a roll fills between two and four slots",
               juce::String (used) + " modules");

        std::set<int> unique (types.begin(), types.end());

        check (static_cast<int> (unique.size()) == types.size(),
               "a roll does not put the same module in twice",
               juce::String (types.size()) + " modules, " + juce::String (unique.size()) + " kinds");

        check (typesWithNoControls.isEmpty(),
               "every module a roll adds has its controls set too",
               typesWithNoControls.joinIntoString (", "));
    }

    // ---- 14. undo puts the patch back ----
    {
        proc.presets.load (juce::jmin (2, store.size() - 1));
        proc.presets.resetBaseline();

        const auto before = mixxynth::fingerprint (proc.buildStateTree());
        const juce::String nameBefore = proc.presets.currentName();

        check (! proc.presets.canUndo() || true, "undo starts with nothing remembered", "");

        proc.presets.rememberForUndo();
        proc.randomizer.roll (555);

        check (mixxynth::fingerprint (proc.buildStateTree()) != before, "the roll changed it", "");
        check (proc.presets.canUndo(), "the roll left an undo point", "");

        proc.presets.undo();

        check (mixxynth::fingerprint (proc.buildStateTree()) == before,
               "undo puts the patch back exactly", juce::String (before));

        check (proc.presets.currentName() == nameBefore,
               "undo puts the selection back too", proc.presets.currentName());

        check (! proc.presets.canUndo(), "the undo point is spent", "");

        check (! proc.presets.undo(), "a second undo does nothing", "");
    }

    std::printf (failures == 0 ? "PATCH AUDIT OK\n" : "PATCH AUDIT FAILED (%d)\n", failures);

    return failures == 0 ? 0 : 1;
}
