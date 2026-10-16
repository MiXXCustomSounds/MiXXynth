#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // A ten-band graphic equaliser.
    //
    // "Graphic" means the band frequencies are FIXED and there is one gain per band, which
    // is what the word has always meant: the shape of the control surface is the shape of
    // the response. That is also why it gets a curve display rather than ten bare knobs -
    // the whole point of the layout is that you can see the curve you are drawing.
    //
    // The bands sit an octave apart from 31 Hz to 16 kHz, which is the standard spacing and
    // means adjacent bands overlap in a predictable way. Each is a peaking filter with a Q
    // of 1.4 - wide enough that neighbouring bands meet without a dip between them, narrow
    // enough that moving one does not drag the whole spectrum with it.
    class EqFx : public FxModule
    {
    public:
        static constexpr int kNumBands = 10;

        enum Param
        {
            pFirstBand = 0,               // ten of these, low to high
            pOutput = kNumBands,          // and the output trim
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // JUCE_DECLARE_NON_COPYABLE below declares a deleted copy constructor, which
        // suppresses the implicit default one.
        EqFx() = default;

        // The band centre frequencies, in Hz. Shared with the UI, which is why they are
        // exposed rather than private.
        static float bandFrequency (int band) noexcept;

        // The module's response at a frequency, in dB, from a normalised parameter block.
        // This is what the card's curve is drawn from.
        //
        // It evaluates the actual biquad magnitudes rather than summing bell curves, so the
        // picture is the transfer function and not an impression of one - if the display and
        // the audio ever disagreed, the display would be the thing that was wrong.
        static float responseDb (const float* params, float hz, double sampleRate);

        // Gain of one band, in dB, from its normalised value.
        static float bandGainDb (float normalised) noexcept;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        // One peaking biquad, direct form 1, with its own state per channel.
        struct Biquad
        {
            float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

            float z1[2] { 0.0f, 0.0f };
            float z2[2] { 0.0f, 0.0f };

            void clear() noexcept
            {
                z1[0] = z1[1] = z2[0] = z2[1] = 0.0f;
            }

            float process (float x, int channel) noexcept
            {
                const auto c = static_cast<size_t> (channel);

                const float y = b0 * x + z1[c];
                z1[c] = b1 * x - a1 * y + z2[c];
                z2[c] = b2 * x - a2 * y;

                return y;
            }
        };

        void updateCoefficients (int band, float gainDb, double sampleRate);

        std::array<Biquad, kNumBands> bands {};

        // The gain each band's coefficients were built for, so they are only recomputed when
        // something actually moves. Ten biquads per sample would be ten tans per sample.
        std::array<float, kNumBands> currentGainDb {};

        double sampleRate = 44100.0;
        int channels = 2;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqFx)
    };
}
