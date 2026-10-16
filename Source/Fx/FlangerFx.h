#pragma once

#include <array>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // Flanger: a short modulated delay with feedback.
    //
    //   in -> [+] -> [delay, 0.5-8 ms, modulated] -> out
    //          ^                        |
    //          |      feedback (signed) |
    //          +------------------------+
    //
    // The delay is what separates this from a chorus. A chorus sits around 5-30 ms and
    // is heard as detuning; a flanger sweeps 0.5-8 ms, where the delay is short enough
    // that the comb notches land inside the musical range and you hear them move as a
    // jet. Feedback is what sharpens the notches and turns a wobble into a sweep.
    //
    // Three things decide whether it sounds like a flanger or like a broken chorus:
    //
    //  * A short delay and a WIDE depth, so the notches travel far enough to be heard
    //    as movement rather than as a fixed colour.
    //  * SIGNED feedback. Positive gives the classic hollow sweep, negative the thinner
    //    metallic one. Both are useful and the polarity is a control, not a hidden
    //    constant - it is a different effect, not a variation.
    //  * A stereo phase offset on the LFO, so the two sides sweep apart instead of
    //    moving together. At zero the effect is mono.
    class FlangerFx : public FxModule
    {
    public:
        enum Param
        {
            pRate = 0,
            pDepth,
            pFeedback,
            pPolarity,   // choice: Positive / Negative
            pDelay,      // the centre delay
            pStereo,     // LFO phase offset between the channels
            pMix,
            kNumParams
        };

        enum Polarity
        {
            polarityPositive = 0,
            polarityNegative
        };

        static const FxDescriptor& descriptor();

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares a
        // deleted copy constructor, and a user-declared constructor suppresses the
        // implicit default one.
        FlangerFx() = default;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        // The delay range. 0.5 ms is the shortest that still sounds like a comb rather
        // than a filter, 8 ms the longest before it starts reading as a chorus.
        static constexpr float kMinDelayMs = 0.5f;
        static constexpr float kMaxDelayMs = 8.0f;

    private:
        struct DelayLine
        {
            std::vector<float> buffer;
            int writeIndex = 0;

            void resize (int lengthSamples);
            void clear();
            float read (float delaySamples) const noexcept;
        };

        double sampleRate = 44100.0;
        int channels = 2;

        DelayLine lines[2];

        float lfoPhase = 0.0f;

        // The delay time actually in use, slewed so changing Delay glides rather than
        // jumps. A step in a delay line is a click.
        float currentDelayMs = 0.0f;
        bool delayPrimed = false;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlangerFx)
    };
}
