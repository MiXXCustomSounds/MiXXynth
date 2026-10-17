#include "FxRack.h"

#include "WeirdVerbFx.h"
#include "ConvolutionFx.h"
#include "JunoChorusFx.h"
#include "DistortionFx.h"
#include "PingPongDelayFx.h"
#include "FlangerFx.h"
#include "PhaserFx.h"
#include "CompressorFx.h"
#include "LimiterFx.h"
#include "MultibandFx.h"
#include "EqFx.h"
#include "MiXXDriveFx.h"

namespace mixxynth
{
    const char* fxTypeName (int type) noexcept
    {
        switch (type)
        {
            case fxOff:         return "Off";
            case fxWeirdVerb:   return "WeirdVerb";
            case fxConvolution: return "Convolution";
            case fxJunoChorus:  return "Juno Chorus";
            case fxDistortion:  return "Distortion";
            case fxPingPongDelay: return "Ping-Pong Delay";
            case fxFlanger:     return "Flanger";
            case fxPhaser:      return "Phaser";
            case fxCompressor:  return "Compressor";
            case fxLimiter:     return "Limiter";
            case fxMultiband:   return "Multiband";
            case fxGraphicEq:   return "Graphic EQ";
            case fxMiXXDrive:   return "MiXX Drive";
            default:            break;
        }

        return "Off";
    }

    FxDescriptor FxRack::descriptorFor (int type)
    {
        switch (type)
        {
            case fxWeirdVerb:   return WeirdVerbFx::descriptor();
            case fxConvolution: return ConvolutionFx::descriptor();
            case fxJunoChorus:  return JunoChorusFx::descriptor();
            case fxDistortion:  return DistortionFx::descriptor();
            case fxPingPongDelay: return PingPongDelayFx::descriptor();
            case fxFlanger:     return FlangerFx::descriptor();
            case fxPhaser:      return PhaserFx::descriptor();
            case fxCompressor:  return CompressorFx::descriptor();
            case fxLimiter:     return LimiterFx::descriptor();
            case fxMultiband:   return MultibandFx::descriptor();
            case fxGraphicEq:   return EqFx::descriptor();
            case fxMiXXDrive:   return MiXXDriveFx::descriptor();
            default:            break;
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
                case fxWeirdVerb:   module = std::make_unique<WeirdVerbFx>(); break;
                case fxConvolution: module = std::make_unique<ConvolutionFx>(); break;
                case fxJunoChorus:  module = std::make_unique<JunoChorusFx>(); break;
                case fxDistortion:  module = std::make_unique<DistortionFx>(); break;
                case fxPingPongDelay: module = std::make_unique<PingPongDelayFx>(); break;
                case fxFlanger:     module = std::make_unique<FlangerFx>(); break;
                case fxPhaser:      module = std::make_unique<PhaserFx>(); break;
                case fxCompressor:  module = std::make_unique<CompressorFx>(); break;
                case fxLimiter:     module = std::make_unique<LimiterFx>(); break;
                case fxMultiband:   module = std::make_unique<MultibandFx>(); break;
                case fxGraphicEq:   module = std::make_unique<EqFx>(); break;
                case fxMiXXDrive:   module = std::make_unique<MiXXDriveFx>(); break;
                default:            return nullptr;
            }

            // Fully built and prepared BEFORE it is ever published, so the audio
            // thread can never see a half-constructed module.
            module->prepare (sampleRate, blockSize, channels);
            module->reset();
        }

        return module.get();
    }

    void FxRack::setSlotFile (int slot, int type, const juce::String& name,
                              const juce::MemoryBlock& data)
    {
        if (type <= fxOff || type >= kNumFxTypes)
            return;

        // Creating the instance first, so a file can be handed to a slot that has
        // never run: the type parameter and the file can arrive in either order
        // after a state load.
        if (auto* module = ensureInstance (slot, type))
            module->setFile (name, data);
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

    void FxRack::setTempo (double bpm)
    {
        // Every instance that exists, not just the active ones: a module that is not
        // currently selected still has to be current the moment it is, and the set is
        // small and fixed.
        for (auto& module : owned)
            if (module != nullptr)
                module->setTempo (bpm);
    }

    float FxRack::getSlotGainReductionDb (int slot) const
    {
        if (slot < 0 || slot >= kMaxFxSlots)
            return 0.0f;

        auto* module = active[static_cast<size_t> (slot)].load (std::memory_order_acquire);

        return module != nullptr ? module->getGainReductionDb() : 0.0f;
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
