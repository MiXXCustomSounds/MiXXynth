#include "TestSignals.h"

#include <cmath>

namespace mixxynth::fxmeasure
{
    namespace
    {
        constexpr double kTwoPi = 6.283185307179586;

        // A raised-cosine fade at each end. The sweep's instantaneous frequency is exact at
        // every sample, but its amplitude does not start and end at zero, and an edge
        // discontinuity IS an impulse - which would land in the deconvolved result looking
        // exactly as if the system under test had produced it.
        void applyEdgeFade (juce::AudioBuffer<float>& buffer, double sampleRate, double ms)
        {
            const int fade = juce::jmin ((int) std::round (ms * 0.001 * sampleRate),
                                         buffer.getNumSamples() / 2);

            if (fade <= 1)
                return;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                const int n = buffer.getNumSamples();

                for (int i = 0; i < fade; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi
                                                            * (double) i / (double) fade);
                    d[i] *= (float) w;
                    d[n - 1 - i] *= (float) w;
                }
            }
        }
    }

    LogSweep makeLogSweep (double sampleRate, double seconds, double startHz, double endHz)
    {
        LogSweep sweep;
        sweep.sampleRate = sampleRate;
        sweep.startHz = startHz;
        sweep.endHz = endHz;

        const int n = juce::jmax (2, (int) std::round (seconds * sampleRate));
        sweep.signal.setSize (1, n);

        auto* d = sweep.signal.getWritePointer (0);

        const double ratio = std::log (endHz / startHz);

        // The instantaneous frequency is startHz * exp(t * ln(endHz/startHz) / total), so the
        // phase is its integral. Integrated in closed form rather than accumulated per sample,
        // because the analysis INVERTS this signal: any drift in the sweep becomes an error in
        // every response measured with it.
        const double k = kTwoPi * startHz * seconds / ratio;

        for (int i = 0; i < n; ++i)
        {
            const double time = (double) i / sampleRate;
            const double phase = k * (std::exp (time * ratio / seconds) - 1.0);
            d[i] = (float) std::sin (phase);
        }

        applyEdgeFade (sweep.signal, sampleRate, 2.0);
        return sweep;
    }

    void fillSilence (juce::AudioBuffer<float>& buffer, int numSamples)
    {
        buffer.setSize (juce::jmax (1, buffer.getNumChannels()), numSamples, false, true, true);
        buffer.clear();
    }

    void fillImpulse (juce::AudioBuffer<float>& buffer, int numSamples, int at, float amplitude)
    {
        buffer.setSize (juce::jmax (1, buffer.getNumChannels()), numSamples, false, true, true);
        buffer.clear();

        if (at >= 0 && at < numSamples)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, at, amplitude);
    }

    void fillSine (juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate,
                   double hz, float amplitude)
    {
        buffer.setSize (juce::jmax (1, buffer.getNumChannels()), numSamples, false, true, true);
        buffer.clear();

        const double w = kTwoPi * hz / sampleRate;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
                d[i] = amplitude * (float) std::sin (w * (double) i);
        }
    }

    void fillNoise (juce::AudioBuffer<float>& buffer, int numSamples, juce::Random& rng,
                    float amplitude)
    {
        buffer.setSize (juce::jmax (1, buffer.getNumChannels()), numSamples, false, true, true);
        buffer.clear();

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
                d[i] = amplitude * (rng.nextFloat() * 2.0f - 1.0f);
        }
    }

    void OnePole::process (juce::AudioBuffer<float>& buffer) noexcept
    {
        const int channels = buffer.getNumChannels();

        if ((int) z.size() < channels)
            z.resize ((size_t) channels, 0.0f);

        const double a = 1.0 - std::exp (-kTwoPi * cutoffHz / sampleRate);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            float state = z[(size_t) ch];

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                state += (float) (a * ((double) d[i] - (double) state));
                d[i] = state;
            }

            z[(size_t) ch] = state;
        }
    }

    double OnePole::magnitudeDb (double hz) const noexcept
    {
        const double a = 1.0 - std::exp (-kTwoPi * cutoffHz / sampleRate);
        const double w = kTwoPi * hz / sampleRate;
        const double re = 1.0 - (1.0 - a) * std::cos (w);
        const double im = (1.0 - a) * std::sin (w);
        const double den = std::sqrt (re * re + im * im);

        return den > 0.0 ? 20.0 * std::log10 (a / den) : -300.0;
    }

    void FractionalDelay::prepare (int maxDelaySamples)
    {
        buffer.assign ((size_t) juce::jmax (8, maxDelaySamples + 4), 0.0f);
        writeIndex = 0;
    }

    void FractionalDelay::reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    void FractionalDelay::write (float sample) noexcept
    {
        if (buffer.empty())
            return;

        buffer[(size_t) writeIndex] = sample;
        writeIndex = (writeIndex + 1) % (int) buffer.size();
    }

    float FractionalDelay::read (float delaySamples) const noexcept
    {
        if (buffer.size() < 2)
            return 0.0f;

        const int size = (int) buffer.size();
        const float clamped = juce::jlimit (0.0f, (float) (size - 2), delaySamples);

        // writeIndex already points one past the newest sample, so the newest sample sits at
        // writeIndex - 1 and a delay of d samples reads back from d samples before that.
        const float position = (float) (writeIndex - 1) - clamped;
        const int floorIndex = (int) std::floor (position);
        const float frac = position - (float) floorIndex;

        const int i0 = ((floorIndex % size) + size) % size;
        const int i1 = (i0 + 1) % size;

        return buffer[(size_t) i0] * (1.0f - frac) + buffer[(size_t) i1] * frac;
    }
}
