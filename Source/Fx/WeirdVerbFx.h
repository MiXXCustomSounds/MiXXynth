#pragma once

#include <array>
#include <vector>

#include "FxModule.h"

namespace mixxynth
{
    // High-quality algorithmic reverb.
    //
    //   in -> pre-delay -> input diffusion -> early reflections
    //      -> 8-line FDN tank (frequency-dependent decay, modulated)
    //      -> tone -> width -> duck -> shimmer -> mix -> out
    //
    // The design follows current practice in artificial reverberation rather than
    // the classic Schroeder/Moorer recipe, because that recipe is exactly what
    // makes a reverb sound short, metallic and dated:
    //
    //  * DELAY LENGTHS are mutually prime and logarithmically distributed, so the
    //    impulse response takes as long as possible to repeat and modal density
    //    stays high (Schroeder's rule: 0.15 modes per Hz is enough for a 1 s
    //    tail). Size rescales them and re-quantises to primes, which keeps them
    //    coprime at every size - the trick JOS describes for variable FDNs.
    //
    //  * DECAY IS FREQUENCY DEPENDENT, through a three-band attenuation filter in
    //    every loop (one-pole crossovers at low/mid/high) rather than one
    //    lowpass. Independent low and high decay times are most of what separates
    //    a real room from a boxy one: air absorbs highs while low modes hang on.
    //
    //  * The feedback matrix is a HOUSEHOLDER reflection - orthogonal, so lossless,
    //    and O(N) rather than O(N^2). Losslessness is what makes a true infinite
    //    tail possible with no gain compensation.
    //
    //  * COLORATION - the metallic ringing that makes cheap reverbs cheap - comes
    //    from modal excitation, so the input and output gain vectors are
    //    deliberately non-uniform (one dense input excites strong modes), and the
    //    tank is fed from a six-stage modulated allpass chain that raises echo
    //    density before the tank sees anything.
    //
    //  * Anything that can ring is modulated: tank lines and diffusers both.
    //
    // Beyond the classic controls this adds early reflections (Attack), stereo
    // width, ducking, and a shimmer (octave-up) feedback path.
    class WeirdVerbFx : public FxModule
    {
    public:
        // Indices into the slot's generic parameter block. Must match descriptor().
        enum Param
        {
            pMode = 0,   // plate / room / chamber / hall / spring
            pSize,       // rescales the whole tank
            pDecay,      // mid-band T60, infinite at the top
            pBass,       // low-band T60 relative to mid
            pDamping,    // high-band T60 relative to mid
            pPreDelay,   // 0 - 500 ms
            pAttack,     // early reflections level
            pDiffusion,  // input allpass chain strength
            pModDepth,   // tank and diffuser modulation depth
            pModRate,    // modulation rate
            pLowCut,     // post highpass
            pHighCut,    // post lowpass
            pWidth,      // stereo width of the wet signal
            pDuck,       // wet attenuation while the dry signal is present
            pShimmer,    // octave-up feedback amount
            pMix,        // dry -> wet, 50/50 at 3 o'clock
            pFreeze,     // hold the tail, ignore input
            kNumParams
        };

        static const FxDescriptor& descriptor();

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        // Longest tank line this instance can ever address, in samples. Exposed so
        // a test can check the allocation actually covers the size range.
        int maxLineLength() const noexcept { return longestLine; }

    private:
        static constexpr int numLines = 8;
        static constexpr int numDiffusers = 6;
        static constexpr int numErTaps = 8;

        // Base tank delay lengths in milliseconds at Size 1.0, per mode. Spread
        // logarithmically so the modes never line up. Plate and spring are short
        // and dense; hall is long and sparse.
        static const std::array<float, numLines>& baseLengthsMs (int mode) noexcept;

        // One delay line of the tank.
        struct Line
        {
            std::vector<float> buffer;

            // Fixed stride chosen at prepare for the longest any mode and size can
            // ask for, so changing Size never reallocates on the audio thread.
            int capacity = 0;
            int length = 0;
            int writeIndex = 0;

            // Three-band attenuation filter state, per channel. This IS the
            // frequency-dependent decay: separate loop gains for low, mid, high.
            float lowState[2] { 0.0f, 0.0f };
            float highState[2] { 0.0f, 0.0f };

            float gainLow = 0.0f, gainMid = 0.0f, gainHigh = 0.0f;

            float modPhase = 0.0f;
            float modRate = 0.0f;
            float modDepth = 0.0f;

            // Per-line input and output gains, per channel and deliberately
            // non-uniform: one dense input vector excites the network's modes
            // hard, which is what colours the tail and makes it ring metallically.
            //
            // The two channels use DIFFERENT vectors. With the same excitation on
            // both sides and identical line lengths the tank evolves identically
            // and the reverb comes out mono - correlation exactly 1.0 - which is
            // the single most obvious giveaway of a cheap reverb.
            float inGain[2] { 0.0f, 0.0f };
            float outGain[2] { 0.0f, 0.0f };
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

        // One early reflection, read from the shared ER delay line.
        struct ErTap
        {
            int delaySamples = 0;
            float gain = 0.0f;
            float dampState[2] { 0.0f, 0.0f };
        };

        // Two-head crossfaded pitch shifter, for the shimmer path. One head alone
        // clicks at every wrap, so a second head offset by half a window is
        // crossfaded in as the first fades out.
        struct PitchShifter
        {
            std::vector<float> buffer;
            int capacity = 0;
            int writeIndex = 0;
            float phase = 0.0f;  // 0..1 across the crossfade window
            float ratio = 2.0f;  // 2 = one octave up
            int window = 0;

            void reset();
            float read (int channel, float delaySamples) const;
        };

        // Interpolated read, `delaySamples` behind the write head.
        float readLine (const Line& line, int channel, float delaySamples) const noexcept;

        // Tank lengths are prime-quantised, so they only need recomputing when the
        // mode or the size has actually moved.
        void updateTankLengths (int mode, float sizeScale);

        // The per-sample loop, split out so the per-block setup stays readable.
        void processTail (juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                          float preDelay01, float attack01, float rate01,
                          float lowCut01, float highCut01, float width01, float duck01,
                          float shimmer01, float mix01, bool freeze);

        double sampleRate = 44100.0;
        int channels = 2;
        int longestLine = 0;

        // Headroom reserved in every delay buffer for the modulation excursion.
        int modMargin = 0;

        std::array<Line, numLines> lines;
        std::array<Diffuser, numDiffusers> diffusers;

        std::vector<float> preBuffer;
        int preLength = 0;
        int preWrite = 0;

        std::vector<float> erBuffer;
        int erLength = 0;
        int erWrite = 0;
        std::array<ErTap, numErTaps> erTaps;

        PitchShifter shimmer;

        float lowCutState[2] { 0.0f, 0.0f };
        float highCutState[2] { 0.0f, 0.0f };
        float shimmerReturn[2] { 0.0f, 0.0f };
        float duckEnvelope = 0.0f;

        // Cached so the prime quantisation only runs when Size or Mode moves.
        int lastMode = -1;
        float lastSize = -1.0f;

        bool prepared = false;
    };
}
