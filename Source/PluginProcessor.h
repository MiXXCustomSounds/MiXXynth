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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    MiXXVoiceManager voiceManager;
    float currentSampleRate = 44100.0f;
    float modWheel = 0.0f;
    float aftertouch = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthProcessor)
};
