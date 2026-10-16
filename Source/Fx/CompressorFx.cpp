#include "CompressorFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr float kMixSplitAt = 0.5f;

        float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }

        float gainToDb (float gain) noexcept
        {
            return 20.0f * std::log10 (juce::jmax (1.0e-6f, gain));
        }

        // 0..1 to a real range, exponentially. Exponential for anything measured in
        // milliseconds, because that is how the ear reads it: half the travel should be
        // half the time, not half the number.
        float expRange (float value01, float lo, float hi) noexcept
        {
            return lo * std::pow (hi / lo, juce::jlimit (0.0f, 1.0f, value01));
        }
    }

    float CompressorFx::gainReductionDb (float inputDb, float thresholdDb, float ratio,
                                         float kneeDb) noexcept
    {
        const float slope = 1.0f - 1.0f / juce::jmax (1.0f, ratio);
        const float halfKnee = kneeDb * 0.5f;
        const float over = inputDb - thresholdDb;

        // Below the knee: no reduction. Above it: the full slope. In between: a quadratic
        // that joins the two smoothly, so there is no point where the transfer curve's
        // slope jumps. A hard knee is most of the difference between a compressor and a
        // gate with a threshold.
        float reduction = 0.0f;

        if (halfKnee <= 0.0f)
            reduction = juce::jmax (0.0f, over) * slope;
        else if (over <= -halfKnee)
            reduction = 0.0f;
        else if (over >= halfKnee)
            reduction = over * slope;
        else
        {
            const float x = over + halfKnee;
            reduction = slope * x * x / (4.0f * halfKnee);
        }

        return -reduction;
    }

    const FxDescriptor& CompressorFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Compressor";
            desc.numParams = kNumParams;

            desc.params[pDetector].name = "Detector";
            desc.params[pDetector].isChoice = true;
            desc.params[pDetector].choices = { "Peak", "RMS" };

            // -18 dB. A threshold that does something on ordinary material without
            // squashing it; a compressor whose default does nothing is one nobody finds.
            desc.params[pThreshold].name = "Threshold";
            desc.params[pThreshold].defaultValue = 0.70f;

            desc.params[pRatio].name = "Ratio";
            desc.params[pRatio].defaultValue = 0.35f;

            desc.params[pAttack].name = "Attack";
            desc.params[pAttack].defaultValue = 0.30f;

            desc.params[pRelease].name = "Release";
            desc.params[pRelease].defaultValue = 0.40f;

            // Some knee by default: it is the setting that makes a compressor sound like it
            // is doing something musical rather than something mechanical.
            desc.params[pKnee].name = "Knee";
            desc.params[pKnee].defaultValue = 0.30f;

            desc.params[pMakeup].name = "Makeup";
            desc.params[pMakeup].defaultValue = 0.5f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 1.0f;

            desc.hasGainReduction = true;

            return desc;
        }();

        return d;
    }

    void CompressorFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        reset();
        prepared = true;
    }

    void CompressorFx::reset()
    {
        for (auto& v : rmsState)
            v = 0.0f;

        for (auto& v : peakState)
            v = 0.0f;

        gain = 1.0f;
        reportedGrDb.store (0.0f, std::memory_order_relaxed);
    }

    void CompressorFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        const bool rms = params[pDetector] > 0.5f;

        // -60 to 0 dB, so the whole useful range is on the control.
        const float thresholdDb = -60.0f * (1.0f - juce::jlimit (0.0f, 1.0f, params[pThreshold]));
        const float ratio = expRange (params[pRatio], 1.0f, 20.0f);
        const float attackMs = expRange (params[pAttack], 0.1f, 100.0f);
        const float releaseMs = expRange (params[pRelease], 10.0f, 1000.0f);
        const float kneeDb = 24.0f * juce::jlimit (0.0f, 1.0f, params[pKnee]);

        // -12 to +24 dB, unity at a third of the travel.
        const float makeup = dbToGain (-12.0f + 36.0f * juce::jlimit (0.0f, 1.0f, params[pMakeup]));

        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        const float sr = static_cast<float> (sampleRate);

        const float attackCoeff = 1.0f - std::exp (-1.0f / (attackMs * 0.001f * sr));
        const float releaseCoeff = 1.0f - std::exp (-1.0f / (releaseMs * 0.001f * sr));

        // The RMS window. 10 ms is short enough to follow a phrase and long enough not to
        // chase individual cycles, which is the whole difference from peak detection.
        const float rmsCoeff = 1.0f - std::exp (-1.0f / (0.010f * sr));

        float worstGain = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float linked = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
            {
                const float x = buffer.getSample (ch, i);
                const auto c = static_cast<size_t> (ch);

                rmsState[c] += rmsCoeff * (x * x - rmsState[c]);

                // Peak detection with a decay, so it is a level follower rather than an
                // instantaneous magnitude - otherwise the gain chatters at the zero
                // crossings of the waveform.
                peakState[c] = juce::jmax (std::abs (x), peakState[c] * (1.0f - releaseCoeff));

                linked = juce::jmax (linked, rms ? std::sqrt (juce::jmax (0.0f, rmsState[c]))
                                                 : peakState[c]);
            }

            const float targetGain = dbToGain (gainReductionDb (gainToDb (linked), thresholdDb,
                                                                ratio, kneeDb));

            // Attack when the gain must fall, release when it may rise. Smoothing both
            // directions with the same coefficient is the classic mistake: it makes the
            // attack time mean nothing.
            const float coeff = targetGain < gain ? attackCoeff : releaseCoeff;

            gain += (targetGain - gain) * coeff;

            worstGain = juce::jmin (worstGain, gain);

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                const float x = data[i];

                data[i] = x * dry + x * gain * makeup * wet;
            }
        }

        reportedGrDb.store (gainToDb (worstGain), std::memory_order_relaxed);
    }
}
