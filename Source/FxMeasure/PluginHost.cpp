#include "PluginHost.h"

namespace mixxynth::fxmeasure
{
    namespace
    {
        juce::AudioPluginFormat* findVst3Format (juce::AudioPluginFormatManager& manager)
        {
            for (int i = 0; i < manager.getNumFormats(); ++i)
                if (manager.getFormat (i)->getName() == "VST3")
                    return manager.getFormat (i);

            return nullptr;
        }

        void descriptionsFor (const juce::File& bundle,
                              juce::OwnedArray<juce::PluginDescription>& descriptions,
                              juce::String& error)
        {
            juce::AudioPluginFormatManager manager;
            manager.addDefaultFormats();

            auto* vst3 = findVst3Format (manager);

            if (vst3 == nullptr)
            {
                error = "No VST3 format is available. The target has to be built with "
                        "JUCE_PLUGINHOST_VST3=1.";
                return;
            }

            vst3->findAllTypesForFile (descriptions, bundle.getFullPathName());

            if (descriptions.isEmpty())
                error = "No plugin found in " + bundle.getFullPathName();
        }
    }

    std::vector<PluginHost::ClassInfo> PluginHost::listClasses (const juce::File& bundle,
                                                                juce::String& error)
    {
        std::vector<ClassInfo> out;

        juce::OwnedArray<juce::PluginDescription> descriptions;
        descriptionsFor (bundle, descriptions, error);

        for (int i = 0; i < descriptions.size(); ++i)
        {
            ClassInfo entry;
            entry.index = i;
            entry.name = descriptions[i]->name;
            entry.numInputChannels = descriptions[i]->numInputChannels;
            entry.numOutputChannels = descriptions[i]->numOutputChannels;
            entry.isInstrument = descriptions[i]->isInstrument;
            out.push_back (entry);
        }

        return out;
    }

    bool PluginHost::load (const juce::File& bundle, double newSampleRate, int newBlockSize,
                           int newChannels, const juce::String& classSelector)
    {
        unload();

        sampleRate = newSampleRate;
        blockSize = juce::jmax (16, newBlockSize);
        channels = juce::jmax (1, newChannels);

        juce::OwnedArray<juce::PluginDescription> descriptions;
        juce::String error;
        descriptionsFor (bundle, descriptions, error);

        if (descriptions.isEmpty())
        {
            info.error = error;
            return false;
        }

        int chosen = 0;

        if (classSelector.isNotEmpty())
        {
            // A number selects by index, anything else matches the name. Naming it is the whole
            // point: the class a bundle happens to list first is not necessarily the one being
            // measured, and picking the wrong one looks exactly like a silent plugin.
            if (classSelector.containsOnly ("0123456789"))
            {
                chosen = classSelector.getIntValue();
            }
            else
            {
                chosen = -1;

                for (int i = 0; i < descriptions.size(); ++i)
                {
                    if (descriptions[i]->name.containsIgnoreCase (classSelector))
                    {
                        chosen = i;
                        break;
                    }
                }
            }

            if (chosen < 0 || chosen >= descriptions.size())
            {
                info.error = "No plugin class matching \"" + classSelector + "\" in "
                             + bundle.getFullPathName() + " ("
                             + juce::String (descriptions.size()) + " declared)";
                return false;
            }
        }

        formatManager.addDefaultFormats();

        juce::String createError;
        instance = formatManager.createPluginInstance (*descriptions[chosen], sampleRate,
                                                      blockSize, createError);

        if (instance == nullptr)
        {
            info.error = "Could not create an instance: " + createError;
            return false;
        }

        instance->prepareToPlay (sampleRate, blockSize);

        const auto description = instance->getPluginDescription();

        info.loaded = true;
        info.name = instance->getName();
        info.manufacturer = description.manufacturerName;
        info.version = description.version;
        info.format = description.pluginFormatName;
        info.numParameters = instance->getParameters().size();
        info.numInputChannels = instance->getTotalNumInputChannels();
        info.numOutputChannels = instance->getTotalNumOutputChannels();
        info.reportedLatencySamples = instance->getLatencySamples();
        info.className = description.name;
        info.classIndex = chosen;
        info.numClasses = descriptions.size();
        return true;
    }

    void PluginHost::unload()
    {
        if (instance != nullptr)
            instance->releaseResources();

        instance.reset();
        info = {};
    }

    void PluginHost::process (juce::AudioBuffer<float>& buffer)
    {
        if (instance == nullptr)
            return;

        const int total = buffer.getNumSamples();
        const int bufferChannels = buffer.getNumChannels();

        if (total <= 0 || bufferChannels <= 0)
            return;

        juce::AudioBuffer<float> block (bufferChannels, blockSize);

        for (int start = 0; start < total; start += blockSize)
        {
            const int n = juce::jmin (blockSize, total - start);

            // The plugin was prepared for `blockSize` and is entitled to assume it is never
            // handed more than that, so a long buffer is split rather than passed through.
            block.setSize (bufferChannels, n, false, false, true);

            for (int ch = 0; ch < bufferChannels; ++ch)
                block.copyFrom (ch, 0, buffer, ch, start, n);

            midi.clear();
            instance->processBlock (block, midi);

            for (int ch = 0; ch < bufferChannels; ++ch)
                buffer.copyFrom (ch, start, block, ch, 0, n);
        }
    }
}
