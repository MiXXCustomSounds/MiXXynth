#include "LimiterFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr float kMixSplitAt = 0.5f;

        float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }
    }

    float LimiterFx::softClip (float x, float amount) noexcept
    {
        const float a = juce::jlimit (0.0f, 1.0f, amount);

        if (a <= 0.0f)
            return x;

        // The cubic soft clip: unity slope at the origin, so quiet playing is untouched,
        // and a slope that reaches zero at +/-1, so there is no corner where it leaves the
        // linear region. x - x^3/3 reaches only 2/3 at x = 1, hence the 1.5 to bring the
        // peak back to full scale.
        const float c = juce::jlimit (-1.0f, 1.0f, x);
        const float shaped = (c - (c * c * c) / 3.0f) * 1.5f;

        // Blended rather than switched, so the control is continuous and zero really is
        // transparent.
        return x * (1.0f - a) + shaped * a;
    }

    const FxDescriptor& LimiterFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Limiter";
            desc.numParams = kNumParams;

            // -0.5 dB by default: a limiter that is not catching anything is a limiter
            // nobody knows is there, and one set to -12 dB by default would be an effect
            // rather than a limiter.
            desc.params[pThreshold].name = "Threshold";
            desc.params[pThreshold].defaultValue = 0.98f;

            desc.params[pRelease].name = "Release";
            desc.params[pRelease].defaultValue = 0.35f;

            // Off by default. Soft clipping is a character, and a limiter that colours the
            // signal without being asked is a bug rather than a feature.
            desc.params[pSoftClip].name = "Soft Clip";
            desc.params[pSoftClip].defaultValue = 0.0f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 1.0f;

            desc.hasGainReduction = true;

            return desc;
        }();

        return d;
    }

    void LimiterFx::prepare (double newSampleRate, int maxBlockSize, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        limiter.prepare (sampleRate, maxBlockSize, channels);
        limiter.setReleaseMs (100.0f);

        reset();
        prepared = true;
    }

    void LimiterFx::reset()
    {
        limiter.reset();
    }

    void LimiterFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // -24 to 0 dB.
        const float thresholdDb = -24.0f * (1.0f - juce::jlimit (0.0f, 1.0f, params[pThreshold]));
        const float releaseMs = 20.0f * std::pow (25.0f, juce::jlimit (0.0f, 1.0f, params[pRelease]));
        const float softClipAmount = juce::jlimit (0.0f, 1.0f, params[pSoftClip]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        // Keep the dry signal before the limiter, so Mix is a true parallel blend rather
        // than a blend against something already limited.
        juce::AudioBuffer<float> dryBuffer (numCh, numSamples);
        dryBuffer.makeCopyOf (buffer, true);

        limiter.setThreshold (dbToGain (thresholdDb));
        limiter.setReleaseMs (releaseMs);
        limiter.process (buffer);

        // The soft clipper runs after the limiter, not before: it is there to round what
        // the limiter has already caught, and putting it first would just make the limiter
        // work less.
        if (softClipAmount > 0.0f)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);

                for (int i = 0; i < numSamples; ++i)
                    data[i] = softClip (data[i], softClipAmount);
            }
        }

        if (wet < 1.0f)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                const auto* src = dryBuffer.getReadPointer (ch);

                for (int i = 0; i < numSamples; ++i)
                    data[i] = src[i] * dry + data[i] * wet;
            }
        }
    }
}
