#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

// The FX rack's module interface (M4b workstream 4).
//
// The rack is a FIXED POOL of slots rather than a growable list, and for a
// concrete reason: an APVTS parameter is created once at construction and its
// items cannot change afterwards, so "add a module" cannot mean "add a
// parameter". Instead every slot owns a `type` choice parameter, and the set of
// types is fixed at compile time too.
//
// A module declares how many of the slot's generic p1..p8 parameters it uses and
// what they mean. The generic block is the pragmatic choice: the alternative is
// a named parameter per control per module, and the pool has to be sized for the
// worst case anyway, so it would cost more parameters for no extra clarity.
namespace mixxynth
{
    constexpr int kMaxFxSlots = 8;

    // Generic parameter block per slot. Sized for the largest module rather than
    // for the average one, because a slot's parameter count is fixed at
    // construction and cannot follow the type that is currently loaded. A module
    // declares how many of these it uses; the rest are inert.
    constexpr int kMaxFxParams = 20;

    // Fixed type list. Order must match `fxTypeName` and the choice parameter's
    // item list; the audit checks that it does.
    enum FxType
    {
        fxOff = 0,
        fxReverb,
        kNumFxTypes
    };

    const char* fxTypeName (int type) noexcept;

    // One generic slot parameter, as described by the module that owns it.
    struct FxParamInfo
    {
        const char* name = "";
        float defaultValue = 0.0f;
        bool isChoice = false;
        juce::StringArray choices; // only when isChoice
    };

    // What a module declares, so the rack can build its parameters and its card.
    struct FxDescriptor
    {
        const char* name = "Off";
        int numParams = 0;
        std::array<FxParamInfo, kMaxFxParams> params {};
    };

    // Everything a slot holds, so reordering can permute whole slots rather than
    // copying fields one at a time.
    struct FxSlotState
    {
        int type = fxOff;
        bool bypass = false;
        std::array<float, kMaxFxParams> params {};

        bool isEmpty() const noexcept { return type <= fxOff; }
    };

    // A rack module. One instance serves one slot.
    //
    // `params` points at kMaxFxParams normalised 0..1 values; a module reads only
    // the entries its descriptor declares.
    class FxModule
    {
    public:
        virtual ~FxModule() = default;

        // Called on the MESSAGE thread before the instance is published, and again
        // from prepareToPlay. Allocate here, never in process().
        virtual void prepare (double sampleRate, int maxBlockSize, int numChannels) = 0;

        // Drops any tail. Message thread.
        virtual void reset() = 0;

        // Audio thread. Must not allocate.
        virtual void process (juce::AudioBuffer<float>& buffer, const float* params) = 0;
    };
}
