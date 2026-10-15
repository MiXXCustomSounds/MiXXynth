#include "ReverbFx.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr int kNumModes = 3;

        // Line lengths in samples at 48 kHz. Co-prime-ish so the modes do not
        // share resonances, and ordered short-to-long within each mode so the
        // modes differ in character rather than just in size.
        //   plate  - dense and bright, the most even set
        //   room   - longer, with more space between taps so the early part is
        //            audible as a room rather than as a smeared plate
        //   spring - shortest and densest, which is what gives the dispersion
        constexpr int kModeLengths[kNumModes][8] = {
            { 1687, 1979, 2237, 2539, 2791, 3067, 3313, 3557 }, // plate
            { 1237, 1381, 1607, 1823, 2029, 2237, 2473, 2687 }, // room
            {  947, 1063, 1181, 1301, 1423, 1543, 1663, 1787 }  // spring
        };

        // Per-mode character. Spring needs the most diffusion to sound like a
        // spring rather than a very short room, and is left the brightest.
        constexpr float kDiffusionGain[kNumModes] = { 0.62f, 0.55f, 0.72f };
        constexpr float kInputGain[kNumModes]     = { 0.50f, 0.45f, 0.55f };
        constexpr float kToneScale[kNumModes]     = { 1.00f, 0.85f, 1.20f };

        // Diffusion stage lengths at 48 kHz: two allpass stages, which smear the
        // input into something the tank can turn into a smooth tail rather than
        // a set of discrete echoes.
        constexpr double kDiffuserSeconds[2] = { 0.0050, 0.0017 };

        // Longest pre-delay, from the pedal's range.
        constexpr double kMaxPreDelaySeconds = 0.25;

        // Modulation excursion at full Mod, in seconds. Deep enough to be
        // obvious, shallow enough not to sound like a chorus.
        constexpr double kMaxModSeconds = 0.0018;

        constexpr float kMixSplitAt = 0.75f; // 3 o'clock, as on the pedal

        // Hard ceiling on what a line may hold. With an infinite tail the loop is
        // energy preserving, so a sustained input can still accumulate; this is
        // the backstop that makes a runaway impossible.
        constexpr float kLineCeiling = 2.0f;

        // Soft ceiling applied inside the loop, below the backstop.
        //
        // With feedback at exactly 1.0 the loop conserves energy, so a sustained
        // input would otherwise accumulate indefinitely - the tail would never
        // decay, but it would also never stop growing while you hold a note. A
        // soft clip bounds that smoothly, and because a nonlinearity dissipates
        // energy it also makes the infinite setting self-limiting: loud passages
        // settle instead of piling up, while a quiet tail still sustains. The
        // hard clamp above it is then only ever a backstop.
        inline float softCeil (float x) noexcept
        {
            return x / (1.0f + std::abs (x) * 0.5f);
        }
    }

    const std::array<int, ReverbFx::numLines>& ReverbFx::baseLengths (int mode) noexcept
    {
        static const std::array<std::array<int, numLines>, kNumModes> table = []
        {
            std::array<std::array<int, numLines>, kNumModes> t {};

            for (int m = 0; m < kNumModes; ++m)
                for (int i = 0; i < numLines; ++i)
                    t[static_cast<size_t> (m)][static_cast<size_t> (i)] = kModeLengths[m][i];

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
            desc.params[pMode].choices = { "Plate", "Room", "Spring" };

            desc.params[pDecay].name = "Decay";
            desc.params[pDecay].defaultValue = 0.55f;

            desc.params[pPreDelay].name = "Pre-Delay";

            desc.params[pTone].name = "Tone";
            desc.params[pTone].defaultValue = 0.6f;

            desc.params[pMod].name = "Mod";
            desc.params[pMod].defaultValue = 0.25f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.35f;

            return desc;
        }();

        return d;
    }

    void ReverbFx::prepare (double newSampleRate, int, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

        // The plugin is stereo, so always prepare two channels even if the host
        // offers fewer. process() clamps to what the buffer actually has.
        channels = juce::jmax (2, juce::jmin (numChannels, 2));

        const double srScale = sampleRate / 48000.0;
        const int modMargin = static_cast<int> (std::ceil (kMaxModSeconds * sampleRate)) + 4;

        longestLine = 0;

        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[i];

            // Size for the longest this line gets in ANY mode, so a mode change
            // only moves `length` and never touches the allocation.
            int longest = 0;

            for (int m = 0; m < kNumModes; ++m)
                longest = juce::jmax (longest,
                                      static_cast<int> (std::lround (kModeLengths[m][i] * srScale)));

            line.capacity = longest + modMargin;
            longestLine = juce::jmax (longestLine, line.capacity);
            line.buffer.assign (static_cast<size_t> (line.capacity) * static_cast<size_t> (channels), 0.0f);
            line.length = juce::jmax (8, static_cast<int> (std::lround (kModeLengths[0][i] * srScale)));
            line.writeIndex = 0;
        }

        preLength = static_cast<int> (std::ceil (kMaxPreDelaySeconds * sampleRate)) + 2;
        preBuffer.assign (static_cast<size_t> (preLength) * static_cast<size_t> (channels), 0.0f);
        preWrite = 0;

        for (int d = 0; d < numDiffusers; ++d)
        {
            auto& ap = diffusers[d];
            ap.length = juce::jmax (8, static_cast<int> (std::lround (kDiffuserSeconds[d] * sampleRate)));
            ap.buffer.assign (static_cast<size_t> (ap.length) * static_cast<size_t> (channels), 0.0f);
            ap.writeIndex = 0;
            ap.gain = kDiffusionGain[0];
        }

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
                line.dampState[ch] = 0.0f;
                line.dcX[ch] = 0.0f;
                line.dcY[ch] = 0.0f;
            }
        }

        std::fill (preBuffer.begin(), preBuffer.end(), 0.0f);
        preWrite = 0;

        for (auto& ap : diffusers)
        {
            std::fill (ap.buffer.begin(), ap.buffer.end(), 0.0f);
            ap.writeIndex = 0;
        }
    }

    float ReverbFx::decaySecondsFor (float decay01) const noexcept
    {
        // 0.25 s at minimum to about a minute at the top. The very top of the
        // travel is treated separately (see process) and becomes a true infinite
        // tail.
        return 0.25f * std::pow (240.0f, juce::jlimit (0.0f, 1.0f, decay01));
    }

    float ReverbFx::toneCoefficient (float tone01) const noexcept
    {
        // Tone is high-frequency DAMPING, as on the pedal: minimum is heavily
        // damped, maximum is open. One-pole coefficient for 1 kHz .. 18 kHz.
        const float fc = 1000.0f * std::pow (18.0f, juce::jlimit (0.0f, 1.0f, tone01));
        const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                          * juce::jmin (fc, static_cast<float> (sampleRate) * 0.45f)
                                          / static_cast<float> (sampleRate));

        return juce::jlimit (0.0f, 0.999f, a);
    }

    float ReverbFx::readLine (const Line& line, int channel, float delaySamples) const noexcept
    {
        // Fractional read, because a modulated line sits between samples.
        float pos = static_cast<float> (line.writeIndex) - delaySamples;

        while (pos < 0.0f)
            pos += static_cast<float> (line.length);

        while (pos >= static_cast<float> (line.length))
            pos -= static_cast<float> (line.length);

        const int i0 = static_cast<int> (pos);
        const float frac = pos - static_cast<float> (i0);
        const int i1 = (i0 + 1 >= line.length) ? 0 : i0 + 1;

        const float* b = line.buffer.data()
                       + static_cast<size_t> (channel) * static_cast<size_t> (line.capacity);

        return b[i0] + frac * (b[i1] - b[i0]);
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
        const float decay01 = juce::jlimit (0.0f, 1.0f, params[pDecay]);
        const float preDelay01 = juce::jlimit (0.0f, 1.0f, params[pPreDelay]);
        const float tone01 = juce::jlimit (0.0f, 1.0f, params[pTone]);
        const float mod01 = juce::jlimit (0.0f, 1.0f, params[pMod]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        // ---- per-block coefficients ------------------------------------------
        const auto& base = baseLengths (mode);
        const double srScale = sampleRate / 48000.0;

        // At the very top of Decay the feedback becomes exactly 1.0. With the
        // orthogonal mixing matrix that conserves energy, so the tail sustains
        // rather than decays - the infinite tail - and needs no gain trickery.
        const bool infinite = decay01 >= 0.995f;
        const float decaySeconds = decaySecondsFor (decay01);

        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lines[i];
            const int wanted = static_cast<int> (std::lround (base[static_cast<size_t> (i)] * srScale));

            // Never longer than prepare() allocated for, including the
            // modulation excursion.
            line.length = juce::jlimit (8, line.capacity - 4, wanted);

            if (infinite)
            {
                line.feedback = 1.0f;
            }
            else
            {
                // -60 dB over the decay time, per line, so every line decays at
                // the same rate and the tail keeps its shape as Decay moves.
                const float lenSeconds = static_cast<float> (line.length) / static_cast<float> (sampleRate);
                line.feedback = std::exp (-6.907755f * lenSeconds / juce::jmax (0.05f, decaySeconds));
            }

            line.feedback = juce::jmin (line.feedback, 1.0f);

            // Decorrelated and slow: a static long tail rings metallically, and
            // one shared rate would modulate the whole tank together.
            line.modRate = 0.13f + 0.041f * static_cast<float> (i);
            line.modDepth = mod01 * static_cast<float> (kMaxModSeconds) * static_cast<float> (sampleRate);
        }

        const float dampA = juce::jlimit (0.0f, 0.999f, toneCoefficient (tone01) * kToneScale[mode]);
        const int preDelaySamples = static_cast<int> (std::lround (preDelay01 * kMaxPreDelaySeconds * sampleRate));

        for (auto& ap : diffusers)
            ap.gain = kDiffusionGain[mode];

        const float inputGain = kInputGain[mode];

        // Dry/wet through the pedal's three points: 100% dry at minimum, 50/50 at
        // 3 o'clock (three quarters of the travel), 100% wet at maximum. A plain
        // linear crossfade would put the split at the centre instead.
        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        processSamples (buffer, numCh, numSamples, dampA, preDelaySamples, inputGain, wet, dry);
    }

    void ReverbFx::processSamples (juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                                   float dampA, int preDelaySamples, float inputGain,
                                   float wet, float dry)
    {
        const float modStepScale = juce::MathConstants<float>::twoPi / static_cast<float> (sampleRate);

        // Householder reflection: y = x - (2/N) * sum(x). Orthogonal, so it
        // conserves energy - which is exactly what makes the infinite tail
        // possible without a gain compensation term.
        const float mixScale = 2.0f / static_cast<float> (numLines);
        const float outputScale = 1.0f / static_cast<float> (numLines);

        for (int s = 0; s < numSamples; ++s)
        {
            // Modulation is shared by both channels: a real modulated delay moves
            // the line, not one side of it.
            float modOffset[numLines];

            for (int i = 0; i < numLines; ++i)
            {
                auto& line = lines[i];
                line.modPhase += line.modRate * modStepScale;

                if (line.modPhase > juce::MathConstants<float>::twoPi)
                    line.modPhase -= juce::MathConstants<float>::twoPi;

                modOffset[i] = std::sin (line.modPhase) * line.modDepth;
            }

            float tankOut[2] { 0.0f, 0.0f };

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                const float dryIn = data[s];

                // --- pre-delay ------------------------------------------------
                float* pre = preBuffer.data() + static_cast<size_t> (ch) * static_cast<size_t> (preLength);
                pre[preWrite] = dryIn;

                int preRead = preWrite - preDelaySamples;

                while (preRead < 0)
                    preRead += preLength;

                float diffused = (preDelaySamples > 0) ? pre[preRead] : dryIn;

                // --- input diffusion ------------------------------------------
                for (auto& ap : diffusers)
                {
                    float* ab = ap.buffer.data()
                              + static_cast<size_t> (ch) * static_cast<size_t> (ap.length);
                    const float stored = ab[ap.writeIndex];
                    const float v = diffused + ap.gain * stored;
                    ab[ap.writeIndex] = v;
                    diffused = stored - ap.gain * v;
                }

                // --- tank: read, damp, mix, write -----------------------------
                float fb[numLines];
                float sum = 0.0f;

                for (int i = 0; i < numLines; ++i)
                {
                    auto& line = lines[i];

                    // Read a full line behind the write head, then modulate.
                    const float tap = readLine (line, ch,
                                                static_cast<float> (line.length) - modOffset[i]);

                    // Tone: a one-pole lowpass inside the loop.
                    line.dampState[ch] += dampA * (tap - line.dampState[ch]);
                    fb[i] = line.dampState[ch] * line.feedback;
                    sum += fb[i];

                    tankOut[ch] += tap * outputScale;
                }

                for (int i = 0; i < numLines; ++i)
                {
                    auto& line = lines[i];
                    const float value = diffused * inputGain + fb[i] - sum * mixScale;

                    // DC blocker, then the ceiling. Without the blocker a long
                    // tail accumulates an offset; without the ceiling an infinite
                    // tail under sustained input could grow without bound.
                    const float dc = value - line.dcX[ch] + 0.995f * line.dcY[ch];
                    line.dcX[ch] = value;
                    line.dcY[ch] = dc;

                    // Soft ceiling, then the hard backstop. Without the soft one
                    // an infinite tail would keep accumulating while a note is
                    // held; the clamp is only there so a corrupted state cannot
                    // become a runaway.
                    float* b = line.buffer.data()
                             + static_cast<size_t> (ch) * static_cast<size_t> (line.capacity);
                    b[line.writeIndex] = juce::jlimit (-kLineCeiling, kLineCeiling, softCeil (dc));
                }
            }

            // Shared indices advance once per sample, not once per channel: the
            // per-channel data is strided, not interleaved.
            if (++preWrite >= preLength)
                preWrite = 0;

            for (auto& ap : diffusers)
                if (++ap.writeIndex >= ap.length)
                    ap.writeIndex = 0;

            for (auto& line : lines)
                if (++line.writeIndex >= line.length)
                    line.writeIndex = 0;

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                data[s] = dry * data[s] + wet * tankOut[ch];
            }
        }
    }
}
