#include "EqFx.h"

#include <cmath>
#include <complex>

namespace mixxynth
{
    namespace
    {
        // An octave apart, 31 Hz to 16 kHz. The standard graphic-EQ centres: each is the
        // geometric mean of its neighbours, so the bands are evenly spaced in the way the
        // ear hears them rather than in the way a linear axis draws them.
        constexpr float kBandHz[EqFx::kNumBands] =
        {
            31.0f, 62.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
        };

        // The band names, as C strings because FxParamInfo::name is a const char* that has to
        // outlive the descriptor - a juce::String built in the lambda would be freed the
        // moment the descriptor was constructed.
        constexpr const char* kBandNames[EqFx::kNumBands] =
        {
            "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
        };

        // Wide enough that neighbouring bands meet without a dip between them, narrow enough
        // that moving one does not drag the whole spectrum with it.
        constexpr float kBandQ = 1.4f;

        // +/- 15 dB, which is more than anyone should need from a graphic EQ and about what
        // one is expected to offer.
        constexpr float kMaxGainDb = 15.0f;

        // The RBJ peaking-EQ coefficients, shared between the audio path and the curve
        // display so the two cannot disagree.
        void peakingCoefficients (float hz, float gainDb, float q, double sampleRate,
                                  float& b0, float& b1, float& b2, float& a1, float& a2)
        {
            const float a = std::pow (10.0f, gainDb / 40.0f);
            const float w0 = 2.0f * juce::MathConstants<float>::pi
                               * juce::jlimit (20.0f, static_cast<float> (sampleRate) * 0.45f, hz)
                               / static_cast<float> (sampleRate);
            const float cosw = std::cos (w0);
            const float alpha = std::sin (w0) / (2.0f * juce::jmax (0.1f, q));

            const float a0 = 1.0f + alpha / a;

            b0 = (1.0f + alpha * a) / a0;
            b1 = (-2.0f * cosw) / a0;
            b2 = (1.0f - alpha * a) / a0;
            a1 = (-2.0f * cosw) / a0;
            a2 = (1.0f - alpha / a) / a0;
        }
    }

    float EqFx::bandFrequency (int band) noexcept
    {
        return kBandHz[static_cast<size_t> (juce::jlimit (0, kNumBands - 1, band))];
    }

    float EqFx::bandGainDb (float normalised) noexcept
    {
        // -15 to +15, so the centre of the control is flat and a band that has not been
        // touched does nothing.
        return (juce::jlimit (0.0f, 1.0f, normalised) * 2.0f - 1.0f) * kMaxGainDb;
    }

    float EqFx::responseDb (const float* params, float hz, double sampleRate)
    {
        if (params == nullptr)
            return 0.0f;

        float totalGain = 1.0f;

        for (int b = 0; b < kNumBands; ++b)
        {
            const float gainDb = bandGainDb (params[pFirstBand + b]);

            // A band at flat contributes nothing, and skipping it keeps the maths exact at
            // unity rather than leaving a rounding residue across ten filters.
            if (std::abs (gainDb) < 0.001f)
                continue;

            float b0, b1, b2, a1, a2;
            peakingCoefficients (bandFrequency (b), gainDb, kBandQ, sampleRate, b0, b1, b2, a1, a2);

            // |H(e^jw)| for a direct-form-1 biquad, evaluated at the frequency asked for.
            const float w = 2.0f * juce::MathConstants<float>::pi * hz
                              / static_cast<float> (sampleRate);

            const std::complex<float> z = std::polar (1.0f, -w);

            const std::complex<float> numerator = b0 + b1 * z + b2 * z * z;
            const std::complex<float> denominator = 1.0f + a1 * z + a2 * z * z;

            totalGain *= std::abs (numerator / denominator);
        }

        return 20.0f * std::log10 (juce::jmax (1.0e-6f, totalGain));
    }

    const FxDescriptor& EqFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Graphic EQ";
            desc.numParams = kNumParams;

            // The band names are the frequencies, because on a graphic EQ that is what the
            // control IS. "Band 3" would be a name for a number nobody can picture.
            for (int b = 0; b < kNumBands; ++b)
            {
                desc.params[pFirstBand + b].name = kBandNames[b];

                // Centred: a graphic EQ that arrives with a curve already drawn is a preset,
                // not an equaliser.
                desc.params[pFirstBand + b].defaultValue = 0.5f;
            }

            desc.params[pOutput].name = "Output";
            desc.params[pOutput].defaultValue = 0.5f;

            // The card draws the response curve from the band gains. This is the hook that
            // makes a generic card able to show something other than knobs.
            desc.drawGraph = &EqFx::responseDb;
            desc.graphLabel = "dB";

            return desc;
        }();

        return d;
    }

    void EqFx::updateCoefficients (int band, float gainDb, double sr)
    {
        const auto b = static_cast<size_t> (band);

        float b0, b1, b2, a1, a2;
        peakingCoefficients (bandFrequency (band), gainDb, kBandQ, sr, b0, b1, b2, a1, a2);

        bands[b].b0 = b0;
        bands[b].b1 = b1;
        bands[b].b2 = b2;
        bands[b].a1 = a1;
        bands[b].a2 = a2;

        currentGainDb[b] = gainDb;
    }

    void EqFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        // Built flat, so a fresh instance is transparent before process() ever runs.
        for (int b = 0; b < kNumBands; ++b)
            updateCoefficients (b, 0.0f, sampleRate);

        reset();
        prepared = true;
    }

    void EqFx::reset()
    {
        for (auto& band : bands)
            band.clear();
    }

    void EqFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // Coefficients are rebuilt only for bands that moved. Ten biquads would otherwise be
        // ten tans and ten sins per sample, for a control nobody turns a thousand times a
        // second.
        for (int b = 0; b < kNumBands; ++b)
        {
            const float gainDb = bandGainDb (params[pFirstBand + b]);

            if (std::abs (gainDb - currentGainDb[static_cast<size_t> (b)]) > 0.01f)
                updateCoefficients (b, gainDb, sampleRate);
        }

        const float outputGain = std::pow (10.0f, bandGainDb (params[pOutput]) / 20.0f * 0.5f);

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                float x = data[i];

                for (int b = 0; b < kNumBands; ++b)
                    x = bands[static_cast<size_t> (b)].process (x, ch);

                data[i] = x * outputGain;
            }
        }
    }
}
