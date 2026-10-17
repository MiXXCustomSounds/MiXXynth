#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include "FxModule.h"

namespace mixxynth
{
    // The MiXX drive: the distortion from MiXXFX, ported to a rack module.
    //
    // A different animal from DistortionFx, which is MiXXynth's own six-curve warmth box. This
    // one is built around four things that module does not have:
    //
    //   character preservation  a conformal map applied alongside the curve, so the input's own
    //                           dynamics survive being driven instead of being flattened into
    //                           the curve's shape
    //   sub preservation        a crossover that holds the low band back from the curve and adds
    //                           it back afterwards, so a bass keeps its fundamental
    //   noise                   white / pink / brown injected BEFORE the curve, so the curve
    //                           shapes the noise rather than hiss being laid on top
    //   three-band EQ           with a pre/post switch, so the emphasis can sit on either side
    //                           of the non-linearity
    //
    // Five algorithms, and two of them are deliberately not smooth: Clip has a hard corner and
    // Destroy quantises. Those are the effects, not accidents - the same way DistortionFx's
    // Crush is a staircase on purpose.
    //
    // Ported from MiXXFX's processBlock, keeping its curve constants, its signal order and its
    // noise seed.
    //
    // The whole chain runs in place: the dry signal for the mix is read from the same buffer the
    // result is written to, before it is overwritten. That is what MiXXFX does, and it means
    // this module needs no scratch buffer at all - which is how it meets the rack's rule that
    // process() must not allocate.
    class MiXXDriveFx : public FxModule
    {
    public:
        enum Param
        {
            pAlgorithm = 0,     // choice: Warm / Tube / Fold / Clip / Destroy
            pDrive,
            pColor,
            pEqLow,
            pEqMid,
            pEqHigh,
            pEqPosition,        // choice: Pre / Post
            pPreserveCharacter, // choice: Off / On
            pPreserveSub,       // choice: Off / On
            pCrossover,
            pMix,
            pNoise,
            pNoiseColor,
            pOversample,        // choice: Off / 2x / 4x
            kNumParams
        };

        enum Algorithm
        {
            algWarm = 0,
            algTube,
            algFold,
            algClip,
            algDestroy,
            kNumAlgorithms
        };

        static const FxDescriptor& descriptor();

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares a
        // deleted copy constructor, and a user-declared constructor suppresses the implicit
        // default one. Without this line the class cannot be constructed at all.
        MiXXDriveFx() = default;

        // One sample of an algorithm's transfer curve.
        //
        // Public and static so the smoke test can check the shapes directly. `drive` is an
        // argument because Destroy's fold depth and its step count both scale with it.
        static float shape (int algorithm, float x, float drive) noexcept;

        // One band's gain in dB: unity at the centre, +/- 12 dB at the ends.
        static float eqGainDb (float normalised) noexcept;

        // The three-band EQ's response in dB, for the card's graph.
        //
        // Shared maths rather than a second implementation: the audio path derives its band
        // gains through eqGainDb and its corners through onePoleCoeff, and this does the same,
        // which is what stops the drawn curve and the applied one drifting apart. The graph
        // shows the EQ rather than the whole module, because the curve itself is not linear and
        // has no frequency response to draw.
        static float eqResponseDb (const float* params, float hz, double sampleRate);

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        // A one-pole low-pass coefficient. The crossover, the colour filter and both EQ splits
        // are all gentle enough that one pole is the right amount.
        static float onePoleCoeff (float freqHz, double sr) noexcept;

        std::array<float, 2> eqLowState {};
        std::array<float, 2> eqHighState {};
        std::array<float, 2> colorState {};
        std::array<float, 2> subState {};
        std::array<float, 2> pinkState {};
        std::array<float, 2> brownState {};

        // The noise source: a xorshift32, seeded to the same constant MiXXFX uses, so a patch
        // with the noise up sounds the same here as it does there.
        uint32_t noiseState = 0x9e3779b9u;

        // Index 0 is "Off" and is left null - no oversampling, process in place at the host
        // rate. Both factors are built up front because initialising one allocates.
        std::unique_ptr<juce::dsp::Oversampling<float>> oversamplers[3];

        double sampleRate = 44100.0;
        int channels = 2;
        int blockSize = 512;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXDriveFx)
    };
}
