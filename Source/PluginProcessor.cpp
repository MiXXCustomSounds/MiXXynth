#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout MiXXynthProcessor::createLayout()
{
    // M1a: shell params only. Full FM/VA/filter/mod/FX/randomize params land in M1b-M4.
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "masterGain", "Master Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.8f));
    return { params.begin(), params.end() };
}

MiXXynthProcessor::MiXXynthProcessor()
    : AudioProcessor (BusesProperties()
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MiXXynthState", createLayout())
{
}

MiXXynthProcessor::~MiXXynthProcessor() = default;

void MiXXynthProcessor::prepareToPlay (double, int) {}
void MiXXynthProcessor::releaseResources() {}

void MiXXynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    // M1a: silence (shell proves hosting + automation + state). M1b adds first FM voice.
    buffer.clear();
}

juce::AudioProcessorEditor* MiXXynthProcessor::createEditor()
{
    return new MiXXynthEditor (*this);
}

void MiXXynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MiXXynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiXXynthProcessor();
}
