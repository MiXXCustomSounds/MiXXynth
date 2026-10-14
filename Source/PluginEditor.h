#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class MiXXynthProcessor;

// M1a: placeholder shell UI (black-metal theme lands with impeccable in M4).
// Min 900x640, default 1440x900 per SPEC S9.
class MiXXynthEditor : public juce::AudioProcessorEditor
{
public:
    explicit MiXXynthEditor (MiXXynthProcessor&);
    ~MiXXynthEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Label title;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthEditor)
};
