#include "Analysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <juce_dsp/juce_dsp.h>

namespace mixxynth::fxmeasure
{
    namespace
    {
        constexpr double kTwoPi = 6.283185307179586;
        constexpr double kFloorDb = -300.0;

        // Below this correlation coefficient, a window's delay is not a measurement. A window
        // whose wet signal is still silence - the first ones, while a delay line fills - has no
        // correlation with anything.
        constexpr double kMinCorrelation = 0.5;

        // What a window with no usable delay reports. NaN rather than a sentinel value, so that
        // arithmetic which forgets to check it cannot silently treat it as a real delay.
        constexpr float noMeasurement = std::numeric_limits<float>::quiet_NaN();

        double toDb (double linear)
        {
            return linear > 0.0 ? juce::jmax (kFloorDb, 20.0 * std::log10 (linear)) : kFloorDb;
        }

        // A transform size that fits `n` samples of linear convolution.
        int orderFor (int n)
        {
            int order = 1;

            while ((1 << order) < n)
                ++order;

            return order;
        }

        // The two spectra a sweep measurement needs, computed once and shared by the impulse
        // response and by the magnitude response - so the two can never disagree about what the
        // sweep was or how long the transform was.
        struct SweepSpectra
        {
            int size = 0;
            int order = 0;
            std::vector<juce::dsp::Complex<float>> x, y;
            double peakPower = 0.0;
            bool valid = false;
        };

        SweepSpectra computeSpectra (const LogSweep& sweep,
                                     const juce::AudioBuffer<float>& recorded)
        {
            SweepSpectra spectra;

            const int n = sweep.numSamples();
            const int r = recorded.getNumSamples();

            if (n < 8 || r < 8)
                return spectra;

            spectra.order = orderFor (n + r - 1);
            spectra.size = 1 << spectra.order;

            const size_t size = (size_t) spectra.size;

            // Separate input and output buffers rather than transforming in place: JUCE does
            // not promise that perform() tolerates aliasing its arguments.
            std::vector<juce::dsp::Complex<float>> xIn (size, { 0.0f, 0.0f });
            std::vector<juce::dsp::Complex<float>> yIn (size, { 0.0f, 0.0f });

            for (int i = 0; i < n; ++i)
                xIn[(size_t) i] = { sweep.signal.getSample (0, i), 0.0f };

            const auto* rec = recorded.getReadPointer (0);

            for (int i = 0; i < r; ++i)
                yIn[(size_t) i] = { rec[i], 0.0f };

            juce::dsp::FFT fft (spectra.order);

            spectra.x.assign (size, { 0.0f, 0.0f });
            spectra.y.assign (size, { 0.0f, 0.0f });

            fft.perform (xIn.data(), spectra.x.data(), false);
            fft.perform (yIn.data(), spectra.y.data(), false);

            for (size_t i = 0; i < size; ++i)
            {
                const double re = (double) spectra.x[i].real();
                const double im = (double) spectra.x[i].imag();
                spectra.peakPower = juce::jmax (spectra.peakPower, re * re + im * im);
            }

            spectra.valid = spectra.peakPower > 0.0;
            return spectra;
        }
    }

    double amplitudeAtHz (const juce::AudioBuffer<float>& buffer, int start, int length,
                          double sampleRate, double hz)
    {
        if (buffer.getNumChannels() < 1 || hz <= 0.0 || sampleRate <= 0.0)
            return 0.0;

        const int available = juce::jmin (length, buffer.getNumSamples() - start);

        if (available < 8)
            return 0.0;

        // Trim to a whole number of periods. A partial period is leakage, and leakage at a
        // single frequency is an amplitude error rather than something that averages out.
        const double periodSamples = sampleRate / hz;
        const int periods = (int) std::floor ((double) available / periodSamples);

        if (periods < 1)
            return 0.0;

        const int n = (int) std::round ((double) periods * periodSamples);

        if (n < 4 || n > available)
            return 0.0;

        const auto* d = buffer.getReadPointer (0);
        const double w = kTwoPi * hz / sampleRate;

        double re = 0.0, im = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double angle = w * (double) i;
            re += (double) d[start + i] * std::cos (angle);
            im -= (double) d[start + i] * std::sin (angle);
        }

        // A real sinusoid splits its energy between the positive and negative frequencies, so
        // the single-sided amplitude is twice the transform magnitude over n.
        return 2.0 * std::sqrt (re * re + im * im) / (double) n;
    }

    ImpulseResponse deconvolve (const LogSweep& sweep, const juce::AudioBuffer<float>& recorded)
    {
        ImpulseResponse ir;
        ir.sampleRate = sweep.sampleRate;

        const auto spectra = computeSpectra (sweep, recorded);

        if (! spectra.valid)
            return ir;

        juce::dsp::FFT fft (spectra.order);

        using Complex = juce::dsp::Complex<float>;

        const size_t size = (size_t) spectra.size;
        std::vector<Complex> h (size);

        // H = Y * conj(X) / (|X|^2 + eps). The regularisation keeps the inverse finite where the
        // sweep has no energy - outside its own band - at the cost of a biased reading there.
        // That is why the magnitude response is taken in the frequency domain rather than from
        // this reconstruction, and why nothing outside 20 Hz to 20 kHz is reported.
        const double eps = spectra.peakPower * 1.0e-12;

        for (size_t i = 0; i < size; ++i)
        {
            const double xr = (double) spectra.x[i].real();
            const double xi = (double) spectra.x[i].imag();
            const double yr = (double) spectra.y[i].real();
            const double yi = (double) spectra.y[i].imag();
            const double den = xr * xr + xi * xi + eps;

            h[i] = { (float) ((yr * xr + yi * xi) / den), (float) ((yi * xr - yr * xi) / den) };
        }

        // H = Y * conj(X) / (|X|^2 + eps). The regularisation is what keeps the inverse finite
        // where the sweep has no energy - outside its own band - at the cost of a biased
        // reading there, which is why the harness does not report outside 20 Hz to 20 kHz.
        std::vector<Complex> time (size);

        // JUCE scales the inverse transform by 1/size (juce_FFT.cpp), so the result arrives at
        // the right absolute level with no correction needed here.
        fft.perform (h.data(), time.data(), true);

        const int linearLength = sweep.numSamples() + recorded.getNumSamples() - 1;
        const int outLength = juce::jmin (spectra.size, linearLength);
        ir.samples.setSize (1, outLength);
        auto* d = ir.samples.getWritePointer (0);

        for (int i = 0; i < outLength; ++i)
            d[i] = time[(size_t) i].real();

        int peak = 0;
        double best = -1.0;

        for (int i = 0; i < outLength; ++i)
        {
            const double magnitude = std::abs ((double) d[i]);

            if (magnitude > best)
            {
                best = magnitude;
                peak = i;
            }
        }

        ir.peakIndex = peak;
        ir.peakValue = (double) d[peak];
        return ir;
    }

    std::vector<FreqPoint> frequencyResponseFromSweep (const LogSweep& sweep,
                                                       const juce::AudioBuffer<float>& recorded,
                                                       const std::vector<double>& freqs,
                                                       double minLevelDb)
    {
        std::vector<FreqPoint> out;
        out.reserve (freqs.size());

        const auto spectra = computeSpectra (sweep, recorded);

        if (! spectra.valid || spectra.size < 8)
        {
            for (auto hz : freqs)
                out.push_back ({ hz, kFloorDb });

            return out;
        }

        const double binWidth = sweep.sampleRate / (double) spectra.size;
        const double minPower = spectra.peakPower * std::pow (10.0, minLevelDb / 10.0);

        for (auto hz : freqs)
        {
            const int bin = (int) std::llround (hz / binWidth);

            if (bin < 1 || bin > spectra.size / 2)
            {
                out.push_back ({ hz, kFloorDb });
                continue;
            }

            const double xr = (double) spectra.x[(size_t) bin].real();
            const double xi = (double) spectra.x[(size_t) bin].imag();
            const double xPower = xr * xr + xi * xi;

            // Outside the sweep's own band there is nothing to divide by, and the ratio taken
            // there would be amplified noise wearing the shape of a response.
            if (xPower < minPower)
            {
                out.push_back ({ hz, kFloorDb });
                continue;
            }

            const double yr = (double) spectra.y[(size_t) bin].real();
            const double yi = (double) spectra.y[(size_t) bin].imag();

            out.push_back ({ hz, toDb (std::sqrt ((yr * yr + yi * yi) / xPower)) });
        }

        return out;
    }

    std::vector<double> logFrequencies (double startHz, double endHz, int count)
    {
        std::vector<double> out;

        if (count < 2 || startHz <= 0.0 || endHz <= startHz)
            return out;

        const double a = std::log (startHz);
        const double b = std::log (endHz);

        for (int i = 0; i < count; ++i)
            out.push_back (std::exp (a + (b - a) * (double) i / (double) (count - 1)));

        return out;
    }

    std::vector<FreqPoint> frequencyResponseByTone (const std::vector<double>& freqs,
                                                    double sampleRate, int channels,
                                                    double settleSeconds, int measurePeriods,
                                                    float amplitude, const ProcessFn& process)
    {
        std::vector<FreqPoint> out;
        out.reserve (freqs.size());

        for (auto hz : freqs)
        {
            const int measure = (int) std::round ((double) measurePeriods * sampleRate / hz);
            const int settle = (int) std::round (settleSeconds * sampleRate);
            const int total = juce::jmax (measure + settle, 16);

            // The tone runs from index 0, so the effect is already being driven during the
            // settle. Only the last `measure` samples are read.
            juce::AudioBuffer<float> buffer (juce::jmax (1, channels), total);
            fillSine (buffer, total, sampleRate, hz, amplitude);

            process (buffer);

            const double measured = amplitudeAtHz (buffer, settle, measure, sampleRate, hz);
            out.push_back ({ hz, toDb (measured / (double) amplitude) });
        }

        return out;
    }

    double nullDepthDb (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet)
    {
        const int channels = juce::jmin (dry.getNumChannels(), wet.getNumChannels());
        const int n = juce::jmin (dry.getNumSamples(), wet.getNumSamples());

        if (channels < 1 || n < 1)
            return 0.0;

        double residual = 0.0, reference = 0.0;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* a = dry.getReadPointer (ch);
            const auto* b = wet.getReadPointer (ch);

            for (int i = 0; i < n; ++i)
            {
                const double d = (double) a[i];
                const double difference = (double) b[i] - d;
                residual += difference * difference;
                reference += d * d;
            }
        }

        if (reference <= 0.0)
            return kFloorDb;

        return toDb (std::sqrt (residual / reference));
    }

    std::vector<TransferPoint> transferCurve (const std::vector<double>& inDbLevels,
                                              double sampleRate, int channels, double hz,
                                              double settleSeconds, int measurePeriods,
                                              const ProcessFn& process)
    {
        std::vector<TransferPoint> out;
        out.reserve (inDbLevels.size());

        for (auto inDb : inDbLevels)
        {
            const float amplitude = (float) std::pow (10.0, inDb / 20.0);
            const int measure = (int) std::round ((double) measurePeriods * sampleRate / hz);
            const int settle = (int) std::round (settleSeconds * sampleRate);
            const int total = juce::jmax (measure + settle, 16);

            juce::AudioBuffer<float> buffer (juce::jmax (1, channels), total);
            fillSine (buffer, total, sampleRate, hz, amplitude);

            process (buffer);

            // Reported as an absolute level, so a unity-gain linear system reads outDb == inDb
            // and any offset from that is real gain the system applied.
            const double measured = amplitudeAtHz (buffer, settle, measure, sampleRate, hz);
            out.push_back ({ inDb, toDb (measured) });
        }

        return out;
    }

    std::vector<GainReductionPoint> gainReduction (const juce::AudioBuffer<float>& dry,
                                                   const juce::AudioBuffer<float>& wet,
                                                   double sampleRate, double windowMs)
    {
        std::vector<GainReductionPoint> out;

        const int n = juce::jmin (dry.getNumSamples(), wet.getNumSamples());
        const int channels = juce::jmin (dry.getNumChannels(), wet.getNumChannels());
        const int window = juce::jmax (4, (int) std::round (windowMs * 0.001 * sampleRate));

        if (channels < 1 || n < window)
            return out;

        for (int start = 0; start + window <= n; start += window)
        {
            double dryEnergy = 0.0, wetEnergy = 0.0;

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* a = dry.getReadPointer (ch);
                const auto* b = wet.getReadPointer (ch);

                for (int i = 0; i < window; ++i)
                {
                    dryEnergy += (double) a[start + i] * (double) a[start + i];
                    wetEnergy += (double) b[start + i] * (double) b[start + i];
                }
            }

            const double dryRms = std::sqrt (dryEnergy / (double) (window * channels));
            const double wetRms = std::sqrt (wetEnergy / (double) (window * channels));

            out.push_back ({ (double) start / sampleRate, toDb (wetRms) - toDb (dryRms) });
        }

        return out;
    }

    std::vector<float> trackDelaySamples (const juce::AudioBuffer<float>& dry,
                                          const juce::AudioBuffer<float>& wet,
                                          int windowSamples, int hopSamples,
                                          int maxDelaySamples)
    {
        std::vector<float> track;

        const int n = juce::jmin (dry.getNumSamples(), wet.getNumSamples());

        if (windowSamples < 8 || hopSamples < 1 || maxDelaySamples < 1
            || n < windowSamples + maxDelaySamples)
            return track;

        const auto* a = dry.getReadPointer (0);
        const auto* b = wet.getReadPointer (0);

        std::vector<double> corr ((size_t) maxDelaySamples + 1, 0.0);

        for (int start = 0; start + windowSamples + maxDelaySamples <= n; start += hopSamples)
        {
            double dryEnergy = 0.0;

            for (int i = 0; i < windowSamples; ++i)
                dryEnergy += (double) a[start + i] * (double) a[start + i];

            if (dryEnergy <= 0.0)
            {
                track.push_back (noMeasurement);
                continue;
            }

            // The correlation COEFFICIENT at each lag rather than a raw sum, so the peak is
            // comparable between windows and does not move with the signal level.
            int bestLag = 0;
            double bestScore = -2.0;

            for (int lag = 0; lag <= maxDelaySamples; ++lag)
            {
                double sum = 0.0, wetEnergy = 0.0;

                for (int i = 0; i < windowSamples; ++i)
                {
                    const double x = (double) a[start + i];
                    const double y = (double) b[start + i + lag];
                    sum += x * y;
                    wetEnergy += y * y;
                }

                const double denom = std::sqrt (dryEnergy * wetEnergy);
                corr[(size_t) lag] = denom > 0.0 ? sum / denom : 0.0;

                if (corr[(size_t) lag] > bestScore)
                {
                    bestScore = corr[(size_t) lag];
                    bestLag = lag;
                }
            }

            // A window that correlates with nothing is reported as no measurement rather than
            // as a delay of zero. This matters more than it looks: the first windows of a wet
            // signal whose delay line is still filling with silence would otherwise report zero,
            // and a single zero reads as real delay in any min/max taken over the track - which
            // would widen every depth measurement that followed it.
            if (bestScore < kMinCorrelation)
            {
                track.push_back (noMeasurement);
                continue;
            }

            // Sub-sample refinement by fitting a parabola to the peak and its two neighbours.
            // At 44.1 kHz a whole sample is 0.023 ms, which is too coarse to read a 1.5 ms
            // depth to the precision a comparison between two choruses needs.
            double refined = (double) bestLag;

            if (bestLag > 0 && bestLag < maxDelaySamples)
            {
                const double y0 = corr[(size_t) bestLag - 1];
                const double y1 = corr[(size_t) bestLag];
                const double y2 = corr[(size_t) bestLag + 1];
                const double denom = y0 - 2.0 * y1 + y2;

                if (std::abs (denom) > 1.0e-12)
                    refined += 0.5 * (y0 - y2) / denom;
            }

            track.push_back ((float) refined);
        }

        return track;
    }

    Modulation modulationFromTrack (const std::vector<float>& delaySamples,
                                    double sampleRate, double hopSeconds)
    {
        Modulation m;

        if (delaySamples.size() < 8 || sampleRate <= 0.0 || hopSeconds <= 0.0)
            return m;

        // Windows with no usable measurement are skipped rather than counted. A NaN reaching the
        // mean or the range would poison both, and it would do so without any sign that it had.
        double mean = 0.0;
        double lo = 0.0, hi = 0.0;
        int measured = 0;

        for (auto value : delaySamples)
        {
            if (! std::isfinite (value))
                continue;

            const double v = (double) value;

            if (measured == 0)
            {
                lo = v;
                hi = v;
            }

            mean += v;
            lo = juce::jmin (lo, v);
            hi = juce::jmax (hi, v);
            ++measured;
        }

        if (measured < 8)
            return m;

        mean /= (double) measured;

        // The rate comes from the trajectory's own spectrum rather than from counting peaks.
        // A triangle LFO and a sine LFO share a fundamental, and counting zero crossings would
        // read a triangle's corners as extra cycles.
        const double minHz = 0.02;
        const double maxHz = juce::jmin (40.0, 0.5 / hopSeconds);

        double bestHz = 0.0, bestMagnitude = -1.0;

        for (double hz = minHz; hz <= maxHz; hz += 0.005)
        {
            double re = 0.0, im = 0.0;

            for (size_t i = 0; i < delaySamples.size(); ++i)
            {
                // The phase still uses the true index, so skipping a gap does not shift the
                // trajectory in time - which would change the rate it reports.
                if (! std::isfinite (delaySamples[i]))
                    continue;

                const double angle = kTwoPi * hz * (double) i * hopSeconds;
                const double v = (double) delaySamples[i] - mean;
                re += v * std::cos (angle);
                im -= v * std::sin (angle);
            }

            const double magnitude = std::sqrt (re * re + im * im);

            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestHz = hz;
            }
        }

        // The centre and depth come from the EXTREMES, not from the mean.
        //
        // A delay that is moving correlates only weakly in the windows where it moves fastest,
        // because the wet signal is smeared across the window and no single lag lines it up. So
        // the samples that survive a correlation threshold cluster near the turning points, and
        // a mean over them is not the centre - it is the centre plus however the surviving
        // samples happened to fall. The extremes ARE the turning points, and they are also what
        // "centre" and "depth" mean for a symmetric LFO, which is what every chorus, flanger
        // and phaser uses.
        m.rateHz = bestHz;
        m.depthMs = 0.5 * (hi - lo) / sampleRate * 1000.0;
        m.centreMs = 0.5 * (hi + lo) / sampleRate * 1000.0;
        m.valid = bestMagnitude > 0.0;
        return m;
    }
}
