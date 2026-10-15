#pragma once

#include <atomic>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include "FxModule.h"

namespace mixxynth
{
    // Convolution reverb.
    //
    //   in -> pre-delay -> [partitioned FFT convolution] -> low cut -> high cut
    //      -> width -> mix -> out
    //
    // The convolution itself is juce::dsp::Convolution (MIT, ships with JUCE). It
    // already does partitioned FFT convolution with a background loader thread,
    // which is the real problem to solve here; hand-rolling it would mean solving
    // the same threading problem again with more ways to get it wrong.
    //
    // THE IMPULSE RESPONSES ARE GENERATED, NOT SHIPPED. An IR downloaded from the
    // internet is exactly the kind of asset whose provenance code review cannot
    // check - CC0, CC-BY and CC-BY-NC all mean different things, and one CC-BY-NC
    // texture would stop this plugin being MIT. Generating them instead costs a
    // few dozen lines, ships no binary assets, cannot go missing, and gives a
    // usable starter set. A user can still load their own WAV or AIFF, and that
    // file's BYTES are carried in the preset so the patch stays portable.
    //
    // Because generation must happen on the message thread, the module watches its
    // own parameters: process() records what was asked for in atomics, and a timer
    // notices a change and rebuilds. That keeps the audio thread free of both
    // allocation and locks.
    class ConvolutionFx : public FxModule,
                          private juce::AsyncUpdater
    {
    public:
        enum Param
        {
            pIr = 0,      // built-in impulse response, or the user's file
            pLength,      // tail trim, as a fraction of the IR
            pPreDelay,
            pLowCut,
            pHighCut,
            pWidth,
            pMix,
            pNormalise,   // choice: Off / On
            kNumParams
        };

        static const FxDescriptor& descriptor();

        // The built-in impulse responses, by name. Shared with the UI, which is why
        // they are exposed rather than private.
        static int getNumBuiltInIrs() noexcept;
        static const char* getBuiltInIrName (int index) noexcept;

        // The value of `pIr` that means "the file the user loaded" rather than one
        // of the built-ins.
        static int customIrChoice() noexcept { return getNumBuiltInIrs(); }

        ConvolutionFx();
        ~ConvolutionFx() override;

        void prepare (double sampleRate, int maxBlockSize, int numChannels) override;
        void reset() override;
        void process (juce::AudioBuffer<float>& buffer, const float* params) override;

        void setFile (const juce::String& name, const juce::MemoryBlock& data) override;

        // Rebuilds the impulse if what the audio thread last asked for differs from
        // what is loaded. The timer calls this; it is public so a caller without a
        // running message loop - an offline render, or a test - can drive it.
        //
        // juce::dsp::Convolution loads asynchronously, so a caller that needs the new
        // impulse active immediately must follow this with prepare(), which is what
        // JUCE documents as the way to force the load to complete.
        void refreshImpulse();

        // What the module is currently loaded with, for the audit and the UI.
        juce::String getLoadedIrName() const;

    private:
        // One synthesised impulse response.
        struct IrSpec
        {
            const char* name;
            float lengthSeconds;
            float decaySeconds;
            float hfDamping;   // 0 = highs ring as long as lows, 1 = they die first
            float diffusion;   // how gradually the tail builds
            float erLevel;     // early reflection amount
            float erSpreadMs;
            float brightness;  // tilt of the noise
        };

        static const IrSpec& specFor (int index) noexcept;

        // Builds the impulse response for `index`, or decodes `fileData` when the
        // slot holds a user file. Message thread.
        bool buildImpulse (int choice, const juce::MemoryBlock& fileData,
                           juce::AudioBuffer<float>& out) const;

        // Applies the tail trim and hands the result to the convolver. Message thread.
        void reloadImpulse();

        // Message thread, when the audio thread has asked for a different impulse.
        void handleAsyncUpdate() override;

        juce::dsp::Convolution convolution;

        // The untrimmed impulse, so changing Length does not mean decoding again.
        juce::AudioBuffer<float> rawIr;
        juce::String loadedName;

        // What the audio thread last asked for. The timer compares these against
        // what is actually loaded and rebuilds when they differ.
        std::atomic<int> requestedIr { 0 };
        std::atomic<int> requestedLength { 16 };
        std::atomic<int> requestedNormalise { 1 };

        int loadedIr = -1;
        int loadedLength = -1;
        int loadedNormalise = -1;

        juce::String fileName;
        juce::MemoryBlock fileData;

        // Pre-delay.
        std::vector<float> preBuffer;
        int preLength = 0;
        int preWrite = 0;

        // Post filters and stereo state.
        float lowCutState[2] { 0.0f, 0.0f };
        float highCutState[2] { 0.0f, 0.0f };

        // A wet buffer, so the convolution can run in place on the dry signal
        // without a second allocation per block.
        juce::AudioBuffer<float> wetBuffer;

        double sampleRate = 44100.0;
        int blockSize = 512;
        int channels = 2;

        bool prepared = false;
        bool reloadPending = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConvolutionFx)
    };
}
