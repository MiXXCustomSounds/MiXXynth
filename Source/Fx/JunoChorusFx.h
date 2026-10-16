#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // The Roland Juno-60 / 106 chorus.
    //
    //   in -> +--> L: triangle LFO (+) -> BBD delay -> 12 dB LP -+
    //         |                                                  |--> mix -> out
    //         +--> R: triangle LFO (-) -> BBD delay -> 12 dB LP -+
    //
    // Emulation rather than a flavoured chorus, so the numbers come from measured
    // hardware rather than from taste. The measurements below are from a Juno-60
    // analysis that derived them from the unit's own output, and they correct two
    // things this was originally specified with:
    //
    //  1. **It is two delay lines, not one.** One per channel, both driven by the
    //     SAME triangle LFO, with the right channel's modulation INVERTED so the two
    //     sweep in opposite directions. That anti-phase pair is the whole reason a
    //     Juno sounds wide, and a single line cannot produce it. (This is still not
    //     an "ensemble" effect - that is the Solina/ARP arrangement of three lines
    //     with phase-offset LFOs. Two lines, one LFO, opposite polarity.)
    //
    //  2. **The delay is not fixed at 5 ms.** It sweeps 1.66 ms to 5.35 ms in modes
    //     I and II, so 5.35 ms is the top of the sweep, not the operating point, and
    //     the centre is about 3.5 ms.
    //
    // The three modes are not one behaviour at three rates:
    //
    //  | Mode | Rate    | Delay sweep      | Stereo | Character                  |
    //  |------|---------|------------------|--------|----------------------------|
    //  | I    | 0.513 Hz| 1.66 - 5.35 ms   | yes    | mild chorus                |
    //  | II   | 0.863 Hz| 1.66 - 5.35 ms   | yes    | deeper, richer chorus      |
    //  | I+II | 9.75 Hz | 3.30 - 3.70 ms   | NO     | Leslie-like, fast and thin |
    //
    // I+II is the odd one out in every column: roughly eleven times the rate, a
    // ninth of the depth, and MONO - the two lines move together instead of apart,
    // so it does not widen at all. Treating it as "I and II at once" is the natural
    // guess and it is wrong; on the hardware it is a narrow fast wobble, which is
    // why it is the one people describe as a rotary speaker.
    //
    // Rate and Depth are TRIMS on top of the mode, not replacements for it: both
    // default to exactly 1.0x, which is the hardware value above. Turned up they
    // push past the hardware, which is useful and is not emulation, so the default
    // is where the emulation lives.
    class JunoChorusFx : public FxModule
    {
    public:
        enum Param
        {
            pMode = 0,   // choice: I / II / I+II
            pRate,       // trim on the mode's rate, 1.0x at centre
            pDepth,      // trim on the mode's depth, 1.0x at centre
            pTone,       // the BBD's low-pass corner
            pMix,
            kNumParams
        };

        enum Mode
        {
            modeI = 0,
            modeII,
            modeBoth,
            kNumModes
        };

        // One mode's measured behaviour.
        struct ModeSpec
        {
            const char* name;
            float rateHz;      // LFO rate
            float centreMs;    // delay at the middle of the sweep
            float depthMs;     // half the sweep width
            bool mono;         // true = both channels move together, no widening
        };

        static const ModeSpec& specFor (int mode) noexcept;

        static const FxDescriptor& descriptor();

        // Declared rather than left implicit, and not for style: JUCE_DECLARE_NON_COPYABLE
        // below declares a deleted copy constructor, and a user-declared constructor
        // suppresses the implicit default one. Without this line the class cannot be
        // constructed at all - which is why every other module in the rack declares
        // one too.
        JunoChorusFx() = default;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        // The LFO value for a phase in 0..1, as a triangle from -1 to +1.
        //
        // A triangle rather than a sine, and this is the single most important
        // detail in the whole effect. A sine gives a generic chorus; the straight
        // edges of a triangle are what the ear reads as "that" Juno shimmer. Static
        // and public so the smoke test can check its shape directly.
        static float triangle (float phase01) noexcept;

    private:
        // A delay line with a fractional read, so the LFO can sweep smoothly
        // between samples.
        struct DelayLine
        {
            std::vector<float> buffer;
            int writeIndex = 0;

            void resize (int lengthSamples);
            void clear();

            // Reads `delaySamples` back from the write head, linearly interpolated.
            float read (float delaySamples) const noexcept;
        };

        double sampleRate = 44100.0;
        int channels = 2;

        DelayLine lines[2];

        // The LFO runs once for the whole effect, not once per channel - the two
        // channels share a phase and differ only in polarity. That is what makes
        // them anti-phase rather than merely detuned.
        float lfoPhase = 0.0f;

        // The delay time is slewed rather than set, so changing mode does not step
        // the read position. A step in a delay line is a discontinuity in the
        // output, which is a click - and mode changes are exactly when it would
        // happen.
        //
        // BOTH the centre and the depth are slewed, because a mode change moves
        // both: I to I+II drops the depth from 1.845 ms to 0.2 ms, which is a jump
        // of over 70 samples at the top of the sweep. Slewing only the centre would
        // leave that jump in place.
        //
        // The slew is on the mode's values, NOT on the LFO's contribution - filtering
        // the sweep itself would attenuate mode I+II's 9.75 Hz, which is the whole
        // character of that mode.
        float currentCentreMs = 0.0f;
        float currentDepthMs = 0.0f;
        bool delayPrimed = false;

        // The BBD's low-pass, two cascaded one-poles (12 dB/oct), per channel.
        float lp1[2] { 0.0f, 0.0f };
        float lp2[2] { 0.0f, 0.0f };

        // How fast the delay time follows a mode change. Long enough to glide,
        // short enough that the switch still feels immediate.
        static constexpr float kDelaySlewSeconds = 0.05f;

        // The longest delay the lines are sized for, with room above the 5.35 ms
        // sweep for the depth trim to be turned up.
        static constexpr float kMaxDelaySeconds = 0.030f;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JunoChorusFx)
    };
}
