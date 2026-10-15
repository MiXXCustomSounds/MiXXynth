#include "FxRack.h"

#include "ReverbFx.h"

namespace mixxynth
{
    const char* fxTypeName (int type) noexcept
    {
        switch (type)
        {
            case fxOff:    return "Off";
            case fxReverb: return "Reverb";
            default:       break;
        }

        return "Off";
    }

    FxDescriptor FxRack::descriptorFor (int type)
    {
        switch (type)
        {
            case fxReverb: return ReverbFx::descriptor();
            default:       break;
        }

        return FxDescriptor {};
    }

    void FxRack::prepare (double newSampleRate, int newBlockSize, int newChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        blockSize = juce::jmax (1, newBlockSize);
        channels = juce::jmax (1, newChannels);

        // Only touches instances that already exist. Ones created later are
        // prepared as they are created, on the message thread.
        for (auto& module : owned)
            if (module != nullptr)
                module->prepare (sampleRate, blockSize, channels);
    }

    FxModule* FxRack::ensureInstance (int slot, int type)
    {
        if (slot < 0 || slot >= kMaxFxSlots || type <= fxOff || type >= kNumFxTypes)
            return nullptr;

        const size_t index = static_cast<size_t> (slot) * static_cast<size_t> (kNumFxTypes)
                           + static_cast<size_t> (type);

        auto& module = owned[index];

        if (module == nullptr)
        {
            switch (type)
            {
                case fxReverb: module = std::make_unique<ReverbFx>(); break;
                default:       return nullptr;
            }

            // Fully built and prepared BEFORE it is ever published, so the audio
            // thread can never see a half-constructed module.
            module->prepare (sampleRate, blockSize, channels);
            module->reset();
        }

        return module.get();
    }

    void FxRack::setSlotType (int slot, int type)
    {
        if (slot < 0 || slot >= kMaxFxSlots)
            return;

        if (type <= fxOff || type >= kNumFxTypes)
        {
            active[static_cast<size_t> (slot)].store (nullptr, std::memory_order_release);
            slotTypes[static_cast<size_t> (slot)].store (fxOff, std::memory_order_release);
            return;
        }

        auto* module = ensureInstance (slot, type);

        // Release, paired with the acquire load in process(). The instance
        // already exists and is prepared, and this pointer is never reassigned to
        // a different object, so the audio thread either sees nullptr or a live
        // module - never a dangling one.
        active[static_cast<size_t> (slot)].store (module, std::memory_order_release);
        slotTypes[static_cast<size_t> (slot)].store (type, std::memory_order_release);
    }

    int FxRack::getSlotType (int slot) const
    {
        if (slot < 0 || slot >= kMaxFxSlots)
            return fxOff;

        return slotTypes[static_cast<size_t> (slot)].load (std::memory_order_acquire);
    }

    bool FxRack::hasInstance (int slot, int type) const
    {
        if (slot < 0 || slot >= kMaxFxSlots || type <= fxOff || type >= kNumFxTypes)
            return false;

        return owned[static_cast<size_t> (slot) * static_cast<size_t> (kNumFxTypes)
                     + static_cast<size_t> (type)] != nullptr;
    }

    void FxRack::clearAll()
    {
        for (auto& slot : active)
            slot.store (nullptr, std::memory_order_release);
    }

    int FxRack::getNumInstances() const
    {
        int n = 0;

        for (const auto& module : owned)
            if (module != nullptr)
                ++n;

        return n;
    }

    void FxRack::process (juce::AudioBuffer<float>& buffer,
                          const int* types,
                          const bool* bypass,
                          const float* params)
    {
        if (types == nullptr || bypass == nullptr || params == nullptr)
            return;

        for (int slot = 0; slot < kMaxFxSlots; ++slot)
        {
            // An Off or bypassed slot is skipped outright, so an empty rack is a
            // pure passthrough and costs nothing.
            if (types[slot] <= fxOff || bypass[slot])
                continue;

            auto* module = active[static_cast<size_t> (slot)].load (std::memory_order_acquire);

            if (module == nullptr)
                continue;

            module->process (buffer, params + static_cast<size_t> (slot) * kMaxFxParams);
        }
    }
}
