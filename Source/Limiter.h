#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace mixxynth
{
    // A lookahead brickwall limiter, used as the output safety net.
    //
    // The signal path already had a guard here - a per-sample tanh above 1.0 - and it
    // was not a limiter. A waveshaper distorts the peaks it is meant to be catching, so
    // it changes the sound of exactly the material it exists to protect, and it has no
    // release, so it works one sample at a time and does nothing about the shape of a
    // transient. This replaces it.
    //
    // HOW IT IS A BRICKWALL. The signal is delayed by the lookahead, and the gain is
    // computed from the input as it arrives - which is the sample that will emerge from
    // the delay `lookahead` samples later. So by the time a peak reaches the output, the
    // gain that reduces it was already applied, and nothing above the threshold can ever
    // get through. No prediction and no overshoot, just the right amount of delay.
    //
    // The gain falls instantly and recovers slowly. Instant attack is what makes it
    // brickwall; the release is the only part that is a matter of taste, which is why it
    // is the only control.
    //
    // The threshold is deliberately NOT a control. A safety limiter's threshold is
    // "just under full scale" - the moment it becomes adjustable it stops being a safety
    // net and starts being an effect, and there is already a compressor-shaped slot in
    // the rack for that.
    class Limiter
    {
    public:
        // -0.18 dBFS. Close enough to full scale to be inaudible when it is not working,
        // low enough that a signal at the threshold still converts cleanly to 16-bit and
        // to any lossy format downstream.
        static constexpr float kThreshold = 0.98f;

        // 2 ms. Long enough that the gain has settled before the peak arrives even at the
        // fastest release, short enough that the latency is not worth reporting to the
        // host as plugin delay.
        static constexpr float kLookaheadMs = 2.0f;

        void prepare (double sampleRate, int maxBlockSize, int numChannels);
        void reset();

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares a
        // deleted copy constructor, and a user-declared constructor suppresses the
        // implicit default one. Third time this has bitten in this codebase, so it is
        // worth the comment.
        Limiter() = default;

        // 20 to 500 ms, exponential. Long enough at the bottom to keep a bass note's
        // level steady, short enough at the top to get out of the way between phrases.
        void setReleaseMs (float ms) noexcept;

        // In place. Below the threshold this is a delay and nothing else.
        void process (juce::AudioBuffer<float>& buffer);

        // The threshold. The safety limiter leaves this alone, so it stays at
        // kThreshold; the FX module sets it from its own control.
        void setThreshold (float newThreshold) noexcept
        {
            threshold = juce::jlimit (0.05f, 1.0f, newThreshold);
        }

        // The current gain reduction in dB, as a negative number, for a meter. Written on
        // the audio thread and read by the UI, so it is an atomic and relaxed is enough -
        // a meter one block stale is not a correctness problem.
        float getGainReductionDb() const noexcept { return gainReductionDb; }

    private:
        std::vector<float> delayLines[2];
        int writeIndex = 0;
        int lookaheadSamples = 0;

        float gain = 1.0f;
        float releaseCoeff = 0.0f;

        // The working threshold. kThreshold by default, so the safety limiter is
        // unchanged; the FX module moves it.
        float threshold = kThreshold;

        double sampleRate = 44100.0;
        int channels = 2;

        std::atomic<float> gainReductionDb { 0.0f };

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Limiter)
    };
}
