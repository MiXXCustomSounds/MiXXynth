#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "FxModule.h"

namespace mixxynth
{
    // A single-band compressor.
    //
    // Feed-forward, with a soft knee, and a choice of peak or RMS detection. Those two
    // are not a nuance: peak detection catches transients and is what you want on a drum
    // bus, RMS follows the perceived level and is what you want on a vocal or a whole
    // mix. Offering one and calling it a compressor would be offering half the tool.
    //
    // The detector runs on the LINKED peak across the channels, not per channel. A
    // compressor that works on each side independently pulls the stereo image around
    // whenever one side is louder, which is the single most common way a compressor ends
    // up sounding broken.
    //
    // Mix is parallel compression rather than a wet/dry for its own sake: heavy
    // compression blended back against the dry signal is how you get density without
    // losing the transients, and it is a standard technique rather than a compromise.
    class CompressorFx : public FxModule
    {
    public:
        enum Param
        {
            pDetector = 0,   // choice: Peak / RMS
            pThreshold,
            pRatio,
            pAttack,
            pRelease,
            pKnee,
            pMakeup,
            pMix,
            kNumParams
        };

        enum Detector
        {
            detectorPeak = 0,
            detectorRms
        };

        static const FxDescriptor& descriptor();

        // JUCE_DECLARE_NON_COPYABLE below declares a deleted copy constructor, which
        // suppresses the implicit default one.
        CompressorFx() = default;

        // The gain the compressor would apply, in dB, for a given input level in dB. Static
        // and public so the smoke test can check the curve - the knee, the ratio above the
        // threshold, and that unity is unity below it - rather than inferring it from audio.
        static float gainReductionDb (float inputDb, float thresholdDb, float ratio,
                                      float kneeDb) noexcept;

        // The most recent gain reduction, for a meter. Relaxed atomic, same contract as the
        // limiter's.
        float getGainReductionDb() const noexcept { return reportedGrDb; }

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        double sampleRate = 44100.0;
        int channels = 2;

        // The detector's envelope, one per channel so a stereo signal's level is followed
        // correctly - but the GAIN is computed from the louder of the two.
        std::array<float, 2> rmsState { 0.0f, 0.0f };
        std::array<float, 2> peakState { 0.0f, 0.0f };

        float gain = 1.0f;

        std::atomic<float> reportedGrDb { 0.0f };

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressorFx)
    };
}
