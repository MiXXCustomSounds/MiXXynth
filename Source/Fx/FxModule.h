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
    //
    // APPEND ONLY. A slot's type is saved as this index, so inserting a type in the
    // middle would silently repoint every existing patch's slots at the wrong
    // module.
    enum FxType
    {
        fxOff = 0,
        fxWeirdVerb,
        fxConvolution,
        fxJunoChorus,
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

        // A module that needs a file declares it here - currently only the
        // convolution reverb, for its impulse response.
        //
        // A file is NOT a parameter and cannot be: an APVTS parameter is a number
        // (or a choice between numbers), and audio data is not a number. So the
        // file lives in the state tree beside the shapes, and the card draws a
        // button instead of a knob.
        bool hasFile = false;
        const char* fileLabel = "File";
        const char* fileFilter = "*";
    };

    // Everything a slot holds, so reordering can permute whole slots rather than
    // copying fields one at a time.
    struct FxSlotState
    {
        int type = fxOff;
        bool bypass = false;
        std::array<float, kMaxFxParams> params {};

        // Non-parameter state for modules that declare hasFile. `fileName` is the
        // display name (and, for a file the user chose, its original path).
        // `fileData` is the file's bytes, carried in the state so a preset is
        // self-contained and does not break when the original file moves.
        //
        // Only user-chosen files embed: a module's built-in options are named by
        // `params`, so they need no bytes and a preset stays small.
        juce::String fileName;
        juce::MemoryBlock fileData;

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

        // Hands the module a file, on the MESSAGE thread. Only called for modules
        // whose descriptor sets hasFile.
        //
        // `data` is empty when the slot has no embedded file - either nothing is
        // loaded, or the module's own built-in option is selected, in which case
        // `name` is still worth honouring for display. A module must cope with
        // being handed nothing at all, including before it is ever called.
        virtual void setFile (const juce::String& name, const juce::MemoryBlock& data)
        {
            juce::ignoreUnused (name, data);
        }

        // Audio thread. Must not allocate.
        virtual void process (juce::AudioBuffer<float>& buffer, const float* params) = 0;
    };
}
