#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace mixxynth
{
    // The Snap switch.
    //
    // Snap mode makes a drag land on values that MEAN something rather than on whatever the
    // cursor happened to be over: musical ratios, round detune steps, decade times. Free mode
    // is what the plugin has always done - the value follows the cursor exactly.
    //
    // Neither is right for everything, which is why it is a switch and not a decision. Free is
    // what you want while you are hunting for a sound; Snap is what you want once you know
    // what you are aiming at and do not want to fight the mouse for 2.000.
    //
    // The snap sets are chosen BY PARAMETER ID, not by range. There is no way to tell a ratio
    // from a level by looking at a range - both are small floats with a skew - and guessing
    // would snap the wrong controls in a way that would be very hard to notice and very
    // annoying to use.
    class SnapValues
    {
    public:
        // The switch itself. A process-wide flag rather than a member, because the knobs and
        // the cells that need it are deep inside component hierarchies that have no route to
        // the editor - and there is exactly one editor.
        static bool isEnabled() noexcept;
        static void setEnabled (bool shouldSnap) noexcept;

        // The snapped normalised value for a parameter, or `normalised` unchanged when the
        // parameter has no useful set or Snap is off.
        static float snap (const juce::RangedAudioParameter& param, float normalised);

        // The number of snap points a parameter has, for the tests. Zero means "no set", in
        // which case snap() returns its input.
        static int numSnapPointsFor (const juce::String& paramId);

        // The snap points themselves, in real units. Exposed so the tests can check that a
        // ratio snaps to 2.0 rather than merely to something.
        static const float* snapPointsFor (const juce::String& paramId, int& count);
    };
}
