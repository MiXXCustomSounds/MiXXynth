#pragma once

#include <algorithm>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

// Test signals and reference systems for the FX measurement harness.
//
// Everything here is deliberately closed-form. Each generator has an analytic counterpart - a
// known gain, a known filter, a known delay - so the analysis in Analysis.h can be checked
// against an answer that the analysis did not produce itself. See docs/FX-MEASUREMENT.md.
namespace mixxynth::fxmeasure
{
    // Farina's exponential sine sweep.
    //
    // A sweep rather than an impulse, because of how it separates a system's LINEAR response
    // from its nonlinearity: in an exponential sweep the harmonics arrive before the
    // fundamental's own response, so after deconvolution the linear impulse response sits on
    // its own and the distortion products land elsewhere. An impulse leaves the distortion
    // sitting on top of the thing being measured.
    struct LogSweep
    {
        juce::AudioBuffer<float> signal; // one channel
        double sampleRate = 44100.0;
        double startHz = 20.0;
        double endHz = 20000.0;

        int numSamples() const noexcept { return signal.getNumSamples(); }
    };

    // `seconds` of sweep from startHz to endHz, with a 2 ms fade at each end so the sweep does
    // not begin or end on a discontinuity. Short enough not to bias the top octave, long enough
    // to keep the ends from ringing the analysis.
    LogSweep makeLogSweep (double sampleRate, double seconds, double startHz, double endHz);

    // ---- generators. Every one fills EVERY channel with the same signal. ----

    void fillSilence (juce::AudioBuffer<float>& buffer, int numSamples);
    void fillImpulse (juce::AudioBuffer<float>& buffer, int numSamples, int at, float amplitude);
    void fillSine (juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate,
                   double hz, float amplitude);
    void fillNoise (juce::AudioBuffer<float>& buffer, int numSamples, juce::Random& rng,
                    float amplitude);

    // ---- reference systems, so a self-test has a known answer to compare against ----

    // A first-order low-pass: the filter the self-tests check the sweep method against. Written
    // here rather than inside the test so the test and the harness cannot disagree about what
    // "1 kHz low-pass" means.
    struct OnePole
    {
        double sampleRate = 44100.0;
        double cutoffHz = 1000.0;

        // One state per channel. A single shared state would be wrong for stereo, and a
        // reference filter that quietly mis-filters one channel is worse than having none.
        std::vector<float> z;

        void reset() noexcept { std::fill (z.begin(), z.end(), 0.0f); }
        void process (juce::AudioBuffer<float>& buffer) noexcept;

        // |H(f)| in dB, the closed-form answer the measurement is compared with.
        double magnitudeDb (double hz) const noexcept;
    };

    // A fractional delay line, so a test can impose a delay that is not a whole number of
    // samples - which is what a modulation measurement needs in order to have a smooth
    // trajectory to track. Linear interpolation, which is enough for tracking a delay through
    // cross-correlation and does not colour the result the way it would in an audio path.
    struct FractionalDelay
    {
        void prepare (int maxDelaySamples);
        void reset();
        void write (float sample) noexcept;
        float read (float delaySamples) const noexcept;

    private:
        std::vector<float> buffer;
        int writeIndex = 0;
    };
}
