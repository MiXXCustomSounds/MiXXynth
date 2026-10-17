#pragma once

#include <array>

#include <juce_core/juce_core.h>

class MiXXynthProcessor;

namespace mixxynth
{
    // The five things a roll can move, and the five things it can be told not to.
    enum RollSection
    {
        rollFm = 0,
        rollVa,
        rollFilter,
        rollMod,
        rollFx,
        kNumRollSections
    };

    // Short name for a lock button: "FM", "VA", "FILT", "MOD", "FX".
    const char* rollSectionName (int section) noexcept;

    // The smart randomizer (M7, and SPEC S8's "randomization CORE").
    //
    // One roll over the sections that are not locked, constrained to values that make a
    // usable patch rather than to each parameter's full range. The constraint is the whole
    // point: 458 parameters drawn uniformly is noise, and a synth that can roll noise is a
    // synth whose dice nobody presses twice.
    //
    // Seeded, so the same seed and the same locks produce the same patch. That is what makes
    // "the roll respects its locks" a check rather than an opinion.
    //
    // Message thread. Every change goes through the APVTS, so the host sees it as a
    // parameter change and the editor's existing poll picks it up with no extra plumbing.
    class Randomizer
    {
    public:
        explicit Randomizer (MiXXynthProcessor&);

        void setLocked (int section, bool locked);
        bool isLocked (int section) const;
        int numLocked() const;
        int numUnlocked() const;

        // Rolls every unlocked section.
        //
        // The undo point is NOT captured here: undo belongs to the patch session rather than to
        // the dice, so the caller pairs this with PresetManager::rememberForUndo(). The bar does
        // both in one gesture, and the patch audit checks the pair.
        void roll (juce::int64 seed);

        // A seed from the clock, for the button. The tests pass their own so they can say
        // what the roll should have done rather than only that it did something.
        static juce::int64 clockSeed();

    private:
        // Named "…Section" rather than "rollFm" and so on, because the section enum already
        // owns those names - and a member function that shadows an enumerator is a call that
        // silently passes a function pointer where an int was meant.
        void rollFmSection (juce::Random&);
        void rollVaSection (juce::Random&);
        void rollFilterSection (juce::Random&);
        void rollModSection (juce::Random&);
        void rollFxSection (juce::Random&);

        // Puts a modulation destination somewhere the modulation can be heard, so a rolled
        // slot does something instead of sweeping a parameter that has no room left.
        void placeDestination (const juce::String& paramId, juce::Random&);

        // Real units in and out, through the parameter, so nothing here has to know about
        // normalised ranges or skews.
        void set (const juce::String& id, float realValue);
        float get (const juce::String& id) const;

        // A value from `low` to `high`, either way round.
        static float between (juce::Random&, float low, float high);

        // An exponential draw: equal odds per octave, which is what a frequency wants.
        static float betweenLog (juce::Random&, float low, float high);

        // The choice index of a generic FX slot parameter, and the value that selects one.
        // The rack stores a choice as index / (count - 1) and reads it back by rounding.
        static float fxChoiceValue (int index, int count);

        MiXXynthProcessor& proc;

        std::array<bool, kNumRollSections> locked {};
    };
}
