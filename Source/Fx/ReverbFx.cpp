#include "ReverbFx.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr int kNumModes = 5;

        const char* const kModeNames[kNumModes] = { "Plate", "Room", "Chamber", "Hall", "Spring" };

        // Tank delay lengths in milliseconds at Size 1.0, per mode. Spread
        // logarithmically rather than evenly, so the modes of the network are
        // distributed across the spectrum instead of clumping.
        //
        // The SUM of these is what sets modal density, and Schroeder's rule of
        // thumb is 0.15 modes per Hz for every second of decay time. Hall sums to
        // about 780 ms, which at 48 kHz is roughly 1.5 modes/Hz - comfortable for
        // tails of ten seconds or more.
        constexpr float kModeLengthsMs[kNumModes][8] = {
            { 23.1f, 28.7f, 34.9f, 41.3f, 48.7f, 56.9f, 65.3f, 74.9f },   // plate
            { 18.3f, 24.1f, 30.7f, 38.3f, 46.1f, 54.7f, 63.1f, 72.7f },   // room
            { 31.7f, 39.1f, 47.3f, 56.9f, 67.1f, 78.7f, 91.3f, 105.1f },  // chamber
            { 45.1f, 57.7f, 71.3f, 86.3f, 102.1f, 119.3f, 138.1f, 158.3f }, // hall
            { 13.7f, 17.3f, 21.1f, 25.7f, 30.3f, 35.9f, 41.7f, 48.1f }    // spring
        };

        // How much early reflection energy each mode has. A plate has essentially
        // none - it is all late field - while a room or hall is defined by its
        // early reflections as much as its tail. A spring is somewhere between.
        constexpr float kModeErAmount[kNumModes] = { 0.25f, 1.00f, 0.80f, 1.00f, 0.15f };

        // Input diffusion: six allpass stages, spread logarithmically. Short ones
        // first to break up the impulse, longer ones to fill the gaps between the
        // tank's echoes. This is the single biggest factor in how "dense" the very
        // start of the tail sounds.
        constexpr float kDiffuserMs[6] = { 4.3f, 6.7f, 9.7f, 13.1f, 17.9f, 23.3f };

        // Early reflection times and amplitudes. Alternating polarity keeps the
        // sum from building a strong comb at any one frequency.
        constexpr float kErTimesMs[8] = { 7.3f, 11.7f, 17.3f, 23.9f, 31.1f, 39.7f, 49.3f, 60.7f };
        constexpr float kErGains[8] = { 1.00f, -0.85f, 0.72f, -0.61f, 0.52f, -0.44f, 0.37f, -0.31f };

        // Band crossovers for the frequency-dependent decay filter. Mid is the
        // band the Decay knob sets; Bass and Damping scale the bands either side.
        //
        // The high crossover sits at 3 kHz, up in the region where air absorption
        // actually lives, rather than the more obvious 1-2 kHz.
        constexpr float kLowCrossoverHz = 250.0f;
        constexpr float kHighCrossoverHz = 3000.0f;

        constexpr double kMaxPreDelaySeconds = 0.5;
        constexpr double kMaxModSeconds = 0.004;

        // Modulation depth as a fraction of the delay length. See the comment where
        // these are applied: absolute depths wreck the tail.
        constexpr float kTankModFraction = 0.012f;     // up to 1.2% of a tank line
        constexpr float kDiffuserModFraction = 0.020f; // up to 2% of a diffuser

        // Shimmer window. Longer is smoother but smears the pitch.
        constexpr double kShimmerWindowSeconds = 0.055;
        constexpr float kShimmerRatio = 2.0f; // one octave up

        constexpr float kMixSplitAt = 0.75f; // 3 o'clock, as on the pedal
        constexpr float kLineCeiling = 2.0f;

        // Output trim for the tank. See the comment where it is applied.
        constexpr float kOutputTrim = 0.55f;

        // Soft ceiling in the loop. With feedback at exactly 1.0 the loop is
        // energy preserving, so a sustained input would otherwise accumulate
        // without bound; a nonlinearity dissipates energy, which makes the
        // infinite setting self-limiting rather than distorted.
        //
        // It is deliberately TRANSPARENT below unity. An earlier version used
        // x/(1+|x|/2), which looks harmless but is already 5% down at 0.2 and 17%
        // down at 0.4 - and because that loss happens once per loop pass it
        // quietly capped the decay time no matter what Decay was set to. A guard
        // that only engages when the tank is actually running away costs nothing.
        inline float softCeil (float x) noexcept
        {
            const float ax = std::abs (x);

            if (ax <= 1.0f)
                return x;

            const float over = ax - 1.0f;

            return (x < 0.0f ? -1.0f : 1.0f) * (1.0f + over / (1.0f + over));
        }

        // Input and output gain vectors for the tank. Deliberately NON-UNIFORM.
        //
        // With b = c = ones/sqrt(N) every mode of the network is excited equally,
        // and the peaks of the resulting transfer function are what you hear as
        // metallic ringing - coloration. Mixed magnitudes and signs spread the
        // excitation so far fewer modes dominate. This is the cheapest large
        // improvement available to an FDN, and the reason a naive one sounds
        // cheap even when its delay lengths are good.
        constexpr float kRawInputGains[8] = { 1.00f, 0.71f, -0.93f, 0.44f, -0.62f, 0.87f, -0.35f, 0.79f };
        constexpr float kRawOutputGains[8] = { 0.83f, -0.55f, 0.97f, -0.71f, 0.41f, -0.88f, 0.66f, -0.29f };

        // A second, independent pair for the right channel. Different excitation
        // is what makes the two output channels decorrelated; identical vectors
        // give a mono reverb no matter how long the tail is.
        constexpr float kRawInputGainsR[8] = { -0.66f, 0.94f, 0.38f, -0.81f, 0.72f, -0.44f, 0.91f, -0.53f };
        constexpr float kRawOutputGainsR[8] = { 0.52f, 0.79f, -0.63f, 0.95f, -0.37f, 0.71f, -0.92f, 0.48f };

        // Nearest prime to n, searching outwards. Used to quantise the tank
        // lengths: primes are trivially mutually prime, which is what keeps the
        // impulse response from repeating, and re-quantising after a Size change
        // preserves that property at every size.
        int nearestPrime (int n)
        {
            auto isPrime = [] (int v)
            {
                if (v < 2)
                    return false;

                if (v % 2 == 0)
                    return v == 2;

                for (int d = 3; d * d <= v; d += 2)
                    if (v % d == 0)
                        return false;

                return true;
            };

            if (n < 2)
                return 2;

            int up = n;
            int down = n;

            while (true)
            {
                if (isPrime (up))
                    return up;

                if (down >= 2 && isPrime (down))
                    return down;

                ++up;
                --down;
            }
        }
    }

    const std::array<float, 8>& ReverbFx::baseLengthsMs (int mode) noexcept
    {
        static const std::array<std::array<float, numLines>, kNumModes> table = []
        {
            std::array<std::array<float, numLines>, kNumModes> t {};

            for (int m = 0; m < kNumModes; ++m)
                for (int i = 0; i < numLines; ++i)
                    t[static_cast<size_t> (m)][static_cast<size_t> (i)] = kModeLengthsMs[m][i];

            return t;
        }();

        return table[static_cast<size_t> (juce::jlimit (0, kNumModes - 1, mode))];
    }

    const FxDescriptor& ReverbFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Reverb";
            desc.numParams = kNumParams;

            desc.params[pMode].name = "Mode";
            desc.params[pMode].isChoice = true;
            desc.params[pMode].choices = { "Plate", "Room", "Chamber", "Hall", "Spring" };

            desc.params[pSize].name = "Size";
            desc.params[pSize].defaultValue = 0.50f;

            desc.params[pDecay].name = "Decay";
            desc.params[pDecay].defaultValue = 0.55f;

            desc.params[pBass].name = "Bass";
            desc.params[pBass].defaultValue = 0.50f;

            desc.params[pDamping].name = "Damping";
            desc.params[pDamping].defaultValue = 0.35f;

            desc.params[pPreDelay].name = "Pre-Delay";

            desc.params[pAttack].name = "Attack";
            desc.params[pAttack].defaultValue = 0.35f;

            desc.params[pDiffusion].name = "Diffusion";
            desc.params[pDiffusion].defaultValue = 0.70f;

            desc.params[pModDepth].name = "Mod";
            desc.params[pModDepth].defaultValue = 0.35f;

            desc.params[pModRate].name = "Rate";
            desc.params[pModRate].defaultValue = 0.35f;

            desc.params[pLowCut].name = "Low Cut";
            desc.params[pLowCut].defaultValue = 0.15f;

            desc.params[pHighCut].name = "High Cut";
            desc.params[pHighCut].defaultValue = 0.80f;

            desc.params[pWidth].name = "Width";
            desc.params[pWidth].defaultValue = 0.50f;

            desc.params[pDuck].name = "Duck";

            desc.params[pShimmer].name = "Shimmer";

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.35f;

            desc.params[pFreeze].name = "Freeze";
            desc.params[pFreeze].isChoice = true;
            desc.params[pFreeze].choices = { "Off", "On" };

            return desc;
        }();

        return d;
    }

    void ReverbFx::prepare (double newSampleRate, int, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (2, juce::jmin (numChannels, 2));

        const double srScale = sampleRate / 48000.0;

        // Modulation moves the read pointer, so every line needs headroom beyond
        // its nominal length.
        modMargin = static_cast<int> (std::ceil (kMaxModSeconds * sampleRate)) + 8;

        // Size tops out at 2.2x, so allocate for the longest line at that size.
        constexpr float maxSizeScale = 2.2f;

        longestLine = 0;

        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[i];

            float longestMs = 0.0f;

            for (int m = 0; m < kNumModes; ++m)
                longestMs = juce::jmax (longestMs, kModeLengthsMs[m][i]);

            const int longest = static_cast<int> (std::lround (longestMs * maxSizeScale * srScale));

            line.capacity = longest + modMargin;
            longestLine = juce::jmax (longestLine, line.capacity);
            line.buffer.assign (static_cast<size_t> (line.capacity) * static_cast<size_t> (channels), 0.0f);
            line.length = juce::jmax (8, static_cast<int> (std::lround (kModeLengthsMs[0][i] * srScale)));
            line.writeIndex = 0;

            // Per-line gains from the fixed non-uniform vectors, normalised so the
            // total energy matches a uniform vector and the level is predictable.
            auto normalised = [i] (const float* raw)
            {
                double sumSq = 0.0;

                for (int k = 0; k < numLines; ++k)
                    sumSq += static_cast<double> (raw[k]) * raw[k];

                const double rms = std::sqrt (sumSq / numLines);

                return static_cast<float> (raw[i] / (rms * std::sqrt (static_cast<double> (numLines))));
            };

            line.inGain[0] = normalised (kRawInputGains);
            line.inGain[1] = normalised (kRawInputGainsR);

            // The output vectors are normalised the same way, then trimmed. An FDN
            // with a long decay genuinely is loud - the tank accumulates energy -
            // so a flat unit-energy output vector leaves the wet signal several
            // times hotter than the dry one. The trim is the one place that is
            // corrected, and it is deliberately a constant rather than something
            // that tracks Decay, so the Decay knob stays honest about tail length.
            line.outGain[0] = normalised (kRawOutputGains) * kOutputTrim;
            line.outGain[1] = normalised (kRawOutputGainsR) * kOutputTrim;
        }

        for (int d = 0; d < numDiffusers; ++d)
        {
            auto& ap = diffusers[d];

            const int nominal = nearestPrime (static_cast<int> (std::lround (kDiffuserMs[d] * srScale)));
            ap.capacity = nominal + modMargin;
            ap.buffer.assign (static_cast<size_t> (ap.capacity) * static_cast<size_t> (channels), 0.0f);
            ap.length = nominal;
            ap.writeIndex = 0;
            ap.gain = 0.5f;
        }

        preLength = static_cast<int> (std::ceil (kMaxPreDelaySeconds * sampleRate)) + 2;
        preBuffer.assign (static_cast<size_t> (preLength) * static_cast<size_t> (channels), 0.0f);
        preWrite = 0;

        // Early reflections read from one shared line long enough for the latest
        // tap at maximum size.
        erLength = static_cast<int> (std::ceil ((kErTimesMs[numErTaps - 1] * maxSizeScale / 1000.0 + 0.02)
                                                  * sampleRate)) + 2;
        erBuffer.assign (static_cast<size_t> (erLength) * static_cast<size_t> (channels), 0.0f);
        erWrite = 0;

        shimmer.window = juce::jmax (64, static_cast<int> (std::lround (kShimmerWindowSeconds * sampleRate)));
        shimmer.capacity = shimmer.window * 2 + 4;
        shimmer.buffer.assign (static_cast<size_t> (shimmer.capacity) * static_cast<size_t> (channels), 0.0f);
        shimmer.ratio = kShimmerRatio;

        lastMode = -1;
        lastSize = -1.0f;
        prepared = true;

        reset();
    }

    void ReverbFx::reset()
    {
        for (auto& line : lines)
        {
            std::fill (line.buffer.begin(), line.buffer.end(), 0.0f);
            line.writeIndex = 0;
            line.modPhase = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
            {
                line.lowState[ch] = 0.0f;
                line.highState[ch] = 0.0f;
            }
        }

        for (auto& ap : diffusers)
        {
            std::fill (ap.buffer.begin(), ap.buffer.end(), 0.0f);
            ap.writeIndex = 0;
            ap.modPhase = 0.0f;
        }

        for (auto& tap : erTaps)
            for (int ch = 0; ch < 2; ++ch)
                tap.dampState[ch] = 0.0f;

        std::fill (preBuffer.begin(), preBuffer.end(), 0.0f);
        preWrite = 0;

        std::fill (erBuffer.begin(), erBuffer.end(), 0.0f);
        erWrite = 0;

        shimmer.reset();

        for (int ch = 0; ch < 2; ++ch)
        {
            lowCutState[ch] = 0.0f;
            highCutState[ch] = 0.0f;
            shimmerReturn[ch] = 0.0f;
        }

        duckEnvelope = 0.0f;
    }

    void ReverbFx::PitchShifter::reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        phase = 0.0f;
    }

    float ReverbFx::PitchShifter::read (int channel, float delaySamples) const
    {
        float pos = static_cast<float> (writeIndex) - delaySamples;

        while (pos < 0.0f)
            pos += static_cast<float> (capacity);

        while (pos >= static_cast<float> (capacity))
            pos -= static_cast<float> (capacity);

        const int i0 = static_cast<int> (pos);
        const float frac = pos - static_cast<float> (i0);
        const int i1 = (i0 + 1 >= capacity) ? 0 : i0 + 1;

        const float* b = buffer.data() + static_cast<size_t> (channel) * static_cast<size_t> (capacity);

        return b[i0] + frac * (b[i1] - b[i0]);
    }

    float ReverbFx::readLine (const Line& line, int channel, float delaySamples) const noexcept
    {
        // Wrap on CAPACITY, not on the nominal length.
        //
        // The line is a circular buffer longer than the delay it nominally
        // provides, so the modulation excursion has room either side. Wrapping on
        // `length` instead would fold a delay of (length + k) round to k, which
        // reads the signal being written right now rather than the tail - a
        // subtle way to turn a reverb into a comb filter.
        const float cap = static_cast<float> (line.capacity);
        const float delay = juce::jlimit (1.0f, cap - 2.0f, delaySamples);

        float pos = static_cast<float> (line.writeIndex) - delay;

        while (pos < 0.0f)
            pos += cap;

        while (pos >= cap)
            pos -= cap;

        const int i0 = static_cast<int> (pos);
        const float frac = pos - static_cast<float> (i0);
        const int i1 = (i0 + 1 >= line.capacity) ? 0 : i0 + 1;

        const float* b = line.buffer.data()
                       + static_cast<size_t> (channel) * static_cast<size_t> (line.capacity);

        return b[i0] + frac * (b[i1] - b[i0]);
    }

    void ReverbFx::updateTankLengths (int mode, float sizeScale)
    {
        const auto& base = baseLengthsMs (mode);
        const double srScale = sampleRate / 48000.0;

        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[i];

            const float ms = base[static_cast<size_t> (i)] * sizeScale;
            const int wanted = static_cast<int> (std::lround (ms * srScale));

            // Re-quantise to a prime after scaling, so the lengths stay mutually
            // prime at every size and the impulse response never repeats early.
            const int p = nearestPrime (juce::jmax (16, wanted));

            // Clamp to what prepare() allocated, modulation excursion included.
            // Capacity is the wrap period now, so `length` is purely the nominal
            // delay and may sit anywhere inside it.
            line.length = juce::jlimit (16, line.capacity - modMargin - 2, p);
        }

        // Early reflection taps scale with the room too, so a bigger space has a
        // proportionally longer early pattern rather than a mismatched one.
        const float erAmount = kModeErAmount[juce::jlimit (0, kNumModes - 1, mode)];

        // Normalise the tap amplitudes so the Attack knob means the same thing in
        // every mode: without this, a mode with more taps simply sounds louder
        // rather than more reflective.
        float erEnergy = 0.0f;

        for (float g : kErGains)
            erEnergy += g * g;

        const float erNorm = 1.0f / std::sqrt (juce::jmax (1.0e-6f, erEnergy));

        for (int i = 0; i < numErTaps; ++i)
        {
            const float ms = kErTimesMs[i] * sizeScale;
            const int samples = static_cast<int> (std::lround (ms * srScale));

            erTaps[static_cast<size_t> (i)].delaySamples = juce::jlimit (1, erLength - 2, samples);
            erTaps[static_cast<size_t> (i)].gain = kErGains[i] * erAmount * erNorm;
        }

        lastMode = mode;
        lastSize = sizeScale;
    }

    void ReverbFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        const float modeF = juce::jlimit (0.0f, 1.0f, params[pMode]);
        const int mode = juce::jlimit (0, kNumModes - 1,
                                       static_cast<int> (std::lround (modeF * static_cast<float> (kNumModes - 1))));

        const float size01 = juce::jlimit (0.0f, 1.0f, params[pSize]);
        const float decay01 = juce::jlimit (0.0f, 1.0f, params[pDecay]);
        const float bass01 = juce::jlimit (0.0f, 1.0f, params[pBass]);
        const float damp01 = juce::jlimit (0.0f, 1.0f, params[pDamping]);
        const float preDelay01 = juce::jlimit (0.0f, 1.0f, params[pPreDelay]);
        const float attack01 = juce::jlimit (0.0f, 1.0f, params[pAttack]);
        const float diffusion01 = juce::jlimit (0.0f, 1.0f, params[pDiffusion]);
        const float mod01 = juce::jlimit (0.0f, 1.0f, params[pModDepth]);
        const float rate01 = juce::jlimit (0.0f, 1.0f, params[pModRate]);
        const float lowCut01 = juce::jlimit (0.0f, 1.0f, params[pLowCut]);
        const float highCut01 = juce::jlimit (0.0f, 1.0f, params[pHighCut]);
        const float width01 = juce::jlimit (0.0f, 1.0f, params[pWidth]);
        const float duck01 = juce::jlimit (0.0f, 1.0f, params[pDuck]);
        const float shimmer01 = juce::jlimit (0.0f, 1.0f, params[pShimmer]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);
        const bool freeze = params[pFreeze] > 0.5f;

        // ---- tank geometry ---------------------------------------------------
        // Size maps to a 0.4x .. 2.2x multiplier, neutral at the halfway point so
        // the knob's default is the nominal room.
        const float sizeScale = 0.4f * std::pow (5.5f, size01);

        if (mode != lastMode || std::abs (sizeScale - lastSize) > 0.005f)
            updateTankLengths (mode, sizeScale);

        // ---- frequency-dependent decay ---------------------------------------
        // Decay sets the mid band; Bass and Damping scale the bands either side.
        // Independent low and high decay times are what make a tail sound like a
        // space rather than a filter: air absorbs highs, low modes hang on.
        const bool infinite = decay01 >= 0.995f;

        const float t60Mid = 0.25f * std::pow (360.0f, decay01);
        const float bassRatio = 0.5f * std::pow (4.0f, bass01);   // 0.5 .. 2.0
        const float dampRatio = std::pow (0.10f, damp01);         // 1.0 .. 0.10

        const float t60Low = juce::jmax (0.05f, t60Mid * bassRatio);
        const float t60High = juce::jmax (0.02f, t60Mid * dampRatio);

        // Per-band loop gain: g = 10^(-3L / (fs * T60)), the standard relation
        // between a delay of L samples and a 60 dB decay time.
        auto bandGain = [] (float lengthSamples, float sr, float t60)
        {
            return std::pow (10.0f, -3.0f * lengthSamples / (sr * juce::jmax (0.02f, t60)));
        };

        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[i];

            if (infinite || freeze)
            {
                // Exactly unity with the lossless matrix, so the tail sustains
                // rather than decays, with no gain compensation needed.
                line.gainLow = line.gainMid = line.gainHigh = 1.0f;
            }
            else
            {
                const float len = static_cast<float> (line.length);
                line.gainLow = bandGain (len, static_cast<float> (sampleRate), t60Low);
                line.gainMid = bandGain (len, static_cast<float> (sampleRate), t60Mid);
                line.gainHigh = bandGain (len, static_cast<float> (sampleRate), t60High);
            }

            // Decorrelated and slow: one shared rate would modulate the whole tank
            // together, and a fast one would sound like a chorus.
            line.modRate = 0.11f + 0.037f * static_cast<float> (i);
            // Modulation depth is a FRACTION OF THE LINE LENGTH, not an absolute
            // number of samples.
            //
            // This matters more than it looks. An absolute depth is ~7% of a short
            // plate line but 0.4% of a long hall line, so one knob position would
            // smear the plate into mush while barely moving the hall. Worse, a
            // time-varying delay is a lossy resampling process: at 7% excursion the
            // modes of the tank get swept far enough to interfere and the tail
            // collapses - the modulation was decaying the reverb faster than the
            // Decay knob could lengthen it. As a fraction, every mode moves by the
            // same proportion, the tail survives, and the knob means one thing.
            line.modDepth = mod01 * kTankModFraction * static_cast<float> (line.length);
        }

        // ---- input diffusion -------------------------------------------------
        // More Diffusion means more allpass feedback, so the input is smeared into
        // a cloud before the tank sees it. The stages are progressively lighter so
        // the chain does not turn into a resonator of its own.
        for (int d = 0; d < numDiffusers; ++d)
        {
            auto& ap = diffusers[d];
            const float t = static_cast<float> (d) / static_cast<float> (numDiffusers - 1);

            ap.gain = 0.42f + 0.34f * diffusion01 - 0.10f * t;
            ap.modRate = 0.07f + 0.029f * static_cast<float> (d);

            // Proportional for the same reason as the tank lines, but a larger
            // fraction: an allpass has no feedback, so its modulation cannot
            // destabilise a resonant mode - it just adds movement to the input.
            ap.modDepth = mod01 * kDiffuserModFraction * static_cast<float> (ap.length);
        }

        processTail (buffer, numCh, numSamples, preDelay01, attack01, rate01,
                     lowCut01, highCut01, width01, duck01, shimmer01, mix01, freeze);
    }

    void ReverbFx::processTail (juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                                float preDelay01, float attack01, float rate01,
                                float lowCut01, float highCut01, float width01, float duck01,
                                float shimmer01, float mix01, bool freeze)
    {
        const float sr = static_cast<float> (sampleRate);
        const float twoPi = juce::MathConstants<float>::twoPi;
        const float modStep = twoPi / sr;

        // ---- coefficients ----------------------------------------------------
        const float preDelaySamples = preDelay01 * static_cast<float> (kMaxPreDelaySeconds) * sr;

        auto onePole = [sr] (float hz)
        {
            const float clamped = juce::jmin (hz, sr * 0.45f);
            return juce::jlimit (0.0f, 0.9999f, 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                                                   * clamped / sr));
        };

        // Crossovers for the three-band attenuation filter.
        const float lowXover = onePole (kLowCrossoverHz);
        const float highXover = onePole (kHighCrossoverHz);

        // Modulation rate is shared, but every line and diffuser has its own phase
        // so the tank never moves as one block.
        const float rateScale = 0.25f + rate01 * 2.75f; // 0.25 .. 3 Hz

        const float erGain = attack01 * 0.85f;

        // Early reflections get progressively darker, as a real room's do: later
        // reflections have travelled further through air and off more surfaces.
        const float erDamp = onePole (4500.0f);

        const float lowCutCoeff = onePole (20.0f * std::pow (50.0f, lowCut01));
        const float highCutCoeff = onePole (1000.0f * std::pow (20.0f, highCut01));

        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        // 0.5 is the nominal stereo image, 0 is mono, 1 is double.
        const float width = width01 * 2.0f;

        const float duckAttack = onePole (200.0f);
        const float duckRelease = onePole (3.0f);

        const float shimmerWindow = static_cast<float> (shimmer.window);
        const float shimmerStep = (shimmer.ratio - 1.0f) / shimmerWindow;

        // Householder reflection: y = x - (2/N) * sum(x). Orthogonal, so lossless -
        // which is exactly what makes the infinite tail possible.
        const float mixScale = 2.0f / static_cast<float> (numLines);

        for (int s = 0; s < numSamples; ++s)
        {
            // ---- modulation --------------------------------------------------
            float lineOffset[numLines];

            for (int i = 0; i < numLines; ++i)
            {
                auto& line = lines[i];
                line.modPhase += line.modRate * rateScale * modStep;

                if (line.modPhase > twoPi)
                    line.modPhase -= twoPi;

                lineOffset[i] = std::sin (line.modPhase) * line.modDepth;
            }

            float diffOffset[numDiffusers];

            for (int d = 0; d < numDiffusers; ++d)
            {
                auto& ap = diffusers[d];
                ap.modPhase += ap.modRate * rateScale * modStep;

                if (ap.modPhase > twoPi)
                    ap.modPhase -= twoPi;

                diffOffset[d] = std::sin (ap.modPhase) * ap.modDepth;
            }

            float wetOut[2] { 0.0f, 0.0f };
            float dryIn[2] { 0.0f, 0.0f };

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                dryIn[ch] = data[s];

                // ---- pre-delay ------------------------------------------------
                float* pre = preBuffer.data() + static_cast<size_t> (ch) * static_cast<size_t> (preLength);
                pre[preWrite] = dryIn[ch];

                int preRead = preWrite - static_cast<int> (preDelaySamples);

                while (preRead < 0)
                    preRead += preLength;

                const float preOut = (preDelaySamples > 0.5f) ? pre[preRead] : dryIn[ch];

                // ---- early reflections ---------------------------------------
                float* er = erBuffer.data() + static_cast<size_t> (ch) * static_cast<size_t> (erLength);
                er[erWrite] = preOut;

                float erSum = 0.0f;

                for (auto& tap : erTaps)
                {
                    int idx = erWrite - tap.delaySamples;

                    while (idx < 0)
                        idx += erLength;

                    tap.dampState[ch] += erDamp * (er[idx] - tap.dampState[ch]);
                    erSum += tap.dampState[ch] * tap.gain;
                }

                // ---- input diffusion -----------------------------------------
                // The shimmer return is injected here, so its octave-up signal gets
                // re-reverbed rather than merely layered on top.
                float diffused = preOut + shimmerReturn[ch] * shimmer01;

                if (freeze)
                    diffused = 0.0f; // freeze holds the tank and ignores the input

                for (int d = 0; d < numDiffusers; ++d)
                {
                    auto& ap = diffusers[d];
                    float* ab = ap.buffer.data()
                              + static_cast<size_t> (ch) * static_cast<size_t> (ap.capacity);

                    // Same wrapping rule as the tank: on capacity, and clamped, so
                    // a modulated tap can never index outside the buffer.
                    const float cap = static_cast<float> (ap.capacity);
                    const float dpos = juce::jlimit (1.0f, cap - 2.0f,
                                                     static_cast<float> (ap.length) - diffOffset[d]);

                    float pos = static_cast<float> (ap.writeIndex) - dpos;

                    while (pos < 0.0f)
                        pos += cap;

                    while (pos >= cap)
                        pos -= cap;

                    const int i0 = static_cast<int> (pos);
                    const int i1 = (i0 + 1 >= ap.capacity) ? 0 : i0 + 1;
                    const float frac = pos - static_cast<float> (i0);
                    const float stored = ab[i0] + frac * (ab[i1] - ab[i0]);

                    const float v = diffused + ap.gain * stored;
                    ab[ap.writeIndex] = v;
                    diffused = stored - ap.gain * v;
                }

                // ---- tank -----------------------------------------------------
                float fb[numLines];
                float sum = 0.0f;

                for (int i = 0; i < numLines; ++i)
                {
                    auto& line = lines[i];
                    const float tap = readLine (line, ch,
                                                static_cast<float> (line.length) - lineOffset[i]);

                    // Three-band attenuation filter: split the tap into low, mid
                    // and high with two one-poles, then scale each band by its own
                    // loop gain. This IS the frequency-dependent decay, and it is
                    // the difference between a room and a filter.
                    line.lowState[ch] += lowXover * (tap - line.lowState[ch]);
                    line.highState[ch] += highXover * (tap - line.highState[ch]);

                    const float lowBand = line.lowState[ch];
                    const float midBand = line.highState[ch] - line.lowState[ch];
                    const float highBand = tap - line.highState[ch];

                    fb[i] = lowBand * line.gainLow
                              + midBand * line.gainMid
                              + highBand * line.gainHigh;
                    sum += fb[i];

                    wetOut[ch] += tap * line.outGain[ch];
                }

                for (int i = 0; i < numLines; ++i)
                {
                    auto& line = lines[i];

                    // Householder, plus this line's own share of the input.
                    const float value = diffused * line.inGain[ch] + fb[i] - sum * mixScale;

                    // Soft ceiling first, then the hard backstop so a corrupted
                    // state can never become a runaway.
                    float* b = line.buffer.data()
                             + static_cast<size_t> (ch) * static_cast<size_t> (line.capacity);
                    b[line.writeIndex] = juce::jlimit (-kLineCeiling, kLineCeiling, softCeil (value));
                }

                // The early reflections are part of the wet signal, not a separate
                // layer: a reverb whose ER and tail do not belong together sounds
                // like two effects rather than one space.
                wetOut[ch] += erSum * erGain;
            }

            // ---- shared indices advance once per sample ----------------------
            // Not once per channel: the per-channel data is strided, not
            // interleaved, so both channels share one write position.
            if (++preWrite >= preLength)
                preWrite = 0;

            if (++erWrite >= erLength)
                erWrite = 0;

            for (auto& ap : diffusers)
                if (++ap.writeIndex >= ap.capacity)
                    ap.writeIndex = 0;

            for (auto& line : lines)
                if (++line.writeIndex >= line.capacity)
                    line.writeIndex = 0;

            // ---- ducking -----------------------------------------------------
            {
                const float level = juce::jmax (std::abs (dryIn[0]), std::abs (dryIn[1]));
                const float coeff = (level > duckEnvelope) ? duckAttack : duckRelease;
                duckEnvelope += coeff * (level - duckEnvelope);
            }

            // Ducking never mutes the wet outright - a reverb that vanishes under
            // playing is a gate, not a duck. 0.85 is the floor.
            const float duckGain = 1.0f
                - duck01 * 0.85f * juce::jlimit (0.0f, 1.0f, duckEnvelope * 1.5f);

            // ---- post: tone --------------------------------------------------
            float wetPost[2] { 0.0f, 0.0f };

            for (int ch = 0; ch < numCh; ++ch)
            {
                float x = wetOut[ch] * duckGain;

                // High cut first, then low cut: the other order lets the highpass
                // undo part of the lowpass's work at the very top.
                highCutState[ch] += highCutCoeff * (x - highCutState[ch]);
                x = highCutState[ch];

                lowCutState[ch] += lowCutCoeff * (x - lowCutState[ch]);
                x -= lowCutState[ch];

                wetPost[ch] = x;
            }

            // ---- width -------------------------------------------------------
            // Mid/side on the wet signal only, so the dry image is never touched.
            if (numCh == 2)
            {
                const float mid = (wetPost[0] + wetPost[1]) * 0.5f;
                const float side = (wetPost[0] - wetPost[1]) * 0.5f * width;

                wetPost[0] = mid + side;
                wetPost[1] = mid - side;
            }

            // ---- shimmer -----------------------------------------------------
            // An octave-up copy of the wet signal, fed back into the tank so it is
            // re-reverbed rather than merely layered on top.
            //
            // Two read heads half a window apart, crossfaded: a single head clicks
            // every time its read pointer wraps.
            {
                const float shimmerInput = (wetPost[0] + wetPost[1]) * 0.5f;

                for (int ch = 0; ch < numCh; ++ch)
                {
                    float* sb = shimmer.buffer.data()
                              + static_cast<size_t> (ch) * static_cast<size_t> (shimmer.capacity);
                    sb[shimmer.writeIndex] = shimmerInput;
                }

                const float gain1 = 0.5f - 0.5f * std::cos (twoPi * shimmer.phase);

                float frac2 = shimmer.phase + 0.5f;

                if (frac2 >= 1.0f)
                    frac2 -= 1.0f;

                const float gain2 = 0.5f - 0.5f * std::cos (twoPi * frac2);

                for (int ch = 0; ch < numCh; ++ch)
                {
                    // Delay decreases as the phase advances, so the read head moves
                    // twice as fast as the write head: one octave up.
                    const float d1 = shimmerWindow * (1.0f - shimmer.phase);
                    const float d2 = shimmerWindow * (1.0f - frac2);

                    shimmerReturn[ch] = shimmer.read (ch, d1) * gain1
                                      + shimmer.read (ch, d2) * gain2;
                }

                shimmer.phase += shimmerStep;

                if (shimmer.phase >= 1.0f)
                    shimmer.phase -= 1.0f;

                if (++shimmer.writeIndex >= shimmer.capacity)
                    shimmer.writeIndex = 0;
            }

            // ---- mix ---------------------------------------------------------
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                data[s] = dry * dryIn[ch] + wet * wetPost[ch];
            }
        }
    }
}
