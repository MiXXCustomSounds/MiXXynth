#include "PluginEditor.h"
#include "PluginProcessor.h"

MiXXynthEditor::MiXXynthEditor (MiXXynthProcessor& p)
    : AudioProcessorEditor (p)
{
    setResizable (true, true);
    setResizeLimits (900, 640, 2560, 1440);
    setSize (1440, 900);

    title.setText ("MiXXynth - M2a (32 DX7 algorithms + free 6x6 matrix)", juce::dontSendNotification);
    title.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (title);
}

MiXXynthEditor::~MiXXynthEditor() = default;

void MiXXynthEditor::paint (juce::Graphics& g)
{
    // M1a placeholder. S9 black-metal + dark-oak + glow theme lands in M4 with impeccable.
    g.fillAll (juce::Colour (0xff0b0d10));
    g.setColour (juce::Colour (0xff35e0ff));
    g.setFont (28.0f);
    g.drawFittedText ("MiXXynth", getLocalBounds().withHeight (120), juce::Justification::centred, 1);
}

void MiXXynthEditor::resized()
{
    title.setBounds (getLocalBounds().withHeight (200));
}
