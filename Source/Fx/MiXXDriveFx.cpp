#include "MiXXDriveFx.h"

#include <cmath>
#include <complex>

namespace mixxynth
{
    namespace
    {
        // The three-band split's corners. Fixed, because the three EQ knobs are gains and a
        // band you cannot aim is not a band.
        constexpr float kEqLowHz = 180.0f;
        constexpr float kEqHighHz = 2200.0f;

        float dbToGain (float db) noexcept
        {
            return std::pow (10.0f, db / 20.0f);
        }
    }

    const FxDescriptor& MiXXDriveFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "MiXX Drive";
            desc.numParams = kNumParams;

            // A choice's default is its index over (count - 1), not its index: the value is a
            // normalised parameter like every other one.
            desc.params[pAlgorithm].name = "Algorithm";
            desc.params[pAlgorithm].isChoice = true;
            desc.params[pAlgorithm].choices = { "Warm", "Tube", "Fold", "Clip", "Destroy" };
            desc.params[pAlgorithm].defaultValue = 0.0f;          // Warm

            desc.params[pDrive].name = "Drive";
            desc.params[pDrive].defaultValue = 0.15f;

            desc.params[pColor].name = "Color";
            desc.params[pColor].defaultValue = 0.5f;

            desc.params[pEqLow].name = "EQ Low";
            desc.params[pEqLow].defaultValue = 0.5f;

            desc.params[pEqMid].name = "EQ Mid";
            desc.params[pEqMid].defaultValue = 0.5f;

            desc.params[pEqHigh].name = "EQ High";
            desc.params[pEqHigh].defaultValue = 0.5f;

            desc.params[pEqPosition].name = "EQ Position";
            desc.params[pEqPosition].isChoice = true;
            desc.params[pEqPosition].choices = { "Pre", "Post" };
            desc.params[pEqPosition].defaultValue = 1.0f;         // Post

            desc.params[pPreserveCharacter].name = "Preserve Character";
            desc.params[pPreserveCharacter].isChoice = true;
            desc.params[pPreserveCharacter].choices = { "Off", "On" };
            desc.params[pPreserveCharacter].defaultValue = 0.0f;

            desc.params[pPreserveSub].name = "Preserve Sub";
            desc.params[pPreserveSub].isChoice = true;
            desc.params[pPreserveSub].choices = { "Off", "On" };
            desc.params[pPreserveSub].defaultValue = 0.0f;

            desc.params[pCrossover].name = "Crossover";
            desc.params[pCrossover].defaultValue = 0.2f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 1.0f;

            desc.params[pNoise].name = "Noise";
            desc.params[pNoise].defaultValue = 0.0f;

            desc.params[pNoiseColor].name = "Noise Color";
            desc.params[pNoiseColor].defaultValue = 0.5f;

            desc.params[pOversample].name = "Oversample";
            desc.params[pOversample].isChoice = true;
            desc.params[pOversample].choices = { "Off", "2x", "4x" };
            desc.params[pOversample].defaultValue = 0.5f;         // 2x

            // The card draws the three-band EQ's response above the knobs. The curve itself has
            // no response to draw, so the graph shows the part of the module that does.
            desc.drawGraph = &MiXXDriveFx::eqResponseDb;
            desc.graphLabel = "EQ dB";

            return desc;
        }();

        return d;
    }

    float MiXXDriveFx::eqGainDb (float normalised) noexcept
    {
        // Unity at the centre, +/- 12 dB at the ends. The single place the mapping lives, so
        // the audio path and the graph cannot disagree about what a knob position means.
        return (juce::jlimit (0.0f, 1.0f, normalised) - 0.5f) * 24.0f;
    }

    float MiXXDriveFx::eqResponseDb (const float* params, float hz, double sampleRate)
    {
        if (params == nullptr)
            return 0.0f;

        const float lowGain = dbToGain (eqGainDb (params[pEqLow]));
        const float midGain = dbToGain (eqGainDb (params[pEqMid]));
        const float highGain = dbToGain (eqGainDb (params[pEqHigh]));

        // The same one-pole the audio path runs, evaluated exactly rather than approximated:
        // the recursion is z += c (x - z), so H(z) = c / (1 - (1 - c) z^-1).
        auto onePoleAt = [sampleRate] (float cornerHz, float atHz)
        {
            const float c = onePoleCoeff (cornerHz, sampleRate);
            const float w = 2.0f * juce::MathConstants<float>::pi * atHz
                              / static_cast<float> (sampleRate);
            const std::complex<float> z = std::polar (1.0f, -w);

            return c / (1.0f - (1.0f - c) * z);
        };

        const auto low = onePoleAt (kEqLowHz, hz);
        const auto high = onePoleAt (kEqHighHz, hz);

        // The three bands, weighted by their gains. With all three at unity the bands sum back
        // to exactly 1, which is why a centred EQ is flat rather than merely nearly flat.
        const std::complex<float> total = low * lowGain
                                            + (high - low) * midGain
                                            + (std::complex<float> (1.0f, 0.0f) - high) * highGain;

        return 20.0f * std::log10 (juce::jmax (1.0e-6f, std::abs (total)));
    }

    float MiXXDriveFx::onePoleCoeff (float freqHz, double sr) noexcept
    {
        const float f = juce::jlimit (1.0f, static_cast<float> (sr) * 0.49f, freqHz);
        return 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * f
                                  / static_cast<float> (sr));
    }

    float MiXXDriveFx::shape (int algorithm, float x, float drive) noexcept
    {
        switch (algorithm)
        {
            case algWarm:
                // Normalised so the curve is unity at x = 1 rather than 0.716, which is what
                // stops Warm reading quieter than the other modes at the same Drive.
                return std::tanh (x * 0.9f) / std::tanh (0.9f);

            case algTube:
                // Asymmetric by construction: the x squared term is the second harmonic, and
                // the denominator is what makes it compress rather than square off.
                return (x + 0.18f * x * x) / (1.0f + std::abs (x) * 0.72f);

            case algFold:
                return std::sin (x * 1.7f);

            case algClip:
                return juce::jlimit (-0.72f, 0.72f, x) / 0.72f;

            default:
            {
                // Destroy: fold hard, then quantise. Drive moves both, so it goes from a rough
                // edge to a staircase.
                const float folded = std::sin (x * (2.0f + drive * 18.0f));
                const float steps = 4.0f + drive * 60.0f;
                return std::round (folded * steps) / steps;
            }
        }
    }

    void MiXXDriveFx::prepare (double newSampleRate, int newBlockSize, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        blockSize = juce::jmax (1, newBlockSize);
        channels = juce::jmax (1, numChannels);

        // Both oversampling factors are built and initialised HERE, so the Oversample control
        // only ever picks one. Initialising a juce::dsp::Oversampling allocates, and that cannot
        // happen on the audio thread - building them up front is what makes the choice a
        // parameter rather than a reload.
        //
        // Index 0 is "Off" and is left null: no oversampling, process in place at the host rate.
        constexpr int factors[3] = { 1, 2, 4 };

        for (int i = 1; i < 3; ++i)
        {
            const int factorLog2 = static_cast<int> (std::lround (std::log2 (factors[i])));

            oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
                static_cast<size_t> (channels), factorLog2,
                juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                true, false);

            oversamplers[i]->initProcessing (static_cast<size_t> (blockSize));
        }

        reset();
        prepared = true;
    }

    void MiXXDriveFx::reset()
    {
        eqLowState.fill (0.0f);
        eqHighState.fill (0.0f);
        colorState.fill (0.0f);
        subState.fill (0.0f);
        pinkState.fill (0.0f);
        brownState.fill (0.0f);

        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();
    }

    void MiXXDriveFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // ---- what the controls are asking for ---------------------------------

        const int algorithm = juce::jlimit (0, kNumAlgorithms - 1,
                                  static_cast<int> (std::lround (
                                      juce::jlimit (0.0f, 1.0f, params[pAlgorithm])
                                          * static_cast<float> (kNumAlgorithms - 1))));

        const float drive01 = juce::jlimit (0.0f, 1.0f, params[pDrive]);
        const float color01 = juce::jlimit (0.0f, 1.0f, params[pColor]);
        const float crossover01 = juce::jlimit (0.0f, 1.0f, params[pCrossover]);
        const float wet01 = juce::jlimit (0.0f, 1.0f, params[pMix]);
        const float noiseMix = juce::jlimit (0.0f, 1.0f, params[pNoise]);
        const float noisePosition = juce::jlimit (0.0f, 1.0f, params[pNoiseColor]) * 2.0f;

        const bool eqBefore = params[pEqPosition] < 0.5f;
        const bool preserveCharacter = params[pPreserveCharacter] >= 0.5f;
        const bool preserveSub = params[pPreserveSub] >= 0.5f;

        // Through eqGainDb, which the card's graph also uses: one mapping, so the curve drawn
        // and the curve applied cannot disagree about what a knob position means.
        const float lowGain = dbToGain (eqGainDb (params[pEqLow]));
        const float midGain = dbToGain (eqGainDb (params[pEqMid]));
        const float highGain = dbToGain (eqGainDb (params[pEqHigh]));

        const int osIndex = juce::jlimit (0, 2,
                               static_cast<int> (std::lround (
                                   juce::jlimit (0.0f, 1.0f, params[pOversample]) * 2.0f)));

        auto* os = osIndex > 0 ? oversamplers[osIndex].get() : nullptr;

        // 25 Hz to 250 Hz, and 300 Hz to 3 kHz. Both exponential, because both are frequency
        // controls and a linear one would spend most of its travel above the range that matters.
        const float crossoverHz = 25.0f * std::pow (10.0f, crossover01);
        const float colorCutoffHz = 300.0f * std::pow (10.0f, color01);

        const float inputGain = 1.0f + drive01 * 20.0f;
        const float characterAmount = preserveCharacter ? 0.72f : 0.0f;
        const float colorGain = dbToGain ((color01 - 0.5f) * 12.0f);

        // ---- the chain, at the oversampled rate --------------------------------

        juce::dsp::AudioBlock<float> block (buffer);
        auto osBlock = os != nullptr ? os->processSamplesUp (block) : block;
        const double osRate = sampleRate * (os != nullptr ? os->getOversamplingFactor() : 1.0);

        // The coefficients are computed at the rate the chain actually runs at. Computing them
        // at the host rate and then using them oversampled would move every corner by the
        // oversampling factor, which is the kind of error that sounds like the EQ doing nothing.
        const float crossoverCoeff = onePoleCoeff (crossoverHz, osRate);
        const float colorCoeff = onePoleCoeff (colorCutoffHz, osRate);
        const float lowCoeff = onePoleCoeff (kEqLowHz, osRate);
        const float highCoeff = onePoleCoeff (kEqHighHz, osRate);

        auto processEq = [&] (float input, size_t channel)
        {
            auto& lowState = eqLowState[channel];
            auto& highState = eqHighState[channel];

            lowState += lowCoeff * (input - lowState);
            highState += highCoeff * (input - highState);

            const float lowBand = lowState;
            const float midBand = highState - lowState;
            const float highBand = input - highState;

            return lowBand * lowGain + midBand * midGain + highBand * highGain;
        };

        const int osSamples = static_cast<int> (osBlock.getNumSamples());

        // Only the live channels: the oversampler is sized for the maximum, so the block can
        // report more channels than there is audio on.
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* samples = osBlock.getChannelPointer (static_cast<size_t> (ch));
            const auto channelIndex = static_cast<size_t> (ch);

            for (int i = 0; i < osSamples; ++i)
            {
                // Read before writing: this is the dry signal for the mix, and the whole chain
                // runs in place.
                const float dry = samples[i];
                float signal = eqBefore ? processEq (dry, channelIndex) : dry;

                float preservedSub = 0.0f;

                if (preserveSub)
                {
                    auto& sub = subState[channelIndex];
                    sub += crossoverCoeff * (signal - sub);
                    preservedSub = sub;
                    signal -= preservedSub;
                }

                // Noise BEFORE the curve, so the curve shapes it. Laid on afterwards it would
                // sit on top as hiss instead of becoming part of the sound.
                uint32_t randomValue = noiseState;
                randomValue ^= randomValue << 13;
                randomValue ^= randomValue >> 17;
                randomValue ^= randomValue << 5;
                noiseState = randomValue;

                const float whiteNoise = static_cast<float> (randomValue & 0x00ffffffu)
                                             / 8388607.5f - 1.0f;

                auto& pink = pinkState[channelIndex];
                pink = pink * 0.985f + whiteNoise * 0.08f;
                const float pinkNoise = juce::jlimit (-1.0f, 1.0f, pink * 2.5f);

                auto& brown = brownState[channelIndex];
                brown = brown * 0.998f + whiteNoise * 0.012f;
                const float brownNoise = juce::jlimit (-1.0f, 1.0f, brown * 3.0f);

                const float noise = noisePosition <= 1.0f
                                        ? juce::jmap (noisePosition, whiteNoise, pinkNoise)
                                        : juce::jmap (noisePosition - 1.0f, pinkNoise, brownNoise);

                signal = (signal + noise * noiseMix) * inputGain;

                float shaped = shape (algorithm, signal, drive01);

                if (preserveCharacter)
                {
                    // A conformal map: it bends the curve's output back toward the input's own
                    // shape, so driving harder does not simply flatten the performance. Mixed
                    // in rather than switched, because a hard switch would step.
                    //
                    // Ported as it stands, which means it also shifts the signal's average: at
                    // zero input the map returns -mapOffset, so the output sits below zero by
                    // 0.72 * mapOffset. That is about -0.09 at the default Drive and -0.50 at the
                    // top of its travel. MiXXFX has no DC blocker after this stage, so neither
                    // does this - the character is the point, and a blocker would remove the
                    // offset without touching it if that is ever wanted.
                    const float mapOffset = juce::jlimit (0.05f, 0.82f, drive01 * 0.82f);
                    const float bounded = juce::jlimit (-0.98f, 0.98f,
                                                        signal / (1.0f + std::abs (signal)));
                    const float denominator = juce::jmax (0.08f, 1.0f - mapOffset * bounded);
                    const float conformal = (bounded - mapOffset) / denominator;

                    shaped = juce::jmap (characterAmount, shaped, conformal);
                }

                // The colour control is a one-pole split at a movable corner: up hands more of
                // the shaped signal through, down smooths it.
                auto& color = colorState[channelIndex];
                color += colorCoeff * (shaped - color);

                float processed = color + (shaped - color) * colorGain + preservedSub;

                if (! eqBefore)
                    processed = processEq (processed, channelIndex);

                samples[i] = dry * (1.0f - wet01) + processed * wet01;
            }
        }

        // Fold the oversampled signal back down to the host rate.
        if (os != nullptr)
            os->processSamplesDown (block);
    }
}
