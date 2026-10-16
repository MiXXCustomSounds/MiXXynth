#pragma once

#include <array>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // Stereo delay with cross-feedback, so it can ping-pong.
    //
    //   inL -> [delay L] --+--> outL
    //            ^         |
    //            |  (cross)|
    //            +---------+---+
    //            |         |   |
    //   inR -> [delay R] --+--> outR
    //
    // The cross amount is a CONTROL rather than a switch, so the range runs from two
    // independent delays (0), through partial bouncing, to strict L/R/L/R ping-pong
    // (1). A switch would have thrown away everything between those two, which is
    // where most of the useful settings are.
    //
    // WHAT MAKES IT NOT A CHEAP DIGITAL DELAY:
    //
    //  * Damping inside the feedback path, so repeats darken as they decay. This is
    //    what makes a delay sit in a mix rather than clutter it.
    //  * Modulation on the delay time, so it is not perfectly static.
    //  * **Drift**, which is the one worth explaining. Each channel's delay time
    //    wanders slowly and independently, so successive echoes arrive very slightly
    //    sharp or flat and the two sides pull apart. That is tape wow and flutter, and
    //    it is the difference between a delay that sounds like a machine and one that
    //    sounds like a machine that is breathing.
    //
    //    The amount is a fraction of the delay time, not a fixed time, so the pitch
    //    shift it produces is the same at any delay length - about 10 cents at the
    //    top of the control. A fixed time offset would drift subtly at 50 ms and
    //    wildly at 1 s, which is the wrong way round.
    class PingPongDelayFx : public FxModule
    {
    public:
        enum Param
        {
            pSync = 0,     // choice: Off, then the musical divisions
            pTimeL,
            pTimeR,
            pFeedback,
            pPingPong,     // 0 = two independent delays, 1 = strict L/R bouncing
            pDamping,
            pMod,
            pDrift,
            pWidth,
            pMix,
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares a
        // deleted copy constructor, and a user-declared constructor suppresses the
        // implicit default one.
        PingPongDelayFx() = default;

        // The delay time for a sync choice, in milliseconds. Exposed so the smoke test
        // can check the divisions rather than trust them.
        float syncTimeMs (int syncIndex) const noexcept;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;
        void setTempo (double newBpm) override;

        // The longest delay the lines are sized for.
        static constexpr float kMaxDelaySeconds = 2.0f;

        // How often the drift walk picks a new target, and how fast it moves there.
        // Slow on purpose: drift that is fast enough to hear as vibrato is a different
        // effect, and this one is meant to be the instability of a tape transport.
        static constexpr float kDriftStepSeconds = 0.35f;
        static constexpr float kDriftSlewSeconds = 0.25f;

        // The pitch shift at full Drift, as a fraction of the delay time. 0.006 is
        // about 10 cents, which is clearly audible as movement without sounding
        // broken.
        static constexpr float kMaxDriftFraction = 0.006f;

    private:
        // A delay line with a fractional read, because the time is always moving.
        struct DelayLine
        {
            std::vector<float> buffer;
            int writeIndex = 0;

            void resize (int lengthSamples);
            void clear();
            float read (float delaySamples) const noexcept;
        };

        // A smoothed random walk in -1..1, one per channel. This is the drift.
        struct DriftWalk
        {
            float value = 0.0f;
            float target = 0.0f;
            float countdown = 0.0f;

            void clear() noexcept { value = 0.0f; target = 0.0f; countdown = 0.0f; }
        };

        float nextRandom() noexcept;

        double sampleRate = 44100.0;
        int channels = 2;
        double bpm = 120.0;

        DelayLine lines[2];
        DriftWalk drift[2];

        // The modulation LFO, shared by both channels but read a quarter cycle apart so
        // the two sides move against each other.
        float lfoPhase = 0.0f;

        // The delay times actually in use, slewed so changing Time or Sync glides
        // rather than jumps. A step in a delay line is a click.
        float currentTimeMs[2] { 0.0f, 0.0f };
        bool timePrimed = false;

        // Damping, one pole per channel, in the feedback path.
        float dampState[2] { 0.0f, 0.0f };

        juce::uint32 rngState = 0x2545f491u;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PingPongDelayFx)
    };
}
