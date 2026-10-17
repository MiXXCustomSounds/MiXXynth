#include "VastVerbFx.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
    namespace
    {
        // These must match the class's own numLines / numDiffusers. They are repeated
        // rather than referenced because the class members are private and these are
        // file-local helpers that size the constant tables below.
        constexpr int kNumLines = 8;
        constexpr int kNumDiffusers = 6;

        // Tank delay lengths in milliseconds at Size 1.0.
        //
        // Longer than any room, and spread so that no two sit near a simple ratio: a
        // 2:1 pair would put two lines an octave apart in modal terms and the network
        // would ring at that pitch. The spread is roughly geometric, each step about
        // 1.15x the last, which distributes modes logarithmically across the spectrum
        // instead of clumping them at the bottom.
        //
        // The SUM is what sets modal density: 1888 ms here, so roughly 1.9 modes per
        // Hz - Schroeder's rule of thumb for tails up to about 12 seconds. Size
        // scaling raises it from there.
        constexpr float kTankMs[kNumLines] =
        {
            73.7f, 107.3f, 151.1f, 197.9f, 251.3f, 307.9f, 367.1f, 431.9f
        };

        // Input diffusion: six allpass stages, longer than WeirdVerb's and spread
        // logarithmically. Short ones first to break up the impulse, long ones to
        // fill the gaps between the tank's echoes. With tank lines this long the
        // diffusion is doing real work - without it the first second of the tail is
        // individual echoes rather than a wash.
        constexpr float kDiffuserMs[kNumDiffusers] = { 5.9f, 9.7f, 14.9f, 21.1f, 28.9f, 38.3f };

        // Band crossovers. Deliberately not WeirdVerb's 250 Hz / 3 kHz: a lower
        // corner and a higher one give this voice a deeper bottom and more air.
        constexpr float kLowCrossoverHz = 200.0f;
        constexpr float kHighCrossoverHz = 3500.0f;

        constexpr double kMaxPreDelaySeconds = 0.5;

        // Headroom reserved per buffer for the modulation excursion.
        constexpr double kMaxModSeconds = 0.005;

        // Modulation depth: the smaller of a fraction of the line and this ceiling.
        // See the header for why the cap is needed on lines this long.
        constexpr float kModFraction = 0.020f;
        constexpr float kModCapSeconds = 0.004f;

        // Per-line modulation rate multipliers, applied to the Rate knob's value.
        // Deliberately irrational with respect to each other - sqrt(2), the golden
        // ratio and their multiples - so no two lines ever return to phase together.
        // If they did, the whole tank would surge as one block and the movement would
        // read as tremolo rather than as smearing.
        constexpr float kModRateMul[kNumLines] =
        {
            1.000f, 1.190f, 1.414f, 1.618f, 1.932f, 2.236f, 2.645f, 3.077f
        };

        // Diffuser modulation, as a fraction of the diffuser's length. A larger
        // fraction than the tank's is safe here: an allpass has no feedback, so its
        // modulation cannot destabilise a resonant mode - it only moves the input.
        constexpr float kDiffuserModFraction = 0.030f;

        // Master trim on the wet signal, applied once - see where the level normalisation
        // happens. It used to be applied twice, here and again inside that normalisation,
        // which on its own put the module 6 dB low.
        constexpr float kOutputTrim = 0.45f;

        // Hard backstop inside the feedback loop, above the soft ceiling.
        constexpr float kLineCeiling = 2.0f;

        // Excitation and pickup vectors, one pair per channel.
        //
        // Deliberately NON-UNIFORM, and this matters more with a Hadamard matrix than
        // with a Householder one. A Hadamard maps the all-ones vector onto a single
        // basis vector - H * 1 = [N, 0, ... 0] - so a uniform excitation would dump
        // the entire input into line 0 and excite one set of modes. Mixed magnitudes
        // and signs spread the excitation so far fewer modes dominate, which is what
        // stops an FDN ringing metallically.
        //
        // The pickup vectors are the same non-uniform weights with the sign flipped on the
        // second half of the network, and that is not a cosmetic choice - it is the only
        // thing here that makes the two channels decorrelated at all.
        //
        // Write a for the weighted sum of lines 0-3 and b for the same over lines 4-7.
        // The left output is then a + b and the right is a - b, so their correlation is
        // (E[a^2] - E[b^2]) over the product of their norms. The two halves carry equal
        // energy because the Hadamard spreads the input across the whole network, so that
        // difference is zero - for any input, and whatever the excitation vectors happen to
        // be. The weights below are the same four magnitudes in each half, so the
        // cancellation is exact rather than approximate.
        //
        // Two earlier attempts failed here and both are worth recording, because neither
        // was fixable by tuning numbers. Independently chosen pickup vectors measured -0.88:
        // anti-phase, and therefore an 18 dB hole when summed to mono. A deliberately
        // near-orthogonal pair then measured +0.82. A fixed pair of vectors simply does not
        // control the correlation of a state the two channels share - the group split does,
        // structurally.
        constexpr float kRawInputL[kNumLines]  = {  1.00f, -0.71f,  0.83f, -0.94f,  0.62f, -0.88f,  0.53f, -0.76f };
        constexpr float kRawInputR[kNumLines]  = {  0.58f,  0.91f, -0.74f,  0.62f,  0.85f, -0.53f,  0.97f,  0.69f };
        constexpr float kRawOutputL[kNumLines] = {  0.79f, -0.93f,  0.61f, -0.85f,  0.93f, -0.61f,  0.85f, -0.79f };
        constexpr float kRawOutputR[kNumLines] = {  0.79f, -0.93f,  0.61f, -0.85f, -0.93f,  0.61f, -0.85f,  0.79f };

        // Soft ceiling in the loop. With feedback at exactly 1.0 the loop is energy
        // preserving, so a sustained input would otherwise accumulate without bound;
        // a nonlinearity dissipates energy, which makes the infinite setting
        // self-limiting rather than distorted.
        //
        // Deliberately TRANSPARENT below unity. A gentler curve such as x/(1+|x|/2)
        // looks harmless but is already 5% down at 0.2, and because that loss happens
        // once per loop pass it quietly caps the decay time no matter what Decay is
        // set to. A guard that only engages when the tank is actually running away
        // costs nothing.
        inline float softCeil (float x) noexcept
        {
            const float ax = std::abs (x);

            if (ax <= 1.0f)
                return x;

            const float over = ax - 1.0f;

            return (x < 0.0f ? -1.0f : 1.0f) * (1.0f + over / (1.0f + over));
        }

        // In-place fast Walsh-Hadamard transform, the 8-point case.
        //
        // Twelve add/subtract pairs in three butterfly stages and not one multiply.
        // The result is the Hadamard matrix times the input, and every output is a
        // signed sum of ALL eight inputs - the property a Householder reflection does
        // not have. Scaled by 1/sqrt(8) at the call site, which makes it orthonormal
        // and therefore lossless.
        inline void hadamard8 (float* x) noexcept
        {
            for (int step = 1; step < kNumLines; step <<= 1)
            {
                for (int i = 0; i < kNumLines; i += (step << 1))
                {
                    for (int j = 0; j < step; ++j)
                    {
                        const float a = x[i + j];
                        const float b = x[i + j + step];

                        x[i + j] = a + b;
                        x[i + j + step] = a - b;
                    }
                }
            }
        }

        // Nearest prime to n, searching outwards. Used to quantise the tank lengths:
        // primes are trivially mutually prime, which is what keeps the impulse
        // response from repeating, and re-quantising after a Size change preserves
        // that property at every size.
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

    const FxDescriptor& VastVerbFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "VastVerb";
            desc.numParams = kNumParams;

            desc.params[pSize].name = "Size";
            desc.params[pSize].defaultValue = 0.50f;

            desc.params[pDecay].name = "Decay";
            desc.params[pDecay].defaultValue = 0.55f;

            desc.params[pBass].name = "Bass";
            desc.params[pBass].defaultValue = 0.55f;

            desc.params[pDamping].name = "Damping";
            desc.params[pDamping].defaultValue = 0.40f;

            desc.params[pDrift].name = "Drift";
            desc.params[pDrift].defaultValue = 0.45f;

            desc.params[pRate].name = "Rate";
            desc.params[pRate].defaultValue = 0.30f;

            desc.params[pDiffusion].name = "Diffusion";
            desc.params[pDiffusion].defaultValue = 0.70f;

            desc.params[pPreDelay].name = "Pre-Delay";

            desc.params[pSwell].name = "Swell";

            desc.params[pLowCut].name = "Low Cut";
            desc.params[pLowCut].defaultValue = 0.10f;

            desc.params[pHighCut].name = "High Cut";
            desc.params[pHighCut].defaultValue = 0.75f;

            desc.params[pWidth].name = "Width";
            desc.params[pWidth].defaultValue = 0.60f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.35f;

            desc.params[pFreeze].name = "Freeze";
            desc.params[pFreeze].isChoice = true;
            desc.params[pFreeze].choices = { "Off", "On" };

            // The card draws the tank's decay profile: what Bass and Damping actually
            // do, per band, which is the one thing about this module that a knob
            // cannot show.
            desc.drawGraph = &VastVerbFx::decayResponseDb;
            desc.graphLabel = "Decay dB";

            return desc;
        }();

        return d;
    }

    float VastVerbFx::decayResponseDb (const float* params, float hz, double sampleRate)
    {
        if (params == nullptr || sampleRate <= 0.0)
            return 0.0f;

        const float sr = static_cast<float> (sampleRate);

        const float size01 = juce::jlimit (0.0f, 1.0f, params[pSize]);
        const float decay01 = juce::jlimit (0.0f, 1.0f, params[pDecay]);
        const float bass01 = juce::jlimit (0.0f, 1.0f, params[pBass]);
        const float damp01 = juce::jlimit (0.0f, 1.0f, params[pDamping]);

        // The same mappings the audio path uses, so the curve cannot disagree with it.
        const float sizeScale = 0.45f * std::pow (6.0f, size01);

        const float t60Mid = 0.4f * std::pow (150.0f, decay01);
        const float bassRatio = 0.5f * std::pow (4.0f, bass01);
        const float dampRatio = std::pow (0.10f, damp01);

        const float t60Low = juce::jmax (0.05f, t60Mid * bassRatio);
        const float t60High = juce::jmax (0.02f, t60Mid * dampRatio);

        // A representative line: the mean of the tank table at the current Size.
        // Absolute decay time depends on which line you look at, because a long line
        // needs a higher loop gain to reach the same T60, so the graph is drawn for
        // the average line and what it shows is the band SHAPE.
        const float nominalSamples = 236.0f * sizeScale * 0.001f * sr;

        auto bandGain = [nominalSamples, sr] (float t60)
        {
            return std::pow (10.0f, -3.0f * nominalSamples
                                       / (sr * juce::jmax (0.02f, t60)));
        };

        const float gLow = bandGain (t60Low);
        const float gMid = bandGain (t60Mid);
        const float gHigh = bandGain (t60High);

        auto onePoleCoeff = [sr] (float freqHz)
        {
            const float clamped = juce::jmin (freqHz, sr * 0.45f);

            return juce::jlimit (0.0f, 0.9999f,
                                 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                                    * clamped / sr));
        };

        // One-pole magnitude at hz, evaluated exactly as the audio path writes the
        // filter: state += c * (x - state), so H(z) = c / (1 - (1 - c) z^-1).
        auto onePoleMag = [hz, sampleRate] (float coeff)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi
                                 * static_cast<double> (hz) / sampleRate;
            const double a = 1.0 - static_cast<double> (coeff);

            return static_cast<float> (static_cast<double> (coeff)
                                       / std::sqrt (1.0 - 2.0 * a * std::cos (w) + a * a));
        };

        const float magLow = onePoleMag (onePoleCoeff (kLowCrossoverHz));
        const float magHigh = onePoleMag (onePoleCoeff (kHighCrossoverHz));

        // The same three-band split the tank applies: low + (high - low) + (1 - high).
        // The three add to exactly one, so the mid band is the reference.
        const float combined = magLow * gLow
                             + (magHigh - magLow) * gMid
                             + (1.0f - magHigh) * gHigh;

        // Converted back to the decay time that loop gain produces, by inverting the very
        // relation the audio path used to derive it: T60 = -3L / (fs * log10(g)).
        //
        // The gain itself would be a poor thing to draw, and that is not a guess. The
        // first version of this function returned it, and at maximum Bass the curve rose
        // by less than 1 dB - honest, and useless to look at, because a 2x difference in
        // tail length is only 0.96 dB of loop gain. Decay is exponential in gain. In
        // decay time the same control reads as +6 dB, which is what it sounds like.
        auto loopGainToT60 = [nominalSamples, sr] (float g)
        {
            return -3.0f * nominalSamples / (sr * std::log10 (juce::jmax (1.0e-6f, g)));
        };

        return 20.0f * std::log10 (juce::jmax (1.0e-6f, loopGainToT60 (combined))
                                   / juce::jmax (1.0e-6f, loopGainToT60 (gMid)));
    }

    void VastVerbFx::prepare (double newSampleRate, int, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (2, juce::jmin (numChannels, 2));

        const double srScale = sampleRate / 48000.0;

        // Modulation moves the read pointer, so every line needs headroom beyond its
        // nominal length.
        modMargin = static_cast<int> (std::ceil (kMaxModSeconds * sampleRate)) + 8;

        // Size tops out at 2.7x, so allocate for the longest line at that size.
        constexpr float maxSizeScale = 2.7f;

        // Normalises a raw gain vector so its total energy matches a uniform one, which
        // is what makes the trim below mean the same thing whatever the vectors are.
        auto normalised = [] (const float* raw, int i)
        {
            double sumSq = 0.0;

            for (int k = 0; k < kNumLines; ++k)
                sumSq += static_cast<double> (raw[k]) * raw[k];

            const double rms = std::sqrt (sumSq / kNumLines);

            return static_cast<float> (raw[i] / (rms * std::sqrt (static_cast<double> (kNumLines))));
        };

        for (int i = 0; i < kNumLines; ++i)
        {
            auto& line = lines[i];

            const int longest = static_cast<int> (std::lround (kTankMs[i] * maxSizeScale * srScale));

            line.capacity = longest + modMargin;
            line.buffer.assign (static_cast<size_t> (line.capacity) * static_cast<size_t> (channels), 0.0f);
            line.length = juce::jmax (16, static_cast<int> (std::lround (kTankMs[i] * srScale)));
            line.writeIndex = 0;

            line.inGain[0] = normalised (kRawInputL, i);
            line.inGain[1] = normalised (kRawInputR, i);

            // The output vectors are normalised the same way. The decay-dependent level
            // correction happens once, in wetNorm, and NOT here.
            line.outGain[0] = normalised (kRawOutputL, i);
            line.outGain[1] = normalised (kRawOutputR, i);
        }

        for (int d = 0; d < kNumDiffusers; ++d)
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

        lastSize = -1.0f;
        prepared = true;

        reset();
    }

    void VastVerbFx::reset()
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

        std::fill (preBuffer.begin(), preBuffer.end(), 0.0f);
        preWrite = 0;

        for (int ch = 0; ch < 2; ++ch)
        {
            lowCutState[ch] = 0.0f;
            highCutState[ch] = 0.0f;
        }

        swellGain = 0.0f;
    }

    float VastVerbFx::readLine (const Line& line, int channel, float delaySamples) const noexcept
    {
        // Wrap on CAPACITY, not on the nominal length.
        //
        // The line is a circular buffer longer than the delay it nominally provides, so
        // the modulation excursion has room either side. Wrapping on `length` instead
        // would fold a delay of (length + k) round to k, which reads the signal being
        // written right now rather than the tail - a subtle way to turn a reverb into a
        // comb filter.
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

    void VastVerbFx::updateTankLengths (float sizeScale)
    {
        const double srScale = sampleRate / 48000.0;

        for (int i = 0; i < kNumLines; ++i)
        {
            auto& line = lines[i];

            const float ms = kTankMs[i] * sizeScale;
            const int wanted = static_cast<int> (std::lround (ms * srScale));

            // Re-quantise to a prime after scaling, so the lengths stay mutually prime
            // at every size and the impulse response never repeats early.
            const int p = nearestPrime (juce::jmax (16, wanted));

            // Clamp to what prepare() allocated, modulation excursion included.
            // Capacity is the wrap period now, so `length` is purely the nominal delay
            // and may sit anywhere inside it.
            line.length = juce::jlimit (16, line.capacity - modMargin - 2, p);
        }

        lastSize = sizeScale;
    }

    void VastVerbFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        const float size01 = juce::jlimit (0.0f, 1.0f, params[pSize]);
        const float decay01 = juce::jlimit (0.0f, 1.0f, params[pDecay]);
        const float bass01 = juce::jlimit (0.0f, 1.0f, params[pBass]);
        const float damp01 = juce::jlimit (0.0f, 1.0f, params[pDamping]);
        const float drift01 = juce::jlimit (0.0f, 1.0f, params[pDrift]);
        const float rate01 = juce::jlimit (0.0f, 1.0f, params[pRate]);
        const float diffusion01 = juce::jlimit (0.0f, 1.0f, params[pDiffusion]);
        const float preDelay01 = juce::jlimit (0.0f, 1.0f, params[pPreDelay]);
        const float swell01 = juce::jlimit (0.0f, 1.0f, params[pSwell]);
        const float lowCut01 = juce::jlimit (0.0f, 1.0f, params[pLowCut]);
        const float highCut01 = juce::jlimit (0.0f, 1.0f, params[pHighCut]);
        const float width01 = juce::jlimit (0.0f, 1.0f, params[pWidth]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);
        const bool freeze = params[pFreeze] > 0.5f;

        const float sr = static_cast<float> (sampleRate);

        // ---- tank geometry ---------------------------------------------------
        // Size maps to a 0.45x .. 2.7x multiplier, so the nominal tank is a medium
        // hall and the top of the range has no real-world equivalent.
        const float sizeScale = 0.45f * std::pow (6.0f, size01);

        if (std::abs (sizeScale - lastSize) > 0.005f)
            updateTankLengths (sizeScale);

        // ---- frequency-dependent decay ---------------------------------------
        // Decay sets the mid band; Bass and Damping scale the bands either side.
        // Independent low and high decay times are what make a tail sound like a space
        // rather than a filter: air absorbs highs, low modes hang on.
        const bool infinite = decay01 >= 0.995f;

        const float t60Mid = 0.4f * std::pow (150.0f, decay01);
        const float bassRatio = 0.5f * std::pow (4.0f, bass01);
        const float dampRatio = std::pow (0.10f, damp01);

        const float t60Low = juce::jmax (0.05f, t60Mid * bassRatio);
        const float t60High = juce::jmax (0.02f, t60Mid * dampRatio);

        // Per-band loop gain: g = 10^(-3L / (fs * T60)), the standard relation between
        // a delay of L samples and a 60 dB decay time. Computed per line from that
        // line's OWN length, because a long line needs a higher gain than a short one
        // to decay at the same rate.
        auto bandGain = [] (float lengthSamples, float srHz, float t60)
        {
            return std::pow (10.0f, -3.0f * lengthSamples
                                       / (srHz * juce::jmax (0.02f, t60)));
        };

        const float maxModSamples = kModCapSeconds * sr;

        for (int i = 0; i < kNumLines; ++i)
        {
            auto& line = lines[i];

            if (infinite || freeze)
            {
                // Exactly unity with the lossless matrix, so the tail sustains rather
                // than decays, with no gain compensation needed.
                line.gainLow = line.gainMid = line.gainHigh = 1.0f;
            }
            else
            {
                const float L = static_cast<float> (line.length);

                line.gainLow = bandGain (L, sr, t60Low);
                line.gainMid = bandGain (L, sr, t60Mid);
                line.gainHigh = bandGain (L, sr, t60High);
            }

            line.modRate = kModRateMul[i];

            // Depth is the SMALLER of a fraction of the line and the cap. The fraction
            // keeps short lines from over-modulating; the cap stops a second-long line
            // from swinging tens of milliseconds and sounding like a broken tape.
            line.modDepth = drift01 * juce::jmin (static_cast<float> (line.length) * kModFraction,
                                                  maxModSamples);
        }

        // Diffusion gain rises with the knob and falls slightly along the chain, so the
        // later stages do less work than the earlier ones and the chain does not turn
        // into a resonator of its own.
        for (int d = 0; d < kNumDiffusers; ++d)
        {
            auto& ap = diffusers[d];
            const float t = static_cast<float> (d) / static_cast<float> (kNumDiffusers - 1);

            ap.gain = 0.45f + 0.35f * diffusion01 - 0.08f * t;
            ap.modRate = 0.09f + 0.037f * static_cast<float> (d);

            // Proportional for the same reason as the tank lines, but a larger
            // fraction: an allpass has no feedback, so its modulation cannot
            // destabilise a resonant mode - it just adds movement to the input.
            ap.modDepth = drift01 * kDiffuserModFraction * static_cast<float> (ap.length);
        }

        // ---- output level ----------------------------------------------------
        // The tank's steady-state gain is 1/sqrt(1 - g^2), so the wet is divided by it or
        // Decay becomes a volume control. HALF strength - the square root of that divisor -
        // is deliberate: full compensation flattens the level so completely that a long
        // tail stops sounding bigger than a short one, which is the whole point of the
        // control. Half leaves about 3 dB of swing across the range instead of 9.
        //
        // Two things were wrong with the first version, and between them they cost 15 dB.
        // It divided by sqrt(T60), which over-corrects badly because loop gain and decay
        // time are related logarithmically rather than linearly; and it applied the trim a
        // second time, having already applied it in outGain.
        //
        // The guard keeps the divisor away from zero as the loop gain approaches unity,
        // where the soft ceiling rather than the loop gain is what bounds the tank.
        const float nominalSamples = 236.0f * sizeScale * 0.001f * sr;
        const float gNominal = bandGain (nominalSamples, sr, t60Mid);
        const float wetNorm = kOutputTrim
                            / std::sqrt (std::sqrt (juce::jmax (0.02f, 1.0f - gNominal * gNominal)));

        const float twoPi = juce::MathConstants<float>::twoPi;
        const float modStep = twoPi / sr;

        const float preDelaySamples = preDelay01 * static_cast<float> (kMaxPreDelaySeconds) * sr;

        auto onePole = [sr] (float hz)
        {
            const float clamped = juce::jmin (hz, sr * 0.45f);

            return juce::jlimit (0.0f, 0.9999f,
                                 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                                    * clamped / sr));
        };

        // Crossovers for the three-band attenuation filter.
        const float lowXover = onePole (kLowCrossoverHz);
        const float highXover = onePole (kHighCrossoverHz);

        // The Rate knob scales every line's own rate; the per-line multipliers keep
        // them from ever returning to phase together.
        const float rateScale = 0.05f + rate01 * 0.85f; // 0.05 .. 0.9 Hz

        const float lowCutCoeff = onePole (20.0f * std::pow (50.0f, lowCut01));
        const float highCutCoeff = onePole (1000.0f * std::pow (20.0f, highCut01));

        const float wet = mix01;
        const float dry = 1.0f - mix01;

        // 0.5 is the nominal stereo image, 0 is mono, 1 is double.
        const float width = width01 * 2.0f;

        // Swell: the tank's input is ramped up over the swell time on each note, so a
        // sustained note blooms into the reverb instead of hitting it. Applied to the
        // tank's INPUT rather than to the wet output, so the tail is swelled in and
        // then left to decay naturally.
        const float swellTime = 0.01f + swell01 * 2.5f;
        const float swellStep = 1.0f / (swellTime * sr);
        const float swellResetStep = 1.0f / (0.05f * sr);

        // 1/sqrt(8): makes the Hadamard transform orthonormal, and therefore lossless.
        const float hadamardScale = 1.0f / std::sqrt (static_cast<float> (kNumLines));

        for (int s = 0; s < numSamples; ++s)
        {
            // ---- modulation --------------------------------------------------
            float lineOffset[kNumLines];

            for (int i = 0; i < kNumLines; ++i)
            {
                auto& line = lines[i];
                line.modPhase += line.modRate * rateScale * modStep;

                if (line.modPhase > twoPi)
                    line.modPhase -= twoPi;

                lineOffset[i] = std::sin (line.modPhase) * line.modDepth;
            }

            float diffOffset[kNumDiffusers];

            for (int d = 0; d < kNumDiffusers; ++d)
            {
                auto& ap = diffusers[d];
                ap.modPhase += ap.modRate * rateScale * modStep;

                if (ap.modPhase > twoPi)
                    ap.modPhase -= twoPi;

                diffOffset[d] = std::sin (ap.modPhase) * ap.modDepth;
            }

            float wetOut[2] { 0.0f, 0.0f };
            float dryIn[2] { 0.0f, 0.0f };

            // Read from the gain the swell produced on the previous sample; the update
            // itself happens at the end of this one, where the input level is known.
            const float swellApply = (1.0f - swell01) + swell01 * swellGain;

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

                // ---- input diffusion ------------------------------------------
                // The swell is applied here, to what feeds the tank, so the reverb
                // blooms in rather than being turned up.
                float diffused = preOut * swellApply;

                if (freeze)
                    diffused = 0.0f; // freeze holds the tank and ignores the input

                for (int d = 0; d < kNumDiffusers; ++d)
                {
                    auto& ap = diffusers[d];
                    float* ab = ap.buffer.data()
                              + static_cast<size_t> (ch) * static_cast<size_t> (ap.capacity);

                    // Same wrapping rule as the tank: on capacity, and clamped, so a
                    // modulated tap can never index outside the buffer.
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

                    const float delayed = ab[i0] + frac * (ab[i1] - ab[i0]);

                    // Schroeder allpass: the line holds x - g * delayed, and the output
                    // is delayed + g times that.
                    const float stored = diffused - ap.gain * delayed;

                    ab[ap.writeIndex] = stored;
                    diffused = delayed + ap.gain * stored;
                }

                // ---- tank read, damping, mixing -------------------------------
                float fb[kNumLines];

                for (int i = 0; i < kNumLines; ++i)
                {
                    auto& line = lines[i];
                    const float tap = readLine (line, ch,
                                                static_cast<float> (line.length) - lineOffset[i]);

                    // Three-band attenuation filter: split the tap into low, mid and
                    // high with two one-poles, then scale each band by its own loop
                    // gain. This IS the frequency-dependent decay, and it is the
                    // difference between a space and a filter.
                    line.lowState[ch] += lowXover * (tap - line.lowState[ch]);
                    line.highState[ch] += highXover * (tap - line.highState[ch]);

                    const float lowBand = line.lowState[ch];
                    const float midBand = line.highState[ch] - line.lowState[ch];
                    const float highBand = tap - line.highState[ch];

                    fb[i] = lowBand * line.gainLow
                              + midBand * line.gainMid
                              + highBand * line.gainHigh;

                    wetOut[ch] += tap * line.outGain[ch];
                }

                // Hadamard mixing of the damped feedback. Every line's contribution
                // reaches every other line here, which is what fills the network in
                // rather than leaving eight independent combs.
                hadamard8 (fb);

                for (int i = 0; i < kNumLines; ++i)
                {
                    auto& line = lines[i];

                    // The input is injected AFTER the matrix, directly into each line.
                    // Injecting it before would be wrong with a Hadamard: H times the
                    // all-ones vector is a single basis vector, so a common input would
                    // pile into one line instead of spreading.
                    const float value = fb[i] * hadamardScale + diffused * line.inGain[ch];

                    // Soft ceiling first, then the hard backstop, so a corrupted state
                    // can never become a runaway.
                    float* b = line.buffer.data()
                             + static_cast<size_t> (ch) * static_cast<size_t> (line.capacity);

                    b[line.writeIndex] = juce::jlimit (-kLineCeiling, kLineCeiling, softCeil (value));
                }
            }

            // ---- shared indices advance once per sample ----------------------
            // Not once per channel: the per-channel data is strided, not interleaved,
            // so both channels share one write position.
            if (++preWrite >= preLength)
                preWrite = 0;

            for (auto& ap : diffusers)
                if (++ap.writeIndex >= ap.capacity)
                    ap.writeIndex = 0;

            for (auto& line : lines)
                if (++line.writeIndex >= line.capacity)
                    line.writeIndex = 0;

            // ---- swell -------------------------------------------------------
            // A one-shot ramp, driven by whether the input is actually present. Updated
            // after the body so it sees this sample's level; the gain it produces is
            // consumed on the next sample, which on a control that moves over seconds
            // is no lag at all.
            {
                const float level = juce::jmax (std::abs (dryIn[0]), std::abs (dryIn[1]));

                if (level > 1.0e-4f)
                    swellGain += swellStep;
                else
                    swellGain -= swellResetStep;

                swellGain = juce::jlimit (0.0f, 1.0f, swellGain);
            }

            // ---- post: tone --------------------------------------------------
            float wetPost[2] { 0.0f, 0.0f };

            for (int ch = 0; ch < numCh; ++ch)
            {
                float x = wetOut[ch] * wetNorm;

                // High cut first, then low cut: the other order lets the highpass undo
                // part of the lowpass's work at the very top.
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

            // ---- mix ---------------------------------------------------------
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                data[s] = dry * dryIn[ch] + wet * wetPost[ch];
            }
        }
    }
}
