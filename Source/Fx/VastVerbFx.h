#pragma once

#include <array>
#include <vector>

#include "FxModule.h"

namespace mixxynth
{
    // VastVerb: a second reverb voice, built for size rather than for rooms.
    //
    //   in -> pre-delay -> swell -> diffusion
    //      -> 8-line FDN tank (Hadamard mixing, three-band decay, deep modulation)
    //      -> tone -> width -> mix -> out
    //
    // This is not WeirdVerb with different numbers. Three structural choices make it
    // a different instrument, and each is the reason for a specific sound:
    //
    //  * HADAMARD MIXING, not Householder. A Householder reflection about the
    //    all-ones vector, I - (2/N)J, has eigenvalue -1 along the all-ones direction
    //    and +1 along every direction orthogonal to it. It is a RANK-ONE update: it
    //    fully mixes in one direction only, and leaves the other seven untouched.
    //    The Hadamard matrix has every entry +/- 1/sqrt(N), so every output is an
    //    equal-magnitude signed sum of all eight inputs, and energy spreads across
    //    all modes on every pass. Both are orthogonal and therefore lossless, so an
    //    infinite tail survives either way - what differs is how fast the network
    //    fills in, and that is what the graininess of the tail comes from. It is
    //    also cheap: twelve add/subtract pairs in butterfly form, no multiplies.
    //
    //  * LENGTHS AN ORDER OF MAGNITUDE LONGER. WeirdVerb's tank runs 18-158 ms,
    //    which is a room. This one runs 74-432 ms at Size 1.0 and scales to 2.7x, so
    //    its longest line is over a second. That is the point of the voice: the
    //    space is enormous, and density comes from diffusion and modulation rather
    //    than from short lines. The trade is that modal density per second of decay
    //    is lower, so long tails need the diffusion to carry them.
    //
    //  * MODULATION DEPTH CAPPED, NOT PURELY PROPORTIONAL. Depth as a fixed fraction
    //    of the line length is the usual rule, and it is right for short lines: it
    //    keeps the pitch-shift ratio constant across the network so the tank moves
    //    as one instrument. But on a 1.1 s line, 2% is 22 ms of excursion at 2 Hz,
    //    which is a broken-tape warble rather than a smear. So depth is the SMALLER
    //    of a fraction of the line and a fixed 4 ms ceiling. Short lines stay
    //    proportional; long lines get bounded movement.
    //
    // There are no early reflections and no modes. This is a late-field instrument:
    // a plate at one end of its range, a black hole at the other. The wet gain is
    // normalised by sqrt(T60) because the decay range here is far wider than
    // WeirdVerb's - without it, sweeping Decay from short to infinite would be a
    // level jump of tens of dB rather than a change of tail length.
    class VastVerbFx : public FxModule
    {
    public:
        // Indices into the slot's generic parameter block. Must match descriptor().
        enum Param
        {
            pSize = 0,   // 0.45x .. 2.7x the nominal tank
            pDecay,      // mid-band T60, 0.4 s .. 60 s, infinite at the top
            pBass,       // low-band T60 relative to mid
            pDamping,    // high-band T60 relative to mid
            pDrift,      // modulation depth, the signature control
            pRate,       // modulation rate
            pDiffusion,  // input allpass strength
            pPreDelay,   // 0 - 500 ms
            pSwell,      // slow attack on the tank's input
            pLowCut,     // post highpass
            pHighCut,    // post lowpass
            pWidth,      // stereo width of the wet signal
            pMix,        // dry -> wet
            pFreeze,     // hold the tail, ignore input
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // The decay profile the card draws: how long each band takes to decay 60 dB,
        // relative to the mid band, in dB. So +6 dB at the low end means the bass
        // outlasts the mids by a factor of two, which is what the control sounds like.
        //
        // Shared maths with the audio path rather than a second implementation, which is
        // what stops the drawn curve and the applied one drifting apart: the band gains
        // come from the same relation the tank uses, and the three bands are summed as
        // low + (high - low) + (1 - high), so they add to exactly one.
        static float decayResponseDb (const float* params, float hz, double sampleRate);

        // Declared rather than left implicit: JUCE_DECLARE_NON_COPYABLE below declares
        // a deleted copy constructor, and a user-declared constructor would suppress
        // the implicit default one.
        VastVerbFx() = default;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

    private:
        static constexpr int numLines = 8;
        static constexpr int numDiffusers = 6;

        // One line of the feedback delay network.
        struct Line
        {
            std::vector<float> buffer;
            int capacity = 0;
            int length = 0;
            int writeIndex = 0;

            // Per-band loop gain, derived from this line's OWN delay so that every
            // line decays at the same rate despite their different lengths.
            float gainLow = 1.0f;
            float gainMid = 1.0f;
            float gainHigh = 1.0f;

            // Excitation and pickup vectors. Non-uniform, and different per channel:
            // see the comment where they are built.
            float inGain[2] { 0.0f, 0.0f };
            float outGain[2] { 0.0f, 0.0f };

            // The three-band attenuation filter's state, one per channel.
            float lowState[2] { 0.0f, 0.0f };
            float highState[2] { 0.0f, 0.0f };

            float modPhase = 0.0f;
            float modRate = 0.0f;
            float modDepth = 0.0f;
        };

        // A modulated Schroeder allpass. The chain of these is what turns a single
        // impulse into a cloud before the tank ever sees it.
        struct Diffuser
        {
            std::vector<float> buffer;
            int capacity = 0;
            int length = 0;
            int writeIndex = 0;
            float gain = 0.5f;
            float modPhase = 0.0f;
            float modRate = 0.0f;
            float modDepth = 0.0f;
        };

        // Interpolated read, `delaySamples` behind the write head.
        float readLine (const Line& line, int channel, float delaySamples) const noexcept;

        // Tank lengths are prime-quantised, so they only need recomputing when Size
        // has actually moved.
        void updateTankLengths (float sizeScale);

        double sampleRate = 44100.0;
        int channels = 2;

        // Headroom reserved in every delay buffer for the modulation excursion.
        int modMargin = 0;

        std::array<Line, numLines> lines;
        std::array<Diffuser, numDiffusers> diffusers;

        std::vector<float> preBuffer;
        int preLength = 0;
        int preWrite = 0;

        float lowCutState[2] { 0.0f, 0.0f };
        float highCutState[2] { 0.0f, 0.0f };

        // Swell: a one-shot ramp on the tank's input, so a sustained note blooms
        // into the reverb instead of hitting it.
        float swellGain = 0.0f;

        // Cached so the prime quantisation only runs when Size moves.
        float lastSize = -1.0f;

        bool prepared = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VastVerbFx)
    };
}
