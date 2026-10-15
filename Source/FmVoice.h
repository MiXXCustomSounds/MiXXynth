#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

#include "OpWave.h"

// M2a: mono 6-op FM voice driven by a routing matrix.
//   mod[src][dst]  modulation depth (src modulates dst), 0..1
//   carrier[op]    whether the op goes to the output
//   feedback[op]   per-op self-feedback (smoothed, 1-sample delayed, DX-style)
// Feed-forward edges (src index > dst index, as in every DX7 algorithm) are
// evaluated with zero delay; any other edge falls back to a 1-sample delay,
// which keeps arbitrary free-matrix routing stable.
// Polyphony/unison land in M2b.
class MiXXFmVoice
{
public:
    static constexpr int numOps = 6;

    struct VoiceParams
    {
        float ratio[numOps]   = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        float level[numOps]   = { 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f };
        float attack[numOps];
        float decay[numOps];
        float sustain[numOps];
        float release[numOps];
        float feedback[numOps] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        float mod[numOps][numOps] = {}; // mod[src][dst]
        bool carrier[numOps] = { true, true, true, true, true, true };
        float index    = 3.0f;
        float master   = 0.8f;

        // M4b: per-operator enable and waveform. `wave` is 0..1 sweeping
        // sine -> triangle -> saw -> square (see OpWave.h).
        bool  opOn[numOps]   = { true, true, true, true, true, true };
        float opWave[numOps] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

        // Shared, owned by the processor. Null falls back to a plain sine.
        const mixxynth::OpWaveTables* waveTables = nullptr;

        // M3b: VA layer (blended with the FM stack) and a per-voice filter.
        float vaLevel     = 0.0f;    // 0 = FM only, 1 = VA only
        float vaDetune    = 0.0f;    // cents
        int   vaOctave    = 0;       // -2..+2
        int   vaWave      = 0;       // 0 saw, 1 square, 2 sub, 3 noise
        float vaPulseWidth = 0.5f;   // square duty (0.05..0.95)

        bool  filterOn    = false;
        int   filterType  = 0;       // 0 LP, 1 HP, 2 BP
        float cutoff      = 12000.0f;
        float resonance   = 0.2f;
        float drive       = 0.0f;

        VoiceParams()
        {
            for (int i = 0; i < numOps; ++i)
            {
                attack[i]  = 0.01f;
                decay[i]   = 0.10f;
                sustain[i] = 0.90f;
                release[i] = 0.20f;
            }
        }
    };

    MiXXFmVoice();

    void setSampleRate (double sampleRate);

    // M2b: per-voice pitch offset (cents) and stereo placement, set by the
    // voice manager for unison stacks.
    void setDetune (float cents);
    void setPan (float pan); // -1 left .. +1 right

    // `phaseOffset01` shifts every operator's start phase, which is how unison
    // voices (and repeated notes) are decorrelated from one another. 0 aligns
    // them all, 1 starts anywhere in the cycle. All operators of a voice get the
    // SAME offset, so the FM relationships inside a voice are preserved and only
    // the voices differ from each other.
    void noteOn (int noteNumber, float velocity, float phaseOffset01 = 0.0f);
    void noteOff (int noteNumber);
    void reset();

    // Renders this voice and ADDS into the buffer range (polyphonic mixing).
    void renderAdd (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& params);

    // Mono convenience path: clears, applies MIDI, renders. Used by tests.
    void renderBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const VoiceParams& params);

    bool isActive() const;
    int getCurrentNote() const { return currentNote; }
    float getVelocity() const { return velocity; }

private:
    struct Op
    {
        double phase = 0.0;
        float env = 0.0f;
        int stage = 0; // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
        float releaseFrom = 0.0f;
        float prevOut = 0.0f; // previous-sample output (feedback + backward edges)
        float curOut = 0.0f;  // current-sample output
        float fbSmooth = 0.0f; // smoothed feedback amount (zipper-free)
    };

    void renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& params);
    static float updateEnvelope (Op& op, float a, float d, float s, float r, double sampleRate);

    std::array<Op, numOps> ops;

    // M3b: VA oscillator and filter state (per voice).
    double vaPhase = 0.0;
    double vaSubPhase = 0.0;
    // Gate-driven AR envelope for the VA oscillator. Without it the VA was an
    // envelopeless tone that ran for as long as the voice existed.
    float vaEnv = 0.0f;
    juce::uint32 noiseState = 0x2545f491u;
    float svfIc1 = 0.0f;
    float svfIc2 = 0.0f;

    // Cached TPT coefficients. The filter is on by default, so recomputing
    // tan() per sample per voice was the hot path; it now runs once per change.
    float svfA1 = 0.0f, svfA2 = 0.0f, svfA3 = 0.0f, svfK = 2.0f;
    float svfLastCutoff = -1.0f, svfLastReso = -1.0f;

    double sampleRate = 44100.0;
    double frequency = 440.0;
    float velocity = 0.0f;
    float detuneCents = 0.0f;
    float panL = 1.0f;
    float panR = 1.0f;
    int currentNote = -1;
    bool gate = false;
};
