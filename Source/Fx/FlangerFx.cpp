#include "FlangerFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr float kMixSplitAt = 0.5f;

        // Delay from the control, exponential so the short end gets half the travel -
        // which is where the ear is most sensitive to a flanger being at the wrong spot.
        float delayMsFor (float value01) noexcept
        {
            return FlangerFx::kMinDelayMs
                     * std::pow (FlangerFx::kMaxDelayMs / FlangerFx::kMinDelayMs,
                                 juce::jlimit (0.0f, 1.0f, value01));
        }
    }

    const FxDescriptor& FlangerFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Flanger";
            desc.numParams = kNumParams;

            desc.params[pRate].name = "Rate";
            desc.params[pRate].defaultValue = 0.25f;

            desc.params[pDepth].name = "Depth";
            desc.params[pDepth].defaultValue = 0.70f;

            desc.params[pFeedback].name = "Feedback";
            desc.params[pFeedback].defaultValue = 0.55f;

            desc.params[pPolarity].name = "Polarity";
            desc.params[pPolarity].isChoice = true;
            desc.params[pPolarity].choices = { "Positive", "Negative" };
            desc.params[pPolarity].defaultValue = 0.0f;

            desc.params[pDelay].name = "Delay";
            desc.params[pDelay].defaultValue = 0.40f;

            // Some offset by default: a flanger that sweeps both channels identically is
            // the one setting that sounds like a fault rather than a choice.
            desc.params[pStereo].name = "Stereo";
            desc.params[pStereo].defaultValue = 0.50f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.50f;

            return desc;
        }();

        return d;
    }

    void FlangerFx::DelayLine::resize (int lengthSamples)
    {
        buffer.assign (static_cast<size_t> (juce::jmax (4, lengthSamples)), 0.0f);
        writeIndex = 0;
    }

    void FlangerFx::DelayLine::clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    float FlangerFx::DelayLine::read (float delaySamples) const noexcept
    {
        const int size = static_cast<int> (buffer.size());

        if (size <= 0)
            return 0.0f;

        const float d = juce::jlimit (1.0f, static_cast<float> (size - 2), delaySamples);
        const float readPos = static_cast<float> (writeIndex) - d;

        int i0 = static_cast<int> (std::floor (readPos));
        const float frac = readPos - static_cast<float> (i0);

        i0 = ((i0 % size) + size) % size;
        const int i1 = (i0 + 1) % size;

        return buffer[static_cast<size_t> (i0)]
                 + frac * (buffer[static_cast<size_t> (i1)] - buffer[static_cast<size_t> (i0)]);
    }

    void FlangerFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        // Twice the longest delay, so the LFO's sweep never runs off the end of the
        // line even with Depth at maximum.
        const int length = static_cast<int> (kMaxDelayMs * 2.0f * 0.001f * sampleRate) + 4;

        for (auto& line : lines)
            line.resize (length);

        reset();
        prepared = true;
    }

    void FlangerFx::reset()
    {
        for (auto& line : lines)
            line.clear();

        lfoPhase = 0.0f;
        currentDelayMs = 0.0f;
        delayPrimed = false;
    }

    void FlangerFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        const float rate01 = juce::jlimit (0.0f, 1.0f, params[pRate]);
        const float depth01 = juce::jlimit (0.0f, 1.0f, params[pDepth]);
        const float feedback01 = juce::jlimit (0.0f, 1.0f, params[pFeedback]);
        const float stereo01 = juce::jlimit (0.0f, 1.0f, params[pStereo]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        const bool negative = params[pPolarity] > 0.5f;

        // 0.02 to 8 Hz, exponential. Slow enough at the bottom to be a sweep you can
        // follow, fast enough at the top to be a wobble.
        const float rateHz = 0.02f * std::pow (400.0f, rate01);
        const float centreMs = delayMsFor (params[pDelay]);

        // The depth is a fraction of the centre delay, so the sweep spans the whole
        // useful range without ever going negative. Wide on purpose: a flanger whose
        // notches barely move is a fixed filter with extra steps.
        const float depthMs = centreMs * depth01 * 0.9f;

        // Signed feedback, and capped below unity: a flanger that can reach 1.0 will
        // run away, and one that can cross zero is a different effect rather than a
        // louder one.
        const float fbGain = (negative ? -1.0f : 1.0f) * feedback01 * 0.95f;

        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        const float lfoInc = rateHz / static_cast<float> (sampleRate);
        const float delaySlew = 1.0f - std::exp (-1.0f / (0.02f * static_cast<float> (sampleRate)));

        if (! delayPrimed)
        {
            currentDelayMs = centreMs;
            delayPrimed = true;
        }

        const float samplesPerMs = static_cast<float> (sampleRate) * 0.001f;

        auto* dataL = buffer.getWritePointer (0);
        auto* dataR = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            // Both channels read the SAME phase, taken before it advances. Computing one,
            // incrementing, then computing the other puts the right channel a sample
            // ahead - which is small, but it means Stereo at zero is not mono, and that
            // is exactly what the control promises.
            const float phaseNow = lfoPhase;

            const float lfo = std::sin (2.0f * juce::MathConstants<float>::pi * phaseNow);

            // The second channel reads the LFO offset by the Stereo control. At zero
            // both sides move together and the effect is mono; at half a cycle they
            // move in opposition, which is the widest it gets.
            const float lfoR = std::sin (2.0f * juce::MathConstants<float>::pi
                                           * (phaseNow + stereo01 * 0.5f));

            lfoPhase += lfoInc;

            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;

            currentDelayMs += (centreMs - currentDelayMs) * delaySlew;

            const float inL = dataL[i];
            const float inR = dataR != nullptr ? dataR[i] : inL;

            const float delayL = (currentDelayMs + depthMs * lfo) * samplesPerMs;
            const float delayR = (currentDelayMs + depthMs * lfoR) * samplesPerMs;

            const float wetL = lines[0].read (delayL);
            const float wetR = lines[1].read (delayR);

            // Feedback goes into the line, not out of it, so the repeats sharpen the
            // notches rather than being heard as a separate echo.
            lines[0].buffer[static_cast<size_t> (lines[0].writeIndex)] = inL + wetL * fbGain;
            lines[0].writeIndex = (lines[0].writeIndex + 1) % static_cast<int> (lines[0].buffer.size());

            lines[1].buffer[static_cast<size_t> (lines[1].writeIndex)] = inR + wetR * fbGain;
            lines[1].writeIndex = (lines[1].writeIndex + 1) % static_cast<int> (lines[1].buffer.size());

            dataL[i] = inL * dry + wetL * wet;

            if (dataR != nullptr)
                dataR[i] = inR * dry + wetR * wet;
        }
    }
}
