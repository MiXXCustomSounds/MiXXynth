#include "MultibandFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        float gainToDb (float gain) noexcept
        {
            return 20.0f * std::log10 (juce::jmax (1.0e-6f, gain));
        }

        // The two thresholds, fixed rather than exposed.
        //
        // OTT's character comes from the RELATIVE placement of these against the material,
        // not from being able to move them. Exposing them would turn a three-control
        // character box into a six-control engineering tool, which is a different module -
        // and there is already a single-band compressor in the rack for that job.
        constexpr float kDownwardThresholdDb = -12.0f;
        constexpr float kUpwardThresholdDb = -36.0f;

        // How far upward compression may lift, in dB. Uncapped it would raise the noise
        // floor of silence to full scale, which is not a musical result.
        constexpr float kMaxUpwardDb = 30.0f;

        // How far downward compression may pull, in dB. A ratio of 20:1 at the top.
        constexpr float kMaxDownwardSlope = 0.95f;
    }

    float MultibandFx::bandGainDb (float levelDb, float downward, float upward) noexcept
    {
        const float down = juce::jlimit (0.0f, 1.0f, downward);
        const float up = juce::jlimit (0.0f, 1.0f, upward);

        float gain = 0.0f;

        // Downward: what is above the threshold comes down, proportionally to how far above.
        if (levelDb > kDownwardThresholdDb)
            gain -= (levelDb - kDownwardThresholdDb) * down * kMaxDownwardSlope;

        // Upward: what is below the threshold comes up. This is the half that makes it OTT,
        // and it is the opposite direction from the other one - which is the whole point.
        if (levelDb < kUpwardThresholdDb)
            gain += juce::jmin (kMaxUpwardDb, (kUpwardThresholdDb - levelDb) * up);

        return gain;
    }

    const FxDescriptor& MultibandFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Multiband";
            desc.numParams = kNumParams;

            desc.params[pSplitLow].name = "Split Low";
            desc.params[pSplitLow].defaultValue = 0.30f;

            desc.params[pSplitHigh].name = "Split High";
            desc.params[pSplitHigh].defaultValue = 0.55f;

            desc.params[pDownward].name = "Downward";
            desc.params[pDownward].defaultValue = 0.60f;

            desc.params[pUpward].name = "Upward";
            desc.params[pUpward].defaultValue = 0.60f;

            desc.params[pTime].name = "Time";
            desc.params[pTime].defaultValue = 0.40f;

            // Half wet by default. At full depth this is an extreme effect, and a module
            // that arrives already extreme is one people remove rather than dial in.
            desc.params[pDepth].name = "Depth";
            desc.params[pDepth].defaultValue = 0.50f;

            return desc;
        }();

        return d;
    }

    float MultibandFx::getBandGainDb (int band) const noexcept
    {
        return reportedDb[static_cast<size_t> (juce::jlimit (0, kNumBands - 1, band))];
    }

    void MultibandFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        reset();
        prepared = true;
    }

    void MultibandFx::reset()
    {
        for (auto& f : filters)
            f.clear();

        envelope.fill (0.0f);
        previousGain.fill (1.0f);
        reportedDb.fill (0.0f);
    }

    void MultibandFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // 100 Hz to 1 kHz, and 1 kHz to 8 kHz. The lower split is where bass stops being
        // bass; the upper is where a mix's presence lives.
        const float splitLowHz = 100.0f * std::pow (10.0f,
                                        juce::jlimit (0.0f, 1.0f, params[pSplitLow]));
        const float splitHighHz = 1000.0f * std::pow (8.0f,
                                         juce::jlimit (0.0f, 1.0f, params[pSplitHigh]));

        const float downward = juce::jlimit (0.0f, 1.0f, params[pDownward]);
        const float upward = juce::jlimit (0.0f, 1.0f, params[pUpward]);
        const float depth = juce::jlimit (0.0f, 1.0f, params[pDepth]);

        // Time scales every attack and release together. OTT is a fast effect, so the range
        // is centred low: 5 ms to 100 ms of release.
        const float timeScale = 0.1f * std::pow (20.0f, juce::jlimit (0.0f, 1.0f, params[pTime]));
        const float attackMs = 0.5f * timeScale;
        const float releaseMs = 5.0f * timeScale;

        const float sr = static_cast<float> (sampleRate);

        const float lowCoeff = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                  * juce::jlimit (20.0f, sr * 0.45f, splitLowHz) / sr);
        const float highCoeff = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                   * juce::jlimit (20.0f, sr * 0.45f, splitHighHz) / sr);

        const float attackCoeff = 1.0f - std::exp (-1.0f / (attackMs * 0.001f * sr));
        const float releaseCoeff = 1.0f - std::exp (-1.0f / (releaseMs * 0.001f * sr));

        juce::AudioBuffer<float> dry (numCh, numSamples);
        dry.makeCopyOf (buffer, true);

        std::array<float, kNumBands> worstGain { 1.0f, 1.0f, 1.0f };

        // Per sample: split every channel, decide the three gains from the linked level,
        // then apply. Splitting once and applying from the same values is what keeps the
        // bands summing back to the input when the gains are all 1.
        std::array<std::array<float, kNumBands>, 2> split {};

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto c = static_cast<size_t> (ch);
                auto& f = filters[c];
                const float x = buffer.getSample (ch, i);

                const float low = f.lowB.process (f.lowA.process (x, lowCoeff), lowCoeff);
                const float belowHigh = f.highB.process (f.highA.process (x, highCoeff), highCoeff);
                const float aboveLow = x - low;
                const float mid = f.midB.process (f.midA.process (aboveLow, highCoeff), highCoeff);

                split[c][0] = low;
                split[c][1] = mid;
                split[c][2] = x - belowHigh;
            }

            std::array<float, kNumBands> bandGain {};

            for (int b = 0; b < kNumBands; ++b)
            {
                const auto bi = static_cast<size_t> (b);

                // The linked peak: the louder channel decides, so both get the same gain and
                // the stereo image stays where it was.
                float peak = 0.0f;

                for (int ch = 0; ch < numCh; ++ch)
                    peak = juce::jmax (peak, std::abs (split[static_cast<size_t> (ch)][bi]));

                envelope[bi] = juce::jmax (peak, envelope[bi] * (1.0f - releaseCoeff));

                const float targetGain = std::pow (10.0f,
                                             bandGainDb (gainToDb (envelope[bi]), downward, upward)
                                               / 20.0f);

                // Attack when the gain must fall, release when it may rise.
                const float coeff = targetGain < previousGain[bi] ? attackCoeff : releaseCoeff;

                previousGain[bi] += (targetGain - previousGain[bi]) * coeff;
                bandGain[bi] = previousGain[bi];

                worstGain[bi] = juce::jmin (worstGain[bi], bandGain[bi]);
            }

            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto c = static_cast<size_t> (ch);

                const float shaped = split[c][0] * bandGain[0]
                                       + split[c][1] * bandGain[1]
                                       + split[c][2] * bandGain[2];

                const float x = dry.getSample (ch, i);

                buffer.setSample (ch, i, x * (1.0f - depth) + shaped * depth);
            }
        }

        for (int b = 0; b < kNumBands; ++b)
            reportedDb[static_cast<size_t> (b)] = gainToDb (worstGain[static_cast<size_t> (b)]);
    }
}
