#include "JunoChorusFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        // The measured hardware values. See the table in the header.
        constexpr JunoChorusFx::ModeSpec kModes[JunoChorusFx::kNumModes] =
        {
            // name   rateHz   centreMs  depthMs  mono
            { "I",     0.513f,  3.505f,   1.845f,  false },
            { "II",    0.863f,  3.505f,   1.845f,  false },
            { "I+II",  9.75f,   3.500f,   0.200f,  true  }
        };

        // The dry/wet curve the other modules use, so a chain of them behaves the
        // same way at the same Mix setting.
        constexpr float kMixSplitAt = 0.5f;

        // The tone control's range. The top is deliberately above hearing: at 1.0
        // the BBD's low-pass is effectively out of the way, which is how you A/B
        // what the dark wet path is actually doing.
        constexpr float kToneMinHz = 1000.0f;
        constexpr float kToneMaxHz = 16000.0f;

        // Default tone, chosen so it lands on the measured BBD corner rather than
        // on a round number in normalised space. See descriptor().
        constexpr float kToneDefault01 = 0.70f;

        // The rate and depth trims: 1.0x at the centre of the control, 0.25x at the
        // bottom and 4x at the top. Exponential, so equal knob movements sound equal.
        float trimFor (float value01) noexcept
        {
            return std::pow (4.0f, (juce::jlimit (0.0f, 1.0f, value01) - 0.5f) * 2.0f);
        }

        float onePoleCoeff (float freqHz, double sr) noexcept
        {
            const float f = juce::jlimit (1.0f, static_cast<float> (sr) * 0.49f, freqHz);
            return 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * f
                                      / static_cast<float> (sr));
        }
    }

    const JunoChorusFx::ModeSpec& JunoChorusFx::specFor (int mode) noexcept
    {
        return kModes[juce::jlimit (0, kNumModes - 1, mode)];
    }

    float JunoChorusFx::triangle (float phase01) noexcept
    {
        // 4 * |p - 0.5| - 1: +1 at p = 0, -1 at p = 0.5, +1 at p = 1. Straight edges,
        // which is the point - a sine here gives a chorus, a triangle gives the Juno.
        return 4.0f * std::abs (phase01 - 0.5f) - 1.0f;
    }

    const FxDescriptor& JunoChorusFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Juno Chorus";
            desc.numParams = kNumParams;

            desc.params[pMode].name = "Mode";
            desc.params[pMode].isChoice = true;

            for (int m = 0; m < kNumModes; ++m)
                desc.params[pMode].choices.add (kModes[m].name);

            // Both trims default to the centre, which is exactly 1.0x - the hardware
            // value. So a freshly loaded Juno Chorus is the emulation, and moving
            // either control takes it away from that. Stated here because it is the
            // difference between "an emulation with trims" and "a chorus with presets".
            desc.params[pRate].name = "Rate";
            desc.params[pRate].defaultValue = 0.5f;

            desc.params[pDepth].name = "Depth";
            desc.params[pDepth].defaultValue = 0.5f;

            desc.params[pTone].name = "Tone";
            desc.params[pTone].defaultValue = kToneDefault01;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.5f;

            return desc;
        }();

        return d;
    }

    void JunoChorusFx::DelayLine::resize (int lengthSamples)
    {
        buffer.assign (static_cast<size_t> (juce::jmax (4, lengthSamples)), 0.0f);
        writeIndex = 0;
    }

    void JunoChorusFx::DelayLine::clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    float JunoChorusFx::DelayLine::read (float delaySamples) const noexcept
    {
        const int size = static_cast<int> (buffer.size());

        if (size <= 0)
            return 0.0f;

        // Clamped so a delay longer than the line reads the oldest sample rather
        // than wrapping to the newest, which would be a click rather than a decay.
        const float d = juce::jlimit (1.0f, static_cast<float> (size - 2), delaySamples);

        const float readPos = static_cast<float> (writeIndex) - d;

        int i0 = static_cast<int> (std::floor (readPos));
        const float frac = readPos - static_cast<float> (i0);

        i0 = ((i0 % size) + size) % size;
        const int i1 = (i0 + 1) % size;

        return buffer[static_cast<size_t> (i0)]
                 + frac * (buffer[static_cast<size_t> (i1)] - buffer[static_cast<size_t> (i0)]);
    }

    void JunoChorusFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        // Sized once, here, so process() never allocates. Two extra samples so the
        // interpolated read always has a neighbour to reach for.
        const int length = static_cast<int> (kMaxDelaySeconds * sampleRate) + 4;

        for (auto& line : lines)
            line.resize (length);

        reset();
        prepared = true;
    }

    void JunoChorusFx::reset()
    {
        for (auto& line : lines)
            line.clear();

        lfoPhase = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
        {
            lp1[ch] = 0.0f;
            lp2[ch] = 0.0f;
        }

        // Not primed, so the first block starts the slew from the mode's own centre
        // rather than gliding in from wherever the last patch left it.
        delayPrimed = false;
        currentCentreMs = 0.0f;
        currentDepthMs = 0.0f;
    }

    void JunoChorusFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // ---- what the controls are asking for ---------------------------------

        // The mode picks the measured hardware behaviour; the trims scale it. At
        // their defaults the trims are exactly 1.0x, so this is the emulation.
        const int mode = juce::jlimit (0, kNumModes - 1,
                            static_cast<int> (std::lround (juce::jlimit (0.0f, 1.0f, params[pMode])
                                                           * static_cast<float> (kNumModes - 1))));

        const auto& spec = specFor (mode);

        const float rateHz = spec.rateHz * trimFor (params[pRate]);
        const float depthMs = spec.depthMs * trimFor (params[pDepth]);

        // The BBD's dark wet path: two cascaded one-poles, so 12 dB/oct, which is
        // what the hardware puts in front of the delay line. Applied to the wet
        // signal here rather than to the input there, which is the same thing for a
        // linear filter either side of a delay - and this way the dry signal never
        // passes through it.
        const float tone01 = juce::jlimit (0.0f, 1.0f, params[pTone]);
        const float toneHz = kToneMinHz * std::pow (kToneMaxHz / kToneMinHz, tone01);
        const float lpCoeff = onePoleCoeff (toneHz, sampleRate);

        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);
        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        const float lfoInc = rateHz / static_cast<float> (sampleRate);
        const float delaySlew = 1.0f - std::exp (-1.0f
                                    / (kDelaySlewSeconds * static_cast<float> (sampleRate)));

        if (! delayPrimed)
        {
            currentCentreMs = spec.centreMs;
            currentDepthMs = depthMs;
            delayPrimed = true;
        }

        const float samplesPerMs = static_cast<float> (sampleRate) * 0.001f;

        for (int n = 0; n < numSamples; ++n)
        {
            // The mode's centre and depth glide to their new values, so switching
            // mode sweeps rather than jumps. The LFO then sweeps around wherever it
            // has got to.
            currentCentreMs += (spec.centreMs - currentCentreMs) * delaySlew;
            currentDepthMs += (depthMs - currentDepthMs) * delaySlew;

            const float lfo = triangle (lfoPhase);

            lfoPhase += lfoInc;

            if (lfoPhase >= 1.0f)
                lfoPhase -= std::floor (lfoPhase);

            const float centreSamples = currentCentreMs * samplesPerMs;
            const float depthSamples = currentDepthMs * samplesPerMs;

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                const float in = data[n];

                // Anti-phase: the left line sweeps up while the right sweeps down.
                // In I+II the two move together instead, which is why that mode is
                // mono - there is no difference between the channels to hear.
                const float polarity = (spec.mono || ch == 0) ? 1.0f : -1.0f;

                const float delayed = lines[ch].read (centreSamples
                                                        + depthSamples * lfo * polarity);

                lp1[ch] += lpCoeff * (delayed - lp1[ch]);
                lp2[ch] += lpCoeff * (lp1[ch] - lp2[ch]);

                auto& line = lines[ch];
                line.buffer[static_cast<size_t> (line.writeIndex)] = in;
                line.writeIndex = (line.writeIndex + 1) % static_cast<int> (line.buffer.size());

                data[n] = dry * in + wet * lp2[ch];
            }
        }
    }
}

