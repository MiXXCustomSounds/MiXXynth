#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

#include "FmVoice.h"

// M2b: polyphonic voice allocation with unison stacks.
//
// Voice pool is a fixed compile-time size. A note allocates `unison` voices
// (detuned + stereo-spread); the pool is shared across all held notes, so
// polyphony x unison is capped by the pool and excess notes steal the oldest.
// Stealing is oldest-note-first, which is the standard "oldest" policy.
class MiXXVoiceManager
{
public:
    static constexpr int maxVoices = 64;

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
                      const PolyParams& poly);

    int getActiveNoteCount() const;
    int getActiveVoiceCount() const;

private:
    struct Slot
    {
        MiXXFmVoice voice;
        int note = -1;
        juce::uint64 order = 0;
        bool held = false;
    };

    void startNote (int note, float velocity, const PolyParams& poly);
    void releaseNote (int note);
    void renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                      const MiXXFmVoice::VoiceParams& params);
    int countHeldNotes() const;

    std::array<Slot, maxVoices> slots;
    double sampleRate = 44100.0;
    juce::uint64 counter = 0;
    float mixGain = 1.0f; // smoothed voice-count compensation
};
