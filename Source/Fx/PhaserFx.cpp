#include "PhaserFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr float kMixSplitAt = 0.5f;

        const int kStageCounts[] = { 2, 3, 4, 5, 6, 8, 10, 12 };
        constexpr int kNumStageCounts = static_cast<int> (sizeof (kStageCounts) / sizeof (kStageCounts[0]));

        // A break frequency to the allpass coefficient. `a` is monotonic in frequency,
        // which is why the sweep can be done on `a` directly.
        float coeffForHz (float hz, double sr) noexcept
        {
            const float f = juce::jlimit (20.0f, static_cast<float> (sr) * 0.45f, hz);
            const float t = std::tan (juce::MathConstants<float>::pi * f
                                        / static_cast<float> (sr));

            return (t - 1.0f) / (t + 1.0f);
        }

        // The Centre control's range: 120 Hz to 6 kHz, exponential.
        float centreHzFor (float value01) noexcept
        {
            return 120.0f * std::pow (50.0f, juce::jlimit (0.0f, 1.0f, value01));
        }
    }

    int PhaserFx::getNumStageCounts() noexcept { return kNumStageCounts; }

    int PhaserFx::stageCountFor (int index) noexcept
    {
        return kStageCounts[juce::jlimit (0, kNumStageCounts - 1, index)];
    }

    const FxDescriptor& PhaserFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Phaser";
            desc.numParams = kNumParams;

            desc.params[pStages].name = "Stages";
            desc.params[pStages].isChoice = true;

            for (int i = 0; i < kNumStageCounts; ++i)
                desc.params[pStages].choices.add (juce::String (kStageCounts[i]));

            // Six stages: enough notches to be unmistakably a phaser, few enough to
            // still sound open.
            desc.params[pStages].defaultValue = 4.0f / static_cast<float> (kNumStageCounts - 1);

            desc.params[pRate].name = "Rate";
            desc.params[pRate].defaultValue = 0.30f;

            desc.params[pDepth].name = "Depth";
            desc.params[pDepth].defaultValue = 0.70f;

            desc.params[pFeedback].name = "Feedback";
            desc.params[pFeedback].defaultValue = 0.50f;

            desc.params[pCentre].name = "Centre";
            desc.params[pCentre].defaultValue = 0.50f;

            desc.params[pStereo].name = "Stereo";
            desc.params[pStereo].defaultValue = 0.40f;

            desc.params[pStageOffset].name = "Stage Offset";
            desc.params[pStageOffset].isChoice = true;
            desc.params[pStageOffset].choices = { "Same", "Right + 2" };
            desc.params[pStageOffset].defaultValue = 0.0f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.50f;

            return desc;
        }();

        return d;
    }

    void PhaserFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        reset();
        prepared = true;
    }

    void PhaserFx::reset()
    {
        for (auto& chain : stages)
            for (auto& ap : chain)
                ap.clear();

        fbState[0] = fbState[1] = 0.0f;
        lfoPhase = 0.0f;
        currentA = 0.0f;
        aPrimed = false;
    }

    void PhaserFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        const int stageIndex = juce::jlimit (0, kNumStageCounts - 1,
                                 static_cast<int> (std::lround (juce::jlimit (0.0f, 1.0f, params[pStages])
                                     * static_cast<float> (kNumStageCounts - 1))));

        const int stagesL = stageCountFor (stageIndex);

        // The right channel can run a couple more stages than the left. Two allpasses
        // apart is a genuinely different notch pattern rather than a wider version of
        // the same one, which is why it is worth a control of its own.
        const int stagesR = juce::jmin (kMaxStages, stagesL + (params[pStageOffset] > 0.5f ? 2 : 0));

        const float rate01 = juce::jlimit (0.0f, 1.0f, params[pRate]);
        const float depth01 = juce::jlimit (0.0f, 1.0f, params[pDepth]);
        const float feedback01 = juce::jlimit (0.0f, 1.0f, params[pFeedback]);
        const float stereo01 = juce::jlimit (0.0f, 1.0f, params[pStereo]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        // 0.01 to 8 Hz.
        const float rateHz = 0.01f * std::pow (800.0f, rate01);

        const float centreA = coeffForHz (centreHzFor (params[pCentre]), sampleRate);

        // The sweep is a range of `a`, and `a` spans -1..1 across the whole spectrum, so
        // the depth is a fraction of that span rather than of a frequency. 0.45 keeps the
        // sweep inside the region where the allpasses actually do something.
        const float depthA = depth01 * 0.45f;

        // Below unity: feedback around a chain of allpasses is stable well past this, but
        // a phaser that can be pushed to oscillation by accident is a bug.
        const float fbGain = feedback01 * 0.9f;

        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        const float lfoInc = rateHz / static_cast<float> (sampleRate);
        const float aSlew = 1.0f - std::exp (-1.0f / (0.05f * static_cast<float> (sampleRate)));

        if (! aPrimed)
        {
            currentA = centreA;
            aPrimed = true;
        }

        auto* dataL = buffer.getWritePointer (0);
        auto* dataR = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            // Both channels read the SAME phase, taken before it advances. Computing one,
            // incrementing, then computing the other puts the right channel a sample
            // ahead - small, but it means Stereo at zero is not mono, which is exactly
            // what that control promises.
            const float phaseNow = lfoPhase;

            const float lfo = std::sin (2.0f * juce::MathConstants<float>::pi * phaseNow);

            const float lfoR = std::sin (2.0f * juce::MathConstants<float>::pi
                                           * (phaseNow + stereo01 * 0.5f));

            lfoPhase += lfoInc;

            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;

            currentA += (centreA - currentA) * aSlew;

            const float aL = juce::jlimit (-0.99f, 0.99f, currentA + depthA * lfo);
            const float aR = juce::jlimit (-0.99f, 0.99f, currentA + depthA * lfoR);

            const float inL = dataL[i];
            const float inR = dataR != nullptr ? dataR[i] : inL;

            // The feedback wraps the whole chain, which is where the resonance comes
            // from - it is the notches being sharpened, not a separate echo.
            float xL = inL + fbState[0] * fbGain;
            float xR = inR + fbState[1] * fbGain;

            for (int s = 0; s < stagesL; ++s)
                xL = stages[0][static_cast<size_t> (s)].process (xL, aL);

            for (int s = 0; s < stagesR; ++s)
                xR = stages[1][static_cast<size_t> (s)].process (xR, aR);

            fbState[0] = xL;
            fbState[1] = xR;

            dataL[i] = inL * dry + xL * wet;

            if (dataR != nullptr)
                dataR[i] = inR * dry + xR * wet;
        }
    }
}
