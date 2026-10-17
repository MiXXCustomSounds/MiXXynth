#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // A rotary speaker: the Leslie.
    //
    // Two rotors, not one, and that is the whole effect:
    //
    //   in -> drive -> crossover --+-- high --> HORN rotor (fast) --+
    //                              |                                 |--> balance -> mix -> out
    //                              +-- low  --> DRUM rotor (slow) --+
    //
    // A single rotating tremolo is a panner. What makes a Leslie sound like a Leslie is that the
    // HORN and the DRUM turn at different speeds, so the treble and the bass move against each
    // other, and that the two microphones hear each rotor from different places.
    //
    // Each rotor is modelled as what it physically is - a source travelling a circular path, seen
    // by two fixed mics. That produces three things at once, and they are why this widens a signal
    // rather than merely wobbling it:
    //
    //   * DIRECTIVITY - each mic's gain follows where the rotor is pointing, so the two channels
    //     rise and fall out of phase. This is the movement you hear.
    //   * DOPPLER - the path length to each mic changes as the rotor sweeps, so each channel's
    //     delay is modulated by the rotor angle. A source approaching is a source sharpened, and
    //     the pitch wobble is most of what the ear calls "rotary".
    //   * MIC SPACING - the mics sit either side of the front, so the two channels never do the
    //     same thing at the same moment. That phase difference IS the stereo image.
    //
    // The drum is the heavier rotor, so it turns slower. That ratio is fixed rather than exposed:
    // it is a property of the cabinet, not a control.
    //
    // A rotary is a MONO cabinet fed from a mono amplifier, so each band is summed to mono before
    // it is rotated. That is not a shortcut - it is what the effect is - and Mix is how you keep
    // some of the input's own stereo image alongside it.
    class RotaryFx : public FxModule
    {
    public:
        enum Param
        {
            pRate = 0,   // horn rotation speed, Hz
            pDepth,      // Doppler / path-length modulation
            pWidth,      // mic separation: how far apart the two mics sit
            pCrossover,  // horn/drum split, Hz
            pBalance,    // drum vs horn level
            pDrive,      // cabinet saturation
            pMix,
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below deletes the copy
        // constructor, and a user-declared member suppresses the implicit default one. Without
        // this line the class cannot be constructed at all.
        RotaryFx() = default;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        // The drum turns this much slower than the horn. A cabinet's drum is the heavier rotor;
        // this ratio is what makes the bass and the treble drift apart instead of moving as one.
        static constexpr float kDrumRateRatio = 0.83f;

        // Where the two mics sit, in radians, either side of the front - about 110 degrees apart.
        // A close pair: a wider one turns the phase difference into a plain tremolo.
        static constexpr float kMicAngle = 0.96f;

        // The rotor's distance from the mics, and the radius it travels, as times. The path to a
        // mic is `base - sweep * cos(angle - mic)`, so the sweep must stay below the base or the
        // delay goes negative.
        static constexpr float kBaseDelaySeconds = 0.0010f;
        static constexpr float kMaxSweepSeconds  = 0.0008f;

        // The longest delay either rotor can ask for, with room above the sweep.
        static constexpr float kMaxDelaySeconds = 0.004f;

        // The two rotors do not start together. A real cabinet's are independent, and starting
        // them aligned makes the first rotation read as one effect rather than two.
        static constexpr double kDrumStartPhase = 0.37;

    private:
        // A delay line with a fractional read, so a rotor can sweep smoothly between samples.
        struct DelayLine
        {
            std::vector<float> buffer;
            int writeIndex = 0;

            void resize (int lengthSamples);
            void clear();
            void write (float sample) noexcept;

            // Reads `delaySamples` back from the write head, linearly interpolated.
            float read (float delaySamples) const noexcept;
        };

        // One rotor: where it is pointing, and the line both mics read from.
        //
        // ONE line rather than one per channel, because both mics hear the same source - they
        // differ only in how far away it is. Two lines would be the same samples twice.
        struct Rotor
        {
            double angle = 0.0;
            DelayLine line;
        };

        double sampleRate = 44100.0;
        int channels = 2;
        bool prepared = false;

        Rotor horn;
        Rotor drum;

        // The crossover, two cascaded one-poles (12 dB/oct). The low band is the filtered signal
        // and the high band is the remainder, so the two always sum back to the input - a
        // crossover whose bands do not sum flat is a filter wearing a crossover's name.
        float lowLp1[2] { 0.0f, 0.0f };
        float lowLp2[2] { 0.0f, 0.0f };

        // The sweep is SLEWED rather than set. A step in the modulation depth is a step in the
        // read position, and a step in a delay line is a click.
        float sweepSm = 0.0f;
        bool sweepPrimed = false;

        // How fast the sweep follows the control. Long enough to glide, short enough that the
        // knob still feels connected.
        static constexpr float kSweepSlewSeconds = 0.03f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotaryFx)
    };
}
