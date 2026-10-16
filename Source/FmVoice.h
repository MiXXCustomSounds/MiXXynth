#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

#include "OpWave.h"

// One 6-op FM voice with a routing matrix, a VA layer and a per-voice filter.
//   mod[src][dst]  modulation depth (src modulates dst), 0..1
//   carrier[op]    whether the op goes to the output
//   feedback[op]   per-op self-feedback (smoothed, 1-sample delayed, DX-style)
// Feed-forward edges (src index > dst index, as in every DX7 algorithm) are
// evaluated with zero delay; any other edge falls back to a 1-sample delay,
// which keeps arbitrary free-matrix routing stable.
//
// This class is a single voice. Polyphony, unison, pan spread and voice stealing
// live in VoiceManager, which owns a pool of these and calls renderAdd on each.
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
        // sine -> triangle -> saw -> square -> pulse 25% -> 12% -> 6%, so the top of
        // the sweep is a pulse width control rather than a further shape (see OpWave.h).
        bool  opOn[numOps]   = { true, true, true, true, true, true };
        float opWave[numOps] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

        // Shared, owned by the processor. Null falls back to a plain sine.
        const mixxynth::OpWaveTables* waveTables = nullptr;

        // M3b: VA layer (blended with the FM stack) and a per-voice filter.
        float vaLevel     = 0.0f;    // 0 = FM only, 1 = VA only
        float vaDetune    = 0.0f;    // cents
        int   vaOctave    = 0;       // -2..+2

        // The same 0..1 morph the operators use, through the same shared tables. Past
        // the square it narrows the duty, so one control is the shape selector and the
        // pulse width control at once. Defaults to saw, which is where the old
        // four-entry shape list started.
        float vaShape     = 1.0f / 3.0f;

        // Their own levels rather than entries in a shape list, so they can be blended
        // with any shape instead of replacing it.
        float vaSubLevel  = 0.0f;    // square, one octave below
        float vaNoiseLevel = 0.0f;   // white noise

        // The VA's own ADSR. It is driven through the same updateEnvelope the operators
        // use, so the two cannot disagree about what a stage does. It was a hardwired
        // one-pole gate before.
        float vaAttack  = 0.008f;
        float vaDecay   = 0.0f;
        float vaSustain = 1.0f;
        float vaRelease = 0.05f;

        // M6: a filter per operator, each with its own envelope and its own routing choice.
        //
        // Per operator rather than one global filter because filtering a MODULATOR and
        // filtering a CARRIER are different sounds, and the whole point of a per-operator
        // filter is being able to do both at once. The routing below decides which of the
        // two paths each operator's filter sits on.
        bool  opFilterOn[numOps]     = {};
        int   opFilterType[numOps]   = {};   // 0 LP, 1 HP, 2 BP
        float opFilterCutoff[numOps] = {};
        float opFilterReso[numOps]   = {};
        float opFilterDrive[numOps]  = {};

        // The filter's OWN envelope, which sweeps its cutoff. It is a separate envelope
        // rather than the amplitude one, because a filter that could only sweep in step
        // with the level could not do the one thing filter envelopes are for.
        float opFilterAttack[numOps]  = {};
        float opFilterDecay[numOps]   = {};
        float opFilterSustain[numOps] = {};
        float opFilterRelease[numOps] = {};

        // Where the filter sits: 0 = the operator's output, 1 = its modulation, 2 = both.
        // Defaults to Both, so switching a filter on does the obvious thing.
        int opFilterRouting[numOps] = { 2, 2, 2, 2, 2, 2 };

        // The one global filter, which the VA panel drives. Separate from the per-operator
        // filters above and staying where it was: it sits across the whole voice rather than
        // inside one operator's signal path.
        bool  filterOn    = false;
        int   filterType  = 0;       // 0 LP, 1 HP, 2 BP
        float cutoff      = 12000.0f;
        float resonance   = 0.2f;
        float drive       = 0.0f;

        // M6: analog-style drift, and velocity response.
        //
        // Drift is a slow random walk per operator, so the *relationships* between
        // operators wander the way analog oscillators do. It is deliberately not the
        // same thing as unison detune: detune is a fixed offset that separates voices,
        // while drift is movement within a single voice over time - which is why it is
        // audible at unison = 1, where detune does nothing at all.
        float drift       = 0.0f;    // 0..1, scales the walk's range
        float velToLevel  = 0.0f;    // 0..1, how much velocity scales the output
        float velToIndex  = 0.0f;    // 0..1, how much velocity scales the FM index

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

    // Gives this voice its own drift trajectory. The voice manager seeds each slot
    // differently so a unison stack's voices wander apart; the same seed always gives
    // the same walk, which is what makes drift testable at all.
    void setDriftSeed (juce::uint32 seed) { driftSeed = seed != 0u ? seed : 1u; }

    // `phaseOffset01` shifts every operator's start phase, which is how unison
    // voices (and repeated notes) are decorrelated from one another. 0 aligns
    // them all, 1 starts anywhere in the cycle. All operators of a voice get the
    // SAME offset, so the FM relationships inside a voice are preserved and only
    // the voices differ from each other.
    void noteOn (int noteNumber, float vel, float phaseOffset01 = 0.0f);
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
    // One envelope's state. Its own type rather than a few fields on Op, because an
    // operator now runs TWO envelopes - the amplitude one and its filter's - and a stage
    // machine should exist once rather than twice. Two copies drift; one cannot.
    struct EnvState
    {
        float env = 0.0f;
        int stage = 0; // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
        float releaseFrom = 0.0f;
    };

    struct Op
    {
        double phase = 0.0;
        EnvState envState;      // the amplitude envelope
        float prevOut = 0.0f;   // previous-sample output (feedback + backward edges)
        float curOut = 0.0f;    // current-sample output

        // Drift: where this operator's pitch wander currently is, and where it is
        // heading. Two values rather than one, because the walk moves the target and
        // a slew moves the current value toward it - which is what makes the pitch
        // wander instead of stepping.
        float drift = 0.0f;
        float driftTarget = 0.0f;
        float fbSmooth = 0.0f; // smoothed feedback amount (zipper-free)

        // The operator's filter: its own SVF state, its own cached coefficients and its own
        // envelope. The envelope is separate from the amplitude one because a filter that
        // could only sweep in step with the level could not do the one thing filter
        // envelopes are for.
        EnvState fltEnvState;
        float fltIc1 = 0.0f, fltIc2 = 0.0f;
        float fltA1 = 0.0f, fltA2 = 0.0f, fltA3 = 0.0f, fltK = 2.0f;
        float fltLastCutoff = -1.0f, fltLastReso = -1.0f;
    };

    void renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& params);
    static float updateEnvelope (EnvState& e, float a, float d, float s, float r, double sampleRate);

    // Drift. `updateDrift` advances every operator's walk and the VA's by one block;
    // `nextDriftRandom` is the deterministic source the walk is built from.
    void updateDrift (const VoiceParams& params, int numSamples);
    float nextDriftRandom();

    std::array<Op, numOps> ops;

    // M3b: VA oscillator and filter state (per voice).
    double vaPhase = 0.0;
    double vaSubPhase = 0.0;
    // The VA's own ADSR state. It reuses the operator Op and updateEnvelope so the two
    // cannot disagree about what a stage does; only the envelope fields are used.
    //
    // This replaced a one-pole gate follower, which had a fixed attack and release and no
    // sustain control. Stage transitions are now driven by the gate edge instead.
    EnvState vaEnvState;
    bool vaGateLast = false;
    float vaEnv = 0.0f;

    // True once the VA layer has actually contributed to the output this note. Set
    // while rendering, read by isActive() - which is why it is a member and not a
    // local: the question "is this voice finished" is asked between blocks, when no
    // parameters are in scope.
    bool vaWasAudible = false;

    // The three VA layer levels arrive once per block, straight from the parameters, so a knob
    // move or a modulation step lands as a jump. On the morph oscillator a jump reads as a
    // level change; on the sub, which is a square, it reads as a CLICK, because a square whose
    // amplitude steps is a discontinuity and the square has no ramp to hide it in. These are
    // the per-sample values the mix actually uses, slewed towards the block's targets.
    float vaLevelSm = 0.0f;
    float vaSubLevelSm = 0.0f;
    float vaNoiseLevelSm = 0.0f;
    juce::uint32 noiseState = 0x2545f491u;

    // Drift state for the VA, kept separate from the operators so the VA wanders on
    // its own trajectory rather than tracking a single operator.
    float vaDrift = 0.0f;
    float vaDriftTarget = 0.0f;

    // Each voice needs its own drift walk. Seeded per slot by the voice manager,
    // because a unison stack sharing one trajectory would drift as a single voice
    // and the movement would collapse back into a fixed detune - which is exactly
    // what drift is supposed to not be.
    juce::uint32 driftSeed = 0x9e3779b9u;

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
