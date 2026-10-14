#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout MiXXynthProcessor::createLayout()
{
    // M1b: per-op ratio/level/ADSR x6 + index + feedback(stub) + master.
    // 6*6 + 3 = 39 params. All automatable, all in DAW state.
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const juce::String n = juce::String (i + 1);
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "ratio", "Op " + n + " Ratio",
            juce::NormalisableRange<float> (0.25f, 16.0f, 0.001f, 0.4f), 1.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "level", "Op " + n + " Level",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
            (i < 2 ? 0.8f : 0.12f)));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "attack", "Op " + n + " Attack",
            juce::NormalisableRange<float> (0.001f, 2.0f, 0.001f, 0.4f), 0.01f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "decay", "Op " + n + " Decay",
            juce::NormalisableRange<float> (0.005f, 4.0f, 0.001f, 0.4f), 0.10f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "sustain", "Op " + n + " Sustain",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.90f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "release", "Op " + n + " Release",
            juce::NormalisableRange<float> (0.005f, 8.0f, 0.001f, 0.4f), 0.20f));
    }

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "fmIndex", "FM Index",
        juce::NormalisableRange<float> (0.0f, 12.0f, 0.001f), 3.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "fmFeedback", "FM Feedback (M1b-ii)",
        juce::NormalisableRange<float> (0.0f, 0.95f, 0.001f), 0.0f));
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

void MiXXynthProcessor::prepareToPlay (double sampleRate, int)
{
    voice.setSampleRate (sampleRate);
}

void MiXXynthProcessor::releaseResources() {}

static float getParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
{
    if (auto* p = apvts.getRawParameterValue (id))
        return p->load();
    return 0.0f;
}

void MiXXynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    MiXXFmVoice::VoiceParams vp;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const juce::String n = juce::String (i + 1);
        vp.ratio[i]   = getParam (apvts, "op" + n + "ratio");
        vp.level[i]   = getParam (apvts, "op" + n + "level");
        vp.attack[i]  = getParam (apvts, "op" + n + "attack");
        vp.decay[i]   = getParam (apvts, "op" + n + "decay");
        vp.sustain[i] = getParam (apvts, "op" + n + "sustain");
        vp.release[i] = getParam (apvts, "op" + n + "release");
    }

    vp.index    = getParam (apvts, "fmIndex");
    vp.feedback = getParam (apvts, "fmFeedback"); // stubbed in voice until M1b-ii
    vp.master   = getParam (apvts, "masterGain");

    voice.renderBlock (buffer, midi, vp);
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
