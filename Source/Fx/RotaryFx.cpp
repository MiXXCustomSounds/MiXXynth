#include "RotaryFx.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

        float onePoleCoeff (float freqHz, double sr) noexcept
        {
            const float f = juce::jlimit (1.0f, static_cast<float> (sr) * 0.49f, freqHz);

            return 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * f
                                    / static_cast<float> (sr));
        }

        // The horn speed a control value asks for. Exponential, so equal knob movements sound
        // equal, and spanning the two speeds a Leslie is actually played at: a slow chorale
        // around 0.8 Hz and a fast tremolo around 6.8 Hz.
        float rateFor (float value01) noexcept
        {
            return 0.05f * std::pow (240.0f, juce::jlimit (0.0f, 1.0f, value01));
        }

        // The horn/drum split, exponential for the same reason. A cabinet's own crossover sits
        // near 800 Hz, which is the centre of this range.
        float crossoverFor (float value01) noexcept
        {
            return 200.0f * std::pow (20.0f, juce::jlimit (0.0f, 1.0f, value01));
        }
    }

    const FxDescriptor& RotaryFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Rotary";
            desc.numParams = kNumParams;

            // The defaults are a usable Leslie rather than a neutral one: slow chorale, mics
            // spread, crossover at the cabinet's own 800 Hz, and fully wet, because a speaker
            // cabinet is not something you blend back into the direct signal.
            desc.params[pRate].name = "Rate";
            desc.params[pRate].defaultValue = 0.5f;        // ~0.77 Hz, the chorale speed

            desc.params[pDepth].name = "Depth";
            desc.params[pDepth].defaultValue = 0.5f;

            desc.params[pWidth].name = "Width";
            desc.params[pWidth].defaultValue = 0.7f;

            desc.params[pCrossover].name = "Crossover";
            desc.params[pCrossover].defaultValue = 0.5f;   // ~894 Hz

            desc.params[pBalance].name = "Balance";
            desc.params[pBalance].defaultValue = 0.5f;

            desc.params[pDrive].name = "Drive";
            desc.params[pDrive].defaultValue = 0.0f;       // clean until asked

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 1.0f;         // a cabinet is fully wet

            return desc;
        }();

        return d;
    }

    void RotaryFx::DelayLine::resize (int lengthSamples)
    {
        buffer.assign (static_cast<size_t> (juce::jmax (8, lengthSamples)), 0.0f);
        writeIndex = 0;
    }

    void RotaryFx::DelayLine::clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    void RotaryFx::DelayLine::write (float sample) noexcept
    {
        if (buffer.empty())
            return;

        buffer[static_cast<size_t> (writeIndex)] = sample;
        writeIndex = (writeIndex + 1) % static_cast<int> (buffer.size());
    }

    float RotaryFx::DelayLine::read (float delaySamples) const noexcept
    {
        const int size = static_cast<int> (buffer.size());

        if (size <= 0)
            return 0.0f;

        // Clamped so a delay longer than the line reads the oldest sample rather than wrapping
        // to the newest, which would be a click rather than a decay.
        const float d = juce::jlimit (1.0f, static_cast<float> (size - 2), delaySamples);

        const float readPos = static_cast<float> (writeIndex) - d;
        const float wrapped = readPos < 0.0f ? readPos + static_cast<float> (size) : readPos;

        const int i0 = static_cast<int> (wrapped);
        const int i1 = (i0 + 1) % size;
        const float frac = wrapped - static_cast<float> (i0);

        return buffer[static_cast<size_t> (i0)] * (1.0f - frac)
                 + buffer[static_cast<size_t> (i1)] * frac;
    }

    void RotaryFx::prepare (double newSampleRate, int, int newChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, newChannels);

        // Sized for the longest delay either rotor can ask for, plus the interpolation's own
        // one-sample lookahead.
        const int length = static_cast<int> (kMaxDelaySeconds * sampleRate) + 4;

        horn.line.resize (length);
        drum.line.resize (length);

        sweepSm = 0.0f;
        sweepPrimed = false;

        prepared = true;
        reset();
    }

    void RotaryFx::reset()
    {
        horn.line.clear();
        drum.line.clear();

        horn.angle = 0.0;
        drum.angle = kDrumStartPhase * static_cast<double> (kTwoPi);

        lowLp1[0] = lowLp1[1] = 0.0f;
        lowLp2[0] = lowLp2[1] = 0.0f;
    }

    void RotaryFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numCh = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        if (numCh <= 0 || numSamples <= 0)
            return;

        const float depth = juce::jlimit (0.0f, 1.0f, params[pDepth]);
        const float width = juce::jlimit (0.0f, 1.0f, params[pWidth]);
        const float balance = juce::jlimit (0.0f, 1.0f, params[pBalance]);
        const float drive = juce::jlimit (0.0f, 1.0f, params[pDrive]);
        const float mix = juce::jlimit (0.0f, 1.0f, params[pMix]);

        const float hornRate = rateFor (params[pRate]);
        const float drumRate = hornRate * kDrumRateRatio;

        const float lpCoeff = onePoleCoeff (crossoverFor (params[pCrossover]), sampleRate);

        // Width IS the mic separation. At zero the two mics sit in the same place, hear the same
        // thing, and the output is mono - which is what makes this a width control rather than a
        // second depth control.
        const float micAngle = kMicAngle * width;

        const float sweepTarget = depth * kMaxSweepSeconds;
        const float sweepCoeff = 1.0f - std::exp (-1.0f / (kSweepSlewSeconds
                                                          * static_cast<float> (sampleRate)));

        if (! sweepPrimed)
        {
            sweepSm = sweepTarget;
            sweepPrimed = true;
        }

        // An equal-power balance, so the centre is not a level dip. Balance 0 is all drum and 1
        // is all horn, named for what the control does to the sound rather than to a graph.
        const float hornGain = std::sin (balance * juce::MathConstants<float>::pi * 0.5f);
        const float drumGain = std::cos (balance * juce::MathConstants<float>::pi * 0.5f);

        auto* dataL = buffer.getWritePointer (0);
        auto* dataR = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

        const float dt = static_cast<float> (1.0 / sampleRate);
        const float sr = static_cast<float> (sampleRate);

        for (int n = 0; n < numSamples; ++n)
        {
            const float dryL = dataL[n];
            const float dryR = dataR != nullptr ? dataR[n] : dryL;

            sweepSm += sweepCoeff * (sweepTarget - sweepSm);

            // The cabinet's amp first, so what rotates is what the cabinet was given.
            float inL = dryL;
            float inR = dryR;

            if (drive > 0.0f)
            {
                const float g = 1.0f + drive * 4.0f;
                inL = std::tanh (inL * g);
                inR = std::tanh (inR * g);
            }

            // --- crossover, per channel, then each band summed to mono for its own rotor ---
            lowLp1[0] += lpCoeff * (inL - lowLp1[0]);
            lowLp2[0] += lpCoeff * (lowLp1[0] - lowLp2[0]);
            const float lowL = lowLp2[0];

            lowLp1[1] += lpCoeff * (inR - lowLp1[1]);
            lowLp2[1] += lpCoeff * (lowLp1[1] - lowLp2[1]);
            const float lowR = lowLp2[1];

            const float drumIn = (lowL + lowR) * 0.5f;
            const float hornIn = ((inL - lowL) + (inR - lowR)) * 0.5f;

            // --- advance both rotors ---
            horn.angle += static_cast<double> (hornRate * dt * kTwoPi);
            drum.angle += static_cast<double> (drumRate * dt * kTwoPi);

            if (horn.angle >= static_cast<double> (kTwoPi))
                horn.angle -= static_cast<double> (kTwoPi);

            if (drum.angle >= static_cast<double> (kTwoPi))
                drum.angle -= static_cast<double> (kTwoPi);

            horn.line.write (hornIn);
            drum.line.write (drumIn);

            const float hornTheta = static_cast<float> (horn.angle);
            const float drumTheta = static_cast<float> (drum.angle);

            // Each mic sees the rotor's directivity, and sits its own distance from it. The path
            // to a mic is `base - sweep * cos(theta - mic)`: shortest when the rotor is pointing
            // at that mic, which is also when that mic hears it loudest.
            const float hornL = horn.line.read ((kBaseDelaySeconds
                                                 - sweepSm * std::cos (hornTheta - micAngle)) * sr);
            const float hornR = horn.line.read ((kBaseDelaySeconds
                                                 - sweepSm * std::cos (hornTheta + micAngle)) * sr);
            const float drumL = drum.line.read ((kBaseDelaySeconds
                                                 - sweepSm * std::cos (drumTheta - micAngle)) * sr);
            const float drumR = drum.line.read ((kBaseDelaySeconds
                                                 - sweepSm * std::cos (drumTheta + micAngle)) * sr);

            const float hornGainL = 0.5f + 0.5f * std::cos (hornTheta - micAngle);
            const float hornGainR = 0.5f + 0.5f * std::cos (hornTheta + micAngle);
            const float drumGainL = 0.5f + 0.5f * std::cos (drumTheta - micAngle);
            const float drumGainR = 0.5f + 0.5f * std::cos (drumTheta + micAngle);

            // The doubling restores the level a cardioid pair loses: each mic averages half the
            // rotor's output, so the two bands together would otherwise sit 6 dB down.
            const float wetL = (hornL * hornGainL * hornGain
                                  + drumL * drumGainL * drumGain) * 2.0f;
            const float wetR = (hornR * hornGainR * hornGain
                                  + drumR * drumGainR * drumGain) * 2.0f;

            dataL[n] = dryL + (wetL - dryL) * mix;

            if (dataR != nullptr)
                dataR[n] = dryR + (wetR - dryR) * mix;
        }
    }
}
