#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "FmVoice.h"

// M1b: mono 6-op FM voice. Op1 -> Op2 phase-mod pair + Ops3-6 spare carriers.
// All 6 ops mixed to output so every per-op param is provably live.
// Polyphony, unison, algorithm matrix, feedback, VA, filter, mod, FX land in M2-M4.
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

    MiXXFmVoice voice;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthProcessor)
};
