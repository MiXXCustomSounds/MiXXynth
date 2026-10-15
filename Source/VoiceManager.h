#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

#include "FmVoice.h"
#include "ModMatrix.h"

// M2b/M3a: polyphonic voice allocation with unison stacks, plus per-voice
// modulation.
//
// Voice pool is a fixed compile-time size. A note allocates `unison` voices
// (detuned + stereo-spread); the pool is shared across all held notes, so
// polyphony x unison is capped by the pool and excess notes steal the oldest.
// Stealing is oldest-note-first, which is the standard "oldest" policy.
//
// Modulation is evaluated per voice in short sub-blocks (see modBlockSize) so
// LFOs and mod envelopes stay smooth instead of stepping once per host block.
class MiXXVoiceManager
{
public:
    static constexpr int maxVoices = 64;
    static constexpr int modBlockSize = 32;

    struct PolyParams
    {
        int polyphony      = 16;  // max simultaneous notes (1..32)
        int unison         = 1;   // voices per note (1..16)
        float unisonDetune = 10.0f; // total detune spread in cents
        float unisonSpread = 0.5f;  // stereo spread of the unison stack (0..1)
    };

    void prepare (double sampleRate);
    void reset();

    void renderBlock (juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midi,
                      const MiXXFmVoice::VoiceParams& params,
                      const mixxynth::ModParams& mod,
                      const PolyParams& poly);

    // MPE entry points. `channel` identifies the note's MPE member channel.
    void startNoteOnChannel (int channel, int note, float velocity,
                             const mixxynth::ModParams& mod, const PolyParams& poly);
    void releaseNoteOnChannel (int channel, int note, const mixxynth::ModParams& mod);
    void setNotePitchBend (int channel, int note, float semitones);
    void setNotePressure (int channel, int note, float value);
    void setNoteTimbre (int channel, int note, float value);

    int getActiveNoteCount() const;
    int getActiveVoiceCount() const;

    // Copies the shape phases of the newest active voice so the editor can draw
    // playheads. Called from the audio thread; -1 means "no voice to show".
    void publishShapePhases (std::array<float, mixxynth::kNumShapes>& out) const;

    // Global pitch offset (semitones) applied on top of each voice's unison
    // detune. MPE will extend this to per-note offsets.
    void setPitchBendSemitones (float semitones);

private:
    struct Slot
    {
        MiXXFmVoice voice;
        mixxynth::ModState modState;
        int note = -1;
        int channel = 1;           // MPE gives each note its own channel
        juce::uint64 order = 0;
        bool held = false;
        float unisonCents = 0.0f;  // this voice's position in the unison stack
        float noteBendSemis = 0.0f; // per-note MPE pitch bend
        float pressure = 0.0f;      // per-note MPE pressure (Z)
        float timbre = 0.0f;        // per-note MPE slide (Y)
    };

    void startNote (int channel, int note, float velocity,
                    const mixxynth::ModParams& mod, const PolyParams& poly);
    void releaseNote (int channel, int note, const mixxynth::ModParams& mod);
    void applyDetune (Slot& s);
    void renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                      const MiXXFmVoice::VoiceParams& params, const mixxynth::ModParams& mod);
    int countHeldNotes() const;

    std::array<Slot, maxVoices> slots;
    double sampleRate = 44100.0;
    juce::uint64 counter = 0;
    float mixGain = 1.0f; // smoothed voice-count compensation
    float pitchBendSemitones = 0.0f;
};
