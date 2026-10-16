#pragma once

#include <array>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "FxModule.h"

namespace mixxynth
{
    // Multi-mode distortion, built for warmth rather than for gain.
    //
    // "Warm" is a real engineering constraint, not a label. The things that make
    // distortion sound cold and cheap are exactly the things that are easy to do by
    // accident, so each one is answered by a specific part of this design:
    //
    //   aliasing            -> oversampling, 2x / 4x / 8x, half-band FIR. Most of
    //                          "warm" on its own: a soft curve generates harmonics
    //                          above Nyquist, and without this they fold back down
    //                          as inharmonic junk.
    //   a hard knee         -> every mode's curve is smooth through the origin, so
    //                          there is no point where the derivative jumps.
    //   odd harmonics only  -> Bias, which offsets the curve so the two halves
    //                          differ and even harmonics appear. This is the single
    //                          biggest difference between warm and buzzy.
    //   drive with no EQ    -> pre-emphasis into the non-linearity and de-emphasis
    //                          out of it, so what is driven depends on frequency,
    //                          the way a tube stage or a tape head actually behaves.
    //   DC offset           -> Bias generates it, so a DC blocker follows.
    //
    // The six modes are six transfer curves with their own emphasis, not one curve at
    // six gains.
    class DistortionFx : public FxModule
    {
    public:
        enum Param
        {
            pMode = 0,     // choice: Tube / Tape / Fuzz / Diode / Fold / Crush
            pDrive,
            pBias,
            pTone,
            pLowCut,
            pOutput,
            pOversample,   // choice: Off / 2x / 4x / 8x
            pMix,
            kNumParams
        };

        enum Mode
        {
            modeTube = 0,
            modeTape,
            modeFuzz,
            modeDiode,
            modeFold,
            modeCrush,
            kNumModes
        };

        // One mode's curve and the emphasis around it.
        struct ModeSpec
        {
            const char* name;

            // Pre-emphasis tilt in dB at 4 kHz before the curve, undone after it.
            // Positive drives the highs harder, which is what a tube stage does;
            // negative drives the lows, which is what a tape head does.
            float preEmphasisDb;

            // How hard the curve is pushed before its own shaping, so the modes are
            // not merely different shapes at the same level.
            float curveGain;

            // How much of the curve's output is second-harmonic asymmetry by default.
            float asymmetry;
        };

        static const ModeSpec& specFor (int mode) noexcept;

        static const FxDescriptor& descriptor();

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares a
        // deleted copy constructor, and a user-declared constructor suppresses the
        // implicit default one. Without this line the class cannot be constructed.
        DistortionFx() = default;

        // One sample of the mode's transfer curve. Public and static so the smoke
        // test can check the shapes directly - smoothness, monotonicity, and that
        // Bias really does introduce even harmonics.
        static float shape (int mode, float x, float bias) noexcept;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        // A one-pole shelf, used for the emphasis and the tone stack. Two of these in
        // series is 12 dB/oct, which is enough to hear without ringing.
        struct OnePole
        {
            float z = 0.0f;

            void clear() noexcept { z = 0.0f; }

            float lowPass (float x, float coeff) noexcept
            {
                z += coeff * (x - z);
                return z;
            }
        };

        // The low and high shelves of the tone stack, and the emphasis pair.
        std::array<OnePole, 2> toneLow {};
        std::array<OnePole, 2> toneHigh {};
        std::array<OnePole, 2> preLow {};
        std::array<OnePole, 2> postLow {};

        // The DC blocker, one per channel. Asymmetry makes DC, and DC in a delay or a
        // reverb downstream is worse than the distortion it came from.
        std::array<float, 2> dcX1 {};
        std::array<float, 2> dcY1 {};

        // Four oversamplers, all built and prepared up front, so choosing one is a
        // pointer pick rather than an allocation. juce::dsp::Oversampling has to be
        // initialised with the block size, which cannot happen on the audio thread -
        // so building them all at prepare time is what makes the choice safe.
        std::unique_ptr<juce::dsp::Oversampling<float>> oversamplers[4];

        double sampleRate = 44100.0;
        int channels = 2;
        int blockSize = 512;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DistortionFx)
    };
}
