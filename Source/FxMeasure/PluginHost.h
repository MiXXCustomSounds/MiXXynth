#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

namespace mixxynth::fxmeasure
{
    // Loads a VST3 by absolute path and renders audio through it, offline.
    //
    // Black box by construction: this can feed audio in and read audio out, and nothing else.
    // It never reads the plugin's binary, its memory or its code, which is the line that makes
    // measuring a commercial plugin legitimate. See docs/FX-MEASUREMENT.md.
    class PluginHost
    {
    public:
        struct Info
        {
            bool loaded = false;
            juce::String error;
            juce::String name;
            juce::String manufacturer;
            juce::String version;
            juce::String format;
            int numParameters = 0;
            int numInputChannels = 0;
            int numOutputChannels = 0;
            int reportedLatencySamples = 0;
            juce::String className;
            int classIndex = 0;
            int numClasses = 0;
        };

        // One plugin class inside a bundle.
        //
        // A VST3 bundle can declare SEVERAL classes, and they are not interchangeable: Serum 2
        // declares both an instrument and an effect from the same binary, with different audio
        // buses and different parameters. Taking the first one the format happens to report is
        // therefore a coin toss, and getting it wrong looks exactly like the plugin being
        // silent - which is what it did here.
        struct ClassInfo
        {
            int index = 0;
            juce::String name;
            int numInputChannels = 0;
            int numOutputChannels = 0;
            bool isInstrument = false;
        };

        // Every class the bundle declares, in the order the format reports them. Needs no
        // instance, so it is safe to call before choosing one.
        static std::vector<ClassInfo> listClasses (const juce::File& bundle, juce::String& error);

        // Declared rather than left implicit, and not for style: JUCE_DECLARE_NON_COPYABLE
        // below declares a deleted copy constructor, and a user-declared constructor suppresses
        // the implicit default one. Without this line the class cannot be constructed at all.
        // The same note appears on every FxModule in this project, for the same reason.
        PluginHost() = default;

        // Loads a plugin from `bundle` and prepares it.
        //
        // `classSelector` picks which class, because a bundle may declare more than one: a plain
        // number selects by index, anything else matches the class name case-insensitively, and
        // empty selects the first. Returns false and fills in getInfo().error rather than
        // throwing: a bundle that will not load is an ordinary outcome when measuring what
        // happens to be installed, not an exceptional one.
        bool load (const juce::File& bundle, double sampleRate, int blockSize, int channels,
                   const juce::String& classSelector = {});
        void unload();

        // `getInfo` rather than `info`, because the member is called `info` and a method of the
        // same name shadows it - a compile error rather than a subtlety.
        const Info& getInfo() const noexcept { return info; }

        // The live instance, for reading parameter names and setting values. Null until load()
        // has succeeded.
        juce::AudioProcessor* processor() const noexcept { return instance.get(); }

        // Renders `buffer` in place, split into blocks the plugin was prepared for.
        void process (juce::AudioBuffer<float>& buffer);

    private:
        juce::AudioPluginFormatManager formatManager;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        Info info;
        double sampleRate = 44100.0;
        int blockSize = 512;
        int channels = 2;
        juce::MidiBuffer midi;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
    };
}
