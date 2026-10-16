#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // Phaser: cascaded first-order allpasses swept by an LFO, with feedback.
    //
    //   in -> [+] -> AP1 -> AP2 -> ... -> APn -> out
    //          ^                               |
    //          |            feedback           |
    //          +-------------------------------+
    //
    // Each allpass shifts phase without changing level, and the sum with the dry signal
    // is what makes the notches. The stage count is therefore the main character
    // control, which is why it is a SELECTOR rather than a knob - "5.3 stages" is not a
    // thing, and a continuous control pretending otherwise is just a way to make the
    // useful settings hard to land on.
    //
    // Odd counts are offered as well as even ones. They give a different notch pattern -
    // an odd chain leaves a notch at DC, an even one does not - and the difference is
    // audible, not academic.
    //
    // Feedback around the chain is most of what separates a lush phaser from a weak one:
    // it sharpens the notches and gives the sweep somewhere to resonate.
    //
    // The coefficient is swept LINEARLY in `a` rather than in frequency. `a` is monotonic
    // in the break frequency, and sweeping it directly avoids a tan() per sample for a
    // difference nobody can hear.
    class PhaserFx : public FxModule
    {
    public:
        enum Param
        {
            pStages = 0,   // choice: 2 / 3 / 4 / 6 / 8 / 10 / 12
            pRate,
            pDepth,
            pFeedback,
            pCentre,
            pStereo,       // LFO phase offset between the channels
            pStageOffset,  // choice: Same / Right + 2
            pMix,
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // The stage counts on offer. Even and odd both, deliberately.
        static int stageCountFor (int index) noexcept;
        static int getNumStageCounts() noexcept;

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares a
        // deleted copy constructor, and a user-declared constructor suppresses the
        // implicit default one.
        PhaserFx() = default;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        // The most stages the fixed array has to hold, plus the stereo offset.
        static constexpr int kMaxStages = 14;

    private:
        // One first-order allpass, in transposed direct form.
        struct Allpass
        {
            float z = 0.0f;

            void clear() noexcept { z = 0.0f; }

            float process (float x, float a) noexcept
            {
                const float y = a * x + z;
                z = x - a * y;
                return y;
            }
        };

        double sampleRate = 44100.0;
        int channels = 2;

        // Per channel, the chain. Fixed size, and the stage count decides how much of it
        // runs - so changing the selector is not an allocation.
        std::array<std::array<Allpass, kMaxStages>, 2> stages {};

        // Feedback state, per channel.
        float fbState[2] { 0.0f, 0.0f };

        float lfoPhase = 0.0f;

        // The coefficient actually in use, slewed so changing Centre glides rather than
        // jumping the notches across the spectrum.
        float currentA = 0.0f;
        bool aPrimed = false;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhaserFx)
    };
}
