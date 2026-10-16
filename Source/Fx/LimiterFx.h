#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "../Limiter.h"
#include "FxModule.h"

namespace mixxynth
{
    // A limiter, with an optional soft clipper.
    //
    // The two stages answer different questions and are deliberately separate controls.
    // The limiter is the brickwall: with a lookahead, nothing above the threshold can
    // reach the output, and the cost is that a hard limit on a loud transient is itself a
    // kind of distortion. The soft clipper is the alternative - it rounds peaks instead of
    // stopping them, which sounds warmer and measures worse, and which is why it is an
    // amount rather than a switch. At zero it is transparent; at full it is doing all of
    // the peak control and the limiter is only catching what is left.
    //
    // Reuses the same limiter the output safety net uses, so there is one brickwall
    // implementation in the codebase rather than two that drift apart. The safety net
    // leaves the threshold at its fixed value; this module moves it.
    class LimiterFx : public FxModule
    {
    public:
        enum Param
        {
            pThreshold = 0,
            pRelease,
            pSoftClip,
            pMix,
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // JUCE_DECLARE_NON_COPYABLE below declares a deleted copy constructor, which
        // suppresses the implicit default one.
        LimiterFx() = default;

        // The soft clipper's curve, as the module applies it. Static and public so the
        // smoke test can check that zero is transparent, that the shape stays monotonic,
        // and that it cannot exceed full scale.
        static float softClip (float x, float amount) noexcept;

        // The most recent gain reduction in dB, for a meter.
        float getGainReductionDb() const noexcept override { return limiter.getGainReductionDb(); }

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        mixxynth::Limiter limiter;

        double sampleRate = 44100.0;
        int channels = 2;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterFx)
    };
}
