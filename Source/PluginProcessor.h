#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "FmVoice.h"
#include "VoiceManager.h"
#include "ModMatrix.h"

// M2b/M3a: polyphonic 6-op FM synth with per-voice modulation.
// 32 simultaneous notes, unison stacks up to 16, 64-voice pool, and a 32-slot
// drag-and-drop modulation matrix (4 LFOs, 3 mod envelopes, MSEG-lite, 4 macros,
// velocity/mod-wheel/aftertouch/key-track).
// VA/filter, FX tail, presets and randomize land in M3b/M4.
class MiXXynthProcessor : public juce::AudioProcessor
{
public:
    MiXXynthProcessor();
    ~MiXXynthProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // Declares MPE capability to the host. This must be a constant (JUCE calls it
    // on the audio thread and expects the same answer every time), so it always
    // reports true; the "MPE Enabled" parameter decides how we interpret MIDI.
    bool supportsMPE() const override { return true; }

    const juce::String getName() const override { return "MiXXynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Init"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Shared with the editor's performance bar so the on-screen keyboard and
    // wheels can show (and drive) live MIDI state.
    juce::MidiKeyboardState keyboardState;
    float getModWheelValue() const { return modWheel; }
    float getAftertouchValue() const { return aftertouch; }
    float getPitchBendValue() const { return pitchBend; }
    void setPitchBendFromUI (float v) { pitchBend = juce::jlimit (-1.0f, 1.0f, v); }
    void setModWheelFromUI (float v) { modWheel = juce::jlimit (0.0f, 1.0f, v); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Routes per-note MPE expression into the voice manager.
    class MpeHandler : public juce::MPEInstrument::Listener
    {
    public:
        explicit MpeHandler (MiXXynthProcessor& p) : proc (p) {}

        void noteAdded (juce::MPENote note) override;
        void noteReleased (juce::MPENote note) override;
        void notePressureChanged (juce::MPENote note) override;
        void notePitchbendChanged (juce::MPENote note) override;
        void noteTimbreChanged (juce::MPENote note) override;
        void noteKeyStateChanged (juce::MPENote) override {}

    private:
        MiXXynthProcessor& proc;
    };

    MiXXVoiceManager voiceManager;
    juce::MPEInstrument mpeInstrument;
    MpeHandler mpeHandler { *this };
    int appliedMemberChannels = -1; // last zone layout pushed to the instrument

    // Snapshot of the current block's parameters, so the MPE callbacks can reach
    // the modulation and polyphony settings.
    mixxynth::ModParams currentMod;
    MiXXVoiceManager::PolyParams currentPoly;
    bool mpeActive = false;

    float currentSampleRate = 44100.0f;
    float modWheel = 0.0f;
    float aftertouch = 0.0f;
    float pitchBend = 0.0f; // -1..+1, from the host or the on-screen wheel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthProcessor)
};
