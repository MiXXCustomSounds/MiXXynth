#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // A three-band upward/downward compressor, in the OTT tradition.
    //
    // The thing that makes OTT sound like OTT is that it compresses in BOTH directions at
    // once, per band:
    //
    //   * DOWNWARD - what is above the threshold comes down, which is ordinary
    //     compression and is what tames peaks.
    //   * UPWARD - what is below the threshold comes UP, which is the half nobody expects
    //     and the whole character. Quiet detail is lifted toward the loud parts, so a
    //     decay keeps its tail and a reverb keeps its shape instead of disappearing.
    //
    // Run on three bands, that gives the flattened, hyper-detailed, slightly unnatural
    // sound people reach for OTT to get. It is not a mastering tool and it is not meant
    // to be transparent.
    //
    // Depth is the dry/wet, and it is what makes the effect usable: at 100% it is the full
    // thing, and at 30% it is a useful "glue and detail" setting on a bus.
    class MultibandFx : public FxModule
    {
    public:
        enum Param
        {
            pSplitLow = 0,   // low/mid crossover
            pSplitHigh,      // mid/high crossover
            pDownward,
            pUpward,
            pTime,           // scales every attack and release together
            pDepth,          // dry/wet
            kNumParams
        };

        static constexpr int kNumBands = 3;

        static const FxDescriptor& descriptor();

        // JUCE_DECLARE_NON_COPYABLE below declares a deleted copy constructor, which
        // suppresses the implicit default one.
        MultibandFx() = default;

        // The gain a band gets, in dB, for a given level in dB. Static and public so the
        // smoke test can check that the two halves really do pull in opposite directions
        // rather than inferring it from audio.
        static float bandGainDb (float levelDb, float downward, float upward) noexcept;

        // Per-band gain reduction of the most recent block, for a meter. Index 0 is low.
        float getBandGainDb (int band) const noexcept;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        // A second-order Linkwitz-Riley section: two cascaded one-poles, so 12 dB/oct. LR
        // rather than Butterworth because the two halves of an LR crossover sum back to
        // flat in amplitude, which is what a band split needs - a Butterworth pair sums to
        // a bump at the crossover and a dip either side of it.
        struct OnePole
        {
            float z = 0.0f;

            void clear() noexcept { z = 0.0f; }

            float process (float x, float coeff) noexcept
            {
                z += coeff * (x - z);
                return z;
            }
        };

        // One channel's crossover state. Six one-poles rather than a filter per band,
        // because the three bands are DERIVED from two low-passes rather than filtered
        // independently: low is LP(f1), high is the complement of LP(f2), and mid is
        // LP(f2) applied to the high-passed input. Sharing the two low-passes is what makes
        // the bands sum back to the input.
        struct ChannelFilters
        {
            OnePole lowA, lowB;    // LP at the low split, on the input
            OnePole highA, highB;  // LP at the high split, on the input
            OnePole midA, midB;    // LP at the high split, on the high-passed input

            void clear() noexcept
            {
                lowA.clear(); lowB.clear();
                highA.clear(); highB.clear();
                midA.clear(); midB.clear();
            }
        };

        std::array<ChannelFilters, 2> filters {};

        // The envelopes are per BAND, not per channel: the detector is linked, so the gain
        // is computed once from the louder channel and applied to both.
        std::array<float, kNumBands> envelope { 0.0f, 0.0f, 0.0f };

        // The gain actually in use, per band, slewed between the attack and release
        // coefficients. Per BAND rather than per channel, like the envelope, because the
        // gain is linked.
        std::array<float, kNumBands> previousGain { 1.0f, 1.0f, 1.0f };

        std::array<float, kNumBands> reportedDb { 0.0f, 0.0f, 0.0f };

        double sampleRate = 44100.0;
        int channels = 2;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandFx)
    };
}
