#pragma once

#include <array>
#include <vector>

#include "FxModule.h"

namespace mixxynth
{
    // blueSky-style reverb, as the rack's first module.
    //
    //   in -> pre-delay -> input diffusion -> 8-line FDN -> tone -> mix -> out
    //
    // The tank is a feedback delay network: eight delay lines whose outputs are
    // combined through a Householder matrix. That matrix is a reflection, so it is
    // ORTHOGONAL and therefore energy preserving - which is what makes a genuine
    // infinite tail possible: at maximum Decay the feedback gain is exactly 1.0
    // and the tail sustains instead of decaying, with no gain compensation needed.
    // A DC blocker and a hard clamp sit in the loop so it cannot run away even if
    // a state is corrupted.
    //
    // The three modes are the pedal's plate / room / spring. They differ in delay
    // length set, diffusion, damping curve and modulation character rather than in
    // topology, which is how a single tank covers all three convincingly.
    class ReverbFx : public FxModule
    {
    public:
        // Indices into the slot's generic parameter block. Must match the
        // descriptor in descriptor().
        enum Param
        {
            pMode = 0,   // plate / room / spring
            pDecay,      // short -> infinite
            pPreDelay,   // 0 - 250 ms
            pTone,       // damped -> open
            pMod,        // off -> deep
            pMix,        // dry -> wet, 50/50 at 3 o'clock
            kNumParams
        };

        static const FxDescriptor& descriptor();

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        // Longest line plus modulation excursion, in samples, for the current
        // sample rate. Exposed for the smoke test's allocation check.
        int maxLineLength() const noexcept { return longestLine; }

    private:
        static constexpr int numLines = 8;
        static constexpr int numDiffusers = 2;

        // Line length tables, in samples at 48 kHz. Co-prime-ish so the modes do
        // not share resonances.
        static const std::array<int, numLines>& baseLengths (int mode) noexcept;

        float decaySecondsFor (float decay01) const noexcept;
        float toneCoefficient (float tone01) const noexcept;

        // One delay line per tank line, per channel.
        struct Line
        {
            std::vector<float> buffer;

            // Fixed stride, chosen at prepare for the longest line any mode can
            // ask for. `length` changes with the mode, but the allocation and the
            // channel stride do not, so switching modes never reallocates on the
            // audio thread.
            int capacity = 0;
            int length = 0;
            int writeIndex = 0;

            // Tone is a one-pole lowpass inside the feedback path.
            float dampState[2] { 0.0f, 0.0f };

            // Slow, decorrelated per-line modulation; a static long tail rings
            // metallically without it.
            float modPhase = 0.0f;
            float modRate = 0.0f;
            float modDepth = 0.0f;

            float feedback = 0.0f;

            // DC blocker state, so a long tail cannot accumulate an offset.
            float dcX[2] { 0.0f, 0.0f };
            float dcY[2] { 0.0f, 0.0f };
        };

        // Interpolated read, `delaySamples` behind the write head.
        float readLine (const Line& line, int channel, float delaySamples) const noexcept;

        // The per-sample loop, split out so the per-block setup stays readable.
        void processSamples (juce::AudioBuffer<float>& buffer, int numCh, int numSamples,
                             float dampA, int preDelaySamples, float inputGain,
                             float wet, float dry);

        double sampleRate = 44100.0;
        int channels = 2;
        int longestLine = 0;

        std::array<Line, numLines> lines;

        // Pre-delay, ahead of the diffusion stage.
        std::vector<float> preBuffer;
        int preLength = 0;
        int preWrite = 0;

        // Input diffusion: two allpass stages, which smear the input into
        // something the tank can turn into a smooth tail.
        struct Allpass
        {
            std::vector<float> buffer;
            int length = 0;
            int writeIndex = 0;
            float gain = 0.5f;
        };

        std::array<Allpass, numDiffusers> diffusers;

        bool prepared = false;
    };
}
