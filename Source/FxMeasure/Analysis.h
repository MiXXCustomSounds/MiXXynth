#pragma once

#include <functional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "TestSignals.h"

// The measurements. Every one takes either buffers or a `ProcessFn`, never a plugin, so the
// whole file can be checked against signals whose answers are known in closed form before any
// of it is pointed at something whose behaviour is unknown. See docs/FX-MEASUREMENT.md.
namespace mixxynth::fxmeasure
{
    // Processes a buffer in place. Taking this rather than a plugin is what makes the analysis
    // testable: a self-test passes in a known filter, the harness passes in a plugin.
    using ProcessFn = std::function<void (juce::AudioBuffer<float>&)>;

    struct FreqPoint
    {
        double hz = 0.0;
        double db = 0.0;
    };

    // Amplitude of the component at `hz`, from CHANNEL 0 only.
    //
    // Channel 0 rather than a channel sum, and that is not a shortcut: a stereo effect with an
    // anti-phase pair - which is what a Juno chorus is - cancels to silence when its channels
    // are summed, so summing would measure the one case that matters as nothing at all.
    //
    // An integer number of periods is used, so there is no leakage to window away.
    double amplitudeAtHz (const juce::AudioBuffer<float>& buffer, int start, int length,
                          double sampleRate, double hz);

    // ---- impulse response ----

    struct ImpulseResponse
    {
        juce::AudioBuffer<float> samples; // one channel, the full deconvolved length
        int peakIndex = -1;
        double peakValue = 0.0;
        double sampleRate = 44100.0;

        bool isValid() const noexcept { return peakIndex >= 0 && samples.getNumSamples() > 0; }
    };

    // The system's impulse response, by regularised inverse filtering of the sweep.
    //
    // Absolute gain survives: a system with +6 dB of gain deconvolves to an impulse of 2.0, and
    // the self-tests assert exactly that, because a deconvolution that is merely proportional
    // would put a silent constant offset on every measurement ever taken.
    ImpulseResponse deconvolve (const LogSweep& sweep, const juce::AudioBuffer<float>& recorded);

    // The magnitude response, from the sweep's own spectra: |Y(f)| / |X(f)|.
    //
    // Deconvolution done in the frequency domain and left there, rather than reconstructed into
    // an impulse response and windowed back out. The reconstruction is what costs accuracy: the
    // inverse has to be regularised where the sweep has no energy, which leaves a notch outside
    // the band, and the notch's time-domain tail is then truncated by the window. For a flat
    // system that reads about 0.1 dB low - small, but it is a CONSTANT offset on every
    // measurement, which is exactly the kind of error that gets believed.
    //
    // Measuring in the frequency domain has none of that: no window, no reconstruction, and no
    // regularisation bias inside the band. Frequencies where the sweep has too little energy to
    // divide by are reported at the floor rather than as a number, because a ratio taken there
    // would be noise amplified rather than a measurement.
    std::vector<FreqPoint> frequencyResponseFromSweep (const LogSweep& sweep,
                                                       const juce::AudioBuffer<float>& recorded,
                                                       const std::vector<double>& freqs,
                                                       double minLevelDb = -60.0);

    // The same measurement taken a completely different way: a steady-state tone per frequency,
    // waited for, then read at that one frequency. No deconvolution, no FFT, no window - so
    // when the two agree, that is evidence rather than a tautology.
    std::vector<FreqPoint> frequencyResponseByTone (const std::vector<double>& freqs,
                                                    double sampleRate, int channels,
                                                    double settleSeconds, int measurePeriods,
                                                    float amplitude, const ProcessFn& process);

    // Log-spaced frequencies, the natural sampling for an audio response.
    std::vector<double> logFrequencies (double startHz, double endHz, int count);

    // ---- null test ----

    // How much of `dry` survives in `wet`, in dB. Deeply negative means the effect left the
    // signal alone; 0 dB means the effect replaced it.
    double nullDepthDb (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet);

    // ---- transfer curve ----

    struct TransferPoint
    {
        double inDb = 0.0;
        double outDb = 0.0;
    };

    // A sine at each level, measuring the FUNDAMENTAL that comes back.
    //
    // The fundamental rather than the RMS: RMS counts the harmonics a waveshaper adds, which
    // makes a hard clipper and a gentle compressor present the same curve.
    std::vector<TransferPoint> transferCurve (const std::vector<double>& inDbLevels,
                                              double sampleRate, int channels, double hz,
                                              double settleSeconds, int measurePeriods,
                                              const ProcessFn& process);

    // ---- gain reduction ----

    struct GainReductionPoint
    {
        double seconds = 0.0;
        double db = 0.0;
    };

    // Wet/dry envelope ratio in dB, windowed. Reads the processor's real gain including any
    // make-up, because that is what the processor actually does.
    std::vector<GainReductionPoint> gainReduction (const juce::AudioBuffer<float>& dry,
                                                   const juce::AudioBuffer<float>& wet,
                                                   double sampleRate, double windowMs = 5.0);

    // ---- modulation ----

    struct Modulation
    {
        double rateHz = 0.0;

        // Centre and depth are the trajectory's turning points: the centre is halfway between
        // them and the depth is half their span. Taken from the extremes rather than from a mean
        // because a moving delay only correlates well where it is nearly still, so the samples
        // that survive a threshold cluster near those turning points.
        double depthMs = 0.0;
        double centreMs = 0.0;
        bool valid = false;
    };

    // The delay between dry and wet, sampled over time by cross-correlating short windows.
    //
    // A delay TRAJECTORY rather than a spectrum, because that is what a modulated delay
    // actually is. Reading a chorus off its spectrum cannot distinguish two delay lines driven
    // anti-phase - the Juno's whole character - from one line with a deeper sweep.
    //
    // A window whose correlation is too weak to be a measurement reports NaN, and
    // modulationFromTrack skips those. A zero there would be indistinguishable from a real
    // delay of zero and would widen every depth taken over the track.
    std::vector<float> trackDelaySamples (const juce::AudioBuffer<float>& dry,
                                          const juce::AudioBuffer<float>& wet,
                                          int windowSamples, int hopSamples,
                                          int maxDelaySamples);

    // Rate and depth read off a delay trajectory.
    Modulation modulationFromTrack (const std::vector<float>& delaySamples,
                                    double sampleRate, double hopSeconds);
}
