#include "Limiter.h"

#include <cmath>

namespace mixxynth
{
    void Limiter::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        lookaheadSamples = juce::jmax (2, static_cast<int> (kLookaheadMs * 0.001f
                                                             * static_cast<float> (sampleRate)));

        // The line is EXACTLY the lookahead long, and that is the whole trick: reading at
        // the write index before overwriting it returns the sample written one full line
        // ago, so the delay is the line length. Padding the line by a couple of samples -
        // which the first version did - silently makes the lookahead two samples longer
        // than kLookaheadMs says it is.
        for (auto& line : delayLines)
            line.assign (static_cast<size_t> (lookaheadSamples), 0.0f);

        reset();
        prepared = true;
    }

    void Limiter::reset()
    {
        for (auto& line : delayLines)
            std::fill (line.begin(), line.end(), 0.0f);

        writeIndex = 0;
        gain = 1.0f;
        gainReductionDb.store (0.0f, std::memory_order_relaxed);
    }

    void Limiter::setReleaseMs (float ms) noexcept
    {
        const float clamped = juce::jlimit (20.0f, 500.0f, ms);

        releaseCoeff = 1.0f - std::exp (-1.0f / (clamped * 0.001f
                                                   * static_cast<float> (sampleRate)));
    }

    void Limiter::process (juce::AudioBuffer<float>& buffer)
    {
        if (! prepared)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0 || lookaheadSamples <= 0)
            return;

        const int lineLength = static_cast<int> (delayLines[0].size());

        float worstGain = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            // The peak of THIS sample across the channels, which is the sample that will
            // come out of the delay in `lookaheadSamples`. Deciding the gain now is what
            // makes the limit brickwall rather than reactive.
            float peak = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
                peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));

            const float target = peak > threshold ? threshold / peak : 1.0f;

            // Instant down, slow up. An instantaneous release would let a peak through
            // again on the very next sample; an instantaneous attack is what guarantees
            // nothing above the threshold survives.
            if (target < gain)
                gain = target;
            else
                gain += (target - gain) * releaseCoeff;

            worstGain = juce::jmin (worstGain, gain);

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& line = delayLines[static_cast<size_t> (ch)];
                auto* data = buffer.getWritePointer (ch);

                // Read the delayed sample BEFORE overwriting its slot, then write the
                // input in its place.
                const float delayed = line[static_cast<size_t> (writeIndex)];
                line[static_cast<size_t> (writeIndex)] = data[i];

                data[i] = delayed * gain;
            }

            writeIndex = (writeIndex + 1) % lineLength;
        }

        // Published for the meter. In dB and negative, so the UI does not have to decide
        // what "1.0" means.
        const float db = worstGain >= 1.0f
            ? 0.0f
            : 20.0f * std::log10 (juce::jmax (1.0e-6f, worstGain));

        gainReductionDb.store (db, std::memory_order_relaxed);
    }
}
