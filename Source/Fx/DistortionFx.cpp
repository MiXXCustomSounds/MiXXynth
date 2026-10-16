#include "DistortionFx.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        // The six mode definitions. `preEmphasisDb` is the high-shelf tilt applied
        // into the curve and undone out of it: positive drives the highs harder like a
        // tube stage, negative drives the lows like a tape head.
        constexpr DistortionFx::ModeSpec kModes[DistortionFx::kNumModes] =
        {
            // name     preDb  curveGain  asymmetry
            { "Tube",     4.0f,  1.0f,      0.25f },
            { "Tape",    -3.0f,  0.8f,      0.10f },
            { "Fuzz",     6.0f,  3.0f,      0.05f },
            { "Diode",    2.0f,  2.0f,      0.35f },
            { "Fold",     0.0f,  1.6f,      0.00f },
            { "Crush",   -2.0f,  1.0f,      0.00f }
        };

        // One-pole low-pass coefficient for a given corner. Used for the emphasis
        // shelves, the tone stack and the low cut - all of which are gentle enough
        // that one pole is the right amount.
        float onePoleCoeff (float freqHz, double sr) noexcept
        {
            const float f = juce::jlimit (1.0f, static_cast<float> (sr) * 0.49f, freqHz);
            return 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * f
                                      / static_cast<float> (sr));
        }

        // The emphasis shelf's corner. 4 kHz is above the fundamental range of most
        // playing, so the emphasis changes the character of the harmonics rather than
        // the level of the notes.
        constexpr float kEmphasisHz = 4000.0f;

        // The raw curves, before bias. Each takes a signal already scaled by the drive
        // and returns something in roughly -1..1.
        //
        // Every one except Crush is smooth through the origin, so its derivative never
        // jumps - a hard knee is most of what makes distortion sound cheap. Crush is
        // deliberately the exception: a quantiser is a staircase by definition, and
        // the steppiness IS the effect.
        float rawCurve (int mode, float x) noexcept
        {
            switch (mode)
            {
                case DistortionFx::modeTube:
                    // The classic tube-ish curve: compressive, and gentle near zero so
                    // quiet playing stays clean.
                    return x / (1.0f + std::abs (x));

                case DistortionFx::modeTape:
                {
                    // Cubic soft clip: unity slope at the origin, and the derivative
                    // reaches exactly zero at +/-1, so there is no corner where it
                    // leaves the linear region.
                    const float a = std::abs (x);

                    if (a >= 1.0f)
                        return x > 0.0f ? 2.0f / 3.0f : -2.0f / 3.0f;

                    return x - (x * x * x) / 3.0f;
                }

                case DistortionFx::modeFuzz:
                    // Exponential saturation: harder than the tube curve, and much
                    // more odd-harmonic.
                    return x > 0.0f ? 1.0f - std::exp (-x) : -(1.0f - std::exp (x));

                case DistortionFx::modeDiode:
                    // A real diode is not symmetric: forward it conducts early, reverse
                    // it barely does. That asymmetry is where the even harmonics come
                    // from, before Bias is even touched.
                    //
                    // Both halves are bounded to +/-1 and both are smooth, and the two
                    // differ in how fast they get there rather than in how far - which
                    // is the actual diode behaviour. The first version of this used
                    // different asymptotes for the two halves, which is not a diode at
                    // all: it drove the negative side to +1.67, so a negative input came
                    // out positive and the curve stopped being monotonic.
                    return x > 0.0f ? std::tanh (x) : std::tanh (0.45f * x);

                case DistortionFx::modeFold:
                    // Wavefolder: folds back on itself as the drive rises, which adds
                    // harmonics without ever clipping flat.
                    return std::sin (juce::MathConstants<float>::pi
                                       * juce::jlimit (-1.0f, 1.0f, x));

                case DistortionFx::modeCrush:
                {
                    // A staircase, on purpose. Steps are fixed rather than tracking the
                    // level, so quiet input keeps its resolution and loud input is
                    // destroyed - which is what makes it a character rather than a
                    // bit-depth control.
                    //
                    // The clamp is not cosmetic. A quantiser does not saturate, so
                    // without it the drive goes straight through: at the drive this
                    // module reaches, the first version returned 7.2 for a 0.4 input and
                    // the output stage clipped at 2.6 instead of 1.0.
                    constexpr float levels = 12.0f;

                    return std::round (juce::jlimit (-1.0f, 1.0f, x) * levels) / levels;
                }

                default:
                    break;
            }

            return x;
        }
    }

    const DistortionFx::ModeSpec& DistortionFx::specFor (int mode) noexcept
    {
        return kModes[juce::jlimit (0, kNumModes - 1, mode)];
    }

    float DistortionFx::shape (int mode, float x, float bias) noexcept
    {
        // The bias offsets the curve so its two halves differ, which is what creates
        // even harmonics. Subtracting the curve's own value at zero input means the
        // result still passes through the origin, so Bias adds character without
        // adding a level jump.
        const float m = juce::jlimit (0.0f, static_cast<float> (kNumModes - 1),
                                      static_cast<float> (mode));
        const int modeIndex = static_cast<int> (std::lround (m));

        return rawCurve (modeIndex, x + bias) - rawCurve (modeIndex, bias);
    }

    const FxDescriptor& DistortionFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Distortion";
            desc.numParams = kNumParams;

            desc.params[pMode].name = "Mode";
            desc.params[pMode].isChoice = true;

            for (int m = 0; m < kNumModes; ++m)
                desc.params[pMode].choices.add (kModes[m].name);

            // Drive starts low: this is a warmth box, and the useful range is the
            // first third of the control. Nothing here is meant to be a fuzz pedal
            // with the knob already at noon.
            desc.params[pDrive].name = "Drive";
            desc.params[pDrive].defaultValue = 0.30f;

            desc.params[pBias].name = "Bias";
            desc.params[pBias].defaultValue = 0.30f;

            desc.params[pTone].name = "Tone";
            desc.params[pTone].defaultValue = 0.50f;

            desc.params[pLowCut].name = "Low Cut";
            desc.params[pLowCut].defaultValue = 0.15f;

            desc.params[pOutput].name = "Output";
            desc.params[pOutput].defaultValue = 0.50f;

            desc.params[pOversample].name = "Oversample";
            desc.params[pOversample].isChoice = true;
            desc.params[pOversample].choices = { "Off", "2x", "4x", "8x" };
            desc.params[pOversample].defaultValue = 2.0f / 3.0f; // 4x

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 1.0f;

            return desc;
        }();

        return d;
    }

    void DistortionFx::prepare (double newSampleRate, int newBlockSize, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        blockSize = juce::jmax (1, newBlockSize);
        channels = juce::jmax (1, numChannels);

        // All four factors are built and initialised HERE, so the Oversample control
        // only ever selects one. Initialising a juce::dsp::Oversampling allocates, and
        // that cannot happen on the audio thread - building them up front is what makes
        // the choice a parameter rather than a reload.
        //
        // Index 0 is "Off" and is left null: no oversampling, process in place.
        constexpr int factors[4] = { 1, 2, 4, 8 };

        for (int i = 1; i < 4; ++i)
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

    void DistortionFx::reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            toneLow[ch].clear();
            toneHigh[ch].clear();
            preLow[ch].clear();
            postLow[ch].clear();
            dcX1[ch] = 0.0f;
            dcY1[ch] = 0.0f;
        }

        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();
    }

    void DistortionFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // ---- what the controls are asking for ---------------------------------

        const int mode = juce::jlimit (0, kNumModes - 1,
                            static_cast<int> (std::lround (juce::jlimit (0.0f, 1.0f, params[pMode])
                                                           * static_cast<float> (kNumModes - 1))));

        const auto& spec = specFor (mode);

        const float drive01 = juce::jlimit (0.0f, 1.0f, params[pDrive]);
        const float bias01 = juce::jlimit (0.0f, 1.0f, params[pBias]);
        const float tone01 = juce::jlimit (0.0f, 1.0f, params[pTone]);
        const float lowCut01 = juce::jlimit (0.0f, 1.0f, params[pLowCut]);
        const float output01 = juce::jlimit (0.0f, 1.0f, params[pOutput]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        // Up to about +22 dB into the curve, on top of the mode's own curve gain. The
        // useful warmth is in the first third, which is why Drive defaults low.
        const float driveGain = (1.0f + drive01 * 11.0f) * spec.curveGain;

        // Bias is scaled by the mode's own asymmetry, so Tube and Diode can be pushed
        // far further than Fold or Crush, which are already symmetric by design.
        const float bias = bias01 * spec.asymmetry;

        // Unity at the centre, +/- 12 dB at the ends.
        const float outputGain = 0.25f * std::pow (16.0f, output01);

        const int osIndex = juce::jlimit (0, 3,
                             static_cast<int> (std::lround (juce::jlimit (0.0f, 1.0f, params[pOversample])
                                                            * 3.0f)));

        // The tone stack: a low shelf and a high shelf moving in opposite directions,
        // +/- 6 dB, pivoting around 1 kHz. Gentle on purpose - this is a tone control
        // on a warmth box, not a filter.
        const float lowShelfGain = std::pow (10.0f, (0.5f - tone01) * 12.0f / 20.0f);
        const float highShelfGain = std::pow (10.0f, (tone01 - 0.5f) * 12.0f / 20.0f);

        // The oversampler to use, or null for "Off".
        auto* os = osIndex > 0 ? oversamplers[osIndex].get() : nullptr;

        // ---- dry, for the mix --------------------------------------------------

        juce::AudioBuffer<float> dry (numCh, numSamples);
        dry.makeCopyOf (buffer, true);

        // The emphasis shelves run INSIDE the oversampled region, and have to: they
        // boost the highs before the curve, and a boosted high that then aliases is
        // exactly the cold sound this module exists to avoid.
        const float preGain = std::pow (10.0f, spec.preEmphasisDb / 20.0f);

        auto applyCurve = [&] (float* const* data, int length, int channelCount, float shelfCoeff)
        {
            for (int ch = 0; ch < channelCount; ++ch)
            {
                auto* d = data[ch];
                auto& pre = preLow[static_cast<size_t> (ch)];
                auto& post = postLow[static_cast<size_t> (ch)];

                for (int i = 0; i < length; ++i)
                {
                    // Pre-emphasis: a high shelf, so what gets driven depends on
                    // frequency the way a real stage behaves.
                    const float low = pre.lowPass (d[i], shelfCoeff);
                    float x = low + (d[i] - low) * preGain;

                    x = shape (mode, x * driveGain, bias);

                    // De-emphasis: the same shelf inverted, so the response is flat
                    // while the curve is still behaving linearly.
                    const float low2 = post.lowPass (x, shelfCoeff);
                    d[i] = low2 + (x - low2) / preGain;
                }
            }
        };

        if (os != nullptr)
        {
            juce::dsp::AudioBlock<float> block (buffer);
            auto up = os->processSamplesUp (block);

            std::array<float*, 2> upPtrs {};

            for (int ch = 0; ch < numCh; ++ch)
                upPtrs[static_cast<size_t> (ch)] = up.getChannelPointer (static_cast<size_t> (ch));

            applyCurve (upPtrs.data(), static_cast<int> (up.getNumSamples()), numCh,
                        onePoleCoeff (kEmphasisHz,
                                      sampleRate * std::pow (2.0, static_cast<double> (osIndex))));

            os->processSamplesDown (block);
        }
        else
        {
            std::array<float*, 2> ptrs {};

            for (int ch = 0; ch < numCh; ++ch)
                ptrs[static_cast<size_t> (ch)] = buffer.getWritePointer (ch);

            applyCurve (ptrs.data(), numSamples, numCh, onePoleCoeff (kEmphasisHz, sampleRate));
        }

        // ---- tone, low cut, DC block, output, mix ------------------------------

        const float lowCoeff = onePoleCoeff (1000.0f, sampleRate);
        const float cutCoeff = onePoleCoeff (20.0f * std::pow (25.0f, lowCut01), sampleRate);
        const float dryGain = 1.0f - mix01;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            const auto* src = dry.getReadPointer (ch);

            auto& tl = toneLow[static_cast<size_t> (ch)];
            auto& th = toneHigh[static_cast<size_t> (ch)];

            float x1 = dcX1[static_cast<size_t> (ch)];
            float y1 = dcY1[static_cast<size_t> (ch)];

            for (int i = 0; i < numSamples; ++i)
            {
                float y = d[i];

                // Asymmetry generates DC, and DC downstream is worse than the
                // distortion it came from.
                const float dc = y - x1 + 0.999f * y1;
                x1 = y;
                y1 = dc;
                y = dc;

                // Tone: a low shelf and a high shelf splitting at 1 kHz.
                const float low = tl.lowPass (y, lowCoeff);
                const float high = y - low;
                y = low * lowShelfGain + high * highShelfGain;

                // Low cut: the mud the curve just added.
                y -= th.lowPass (y, cutCoeff);

                d[i] = src[i] * dryGain + y * outputGain * mix01;
            }

            dcX1[static_cast<size_t> (ch)] = x1;
            dcY1[static_cast<size_t> (ch)] = y1;
        }
    }
}
