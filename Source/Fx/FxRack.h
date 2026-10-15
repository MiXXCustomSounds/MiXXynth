#pragma once

#include <array>
#include <memory>

#include "FxModule.h"

namespace mixxynth
{
    // The rack: a fixed pool of slots, each holding at most one module instance.
    //
    // Lifetime is the part worth being careful about. An instance is created
    // LAZILY on the message thread the first time a slot uses that type, and is
    // never destroyed while the plugin lives. The audio thread reaches it through
    // an atomic pointer that is written once per (slot, type) with release
    // ordering and read with acquire.
    //
    // That gives three properties at once: no allocation on the audio thread, no
    // lock, and no lifetime race - a pointer the audio thread can see is never
    // freed. Only the slot's *active* pointer changes at runtime, and it only ever
    // points at an instance that already exists.
    class FxRack
    {
    public:
        FxRack() = default;

        // Message thread, and again from prepareToPlay. Prepares every instance
        // that already exists; later ones are prepared as they are created.
        void prepare (double sampleRate, int maxBlockSize, int numChannels);

        // Publishes `type` for the slot, creating the instance if this is the
        // first time the slot has used it. Message thread.
        void setSlotType (int slot, int type);
        int getSlotType (int slot) const;

        // Hands the slot's module its file, creating the instance first if needed.
        // Message thread, and safe to call with an empty name for a slot whose
        // module has no file.
        void setSlotFile (int slot, int type, const juce::String& name,
                          const juce::MemoryBlock& data);

        // True when an instance for (slot, type) already exists, so callers can
        // tell "created on demand" from "already there".
        bool hasInstance (int slot, int type) const;

        // Every slot back to Off. Instances are kept, since a later pick of the
        // same type reuses them.
        void clearAll();

        // Audio thread. `types` and `bypass` are per slot; `params` is
        // kMaxFxSlots * kMaxFxParams normalised values, slot-major.
        void process (juce::AudioBuffer<float>& buffer,
                      const int* types,
                      const bool* bypass,
                      const float* params);

        static FxDescriptor descriptorFor (int type);

        // Number of instances created so far (tests).
        int getNumInstances() const;

    private:
        // Creates and prepares the instance for (slot, type) if it is missing, and
        // returns it. Message thread only.
        FxModule* ensureInstance (int slot, int type);

        double sampleRate = 44100.0;
        int blockSize = 512;
        int channels = 2;

        // Written by the message thread, read by the audio thread. Each element is
        // written only after its instance exists, and never afterwards, so an
        // acquire load either sees nullptr (nothing to run) or a live object.
        std::array<std::atomic<FxModule*>, kMaxFxSlots> active {};

        // The published type per slot, so the editor can read back what the audio
        // thread is actually running without touching the module pointers.
        std::array<std::atomic<int>, kMaxFxSlots> slotTypes {};

        // Owned, slot-major: slot * kNumFxTypes + type. Touched only on the
        // message thread, which is why the audio thread never reads it.
        std::array<std::unique_ptr<FxModule>, kMaxFxSlots * kNumFxTypes> owned;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxRack)
    };
}
