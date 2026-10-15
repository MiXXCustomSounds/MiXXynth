#pragma once

#include <array>

#include "FmVoice.h"
#include "ModShape.h"

// M3a: drag-and-drop modulation engine, with Serum/Vital-style editable shapes.
//
// Sources (17): Off, LFO 1-4, Env 1-3, Macro 1-4, Velocity, ModWheel,
// Aftertouch, KeyTrack, Timbre.
// Destinations (51): Off, FM Index, Op1-6 Ratio, Op1-6 Level, Master, VA
// Level/Detune/PulseWidth, Filter Cutoff/Reso/Drive, then the 30 matrix depths.
// Slots (32): each is source + destination + bipolar depth (-1..+1).
//
// The four LFOs and three envelopes are breakpoint shapes (see ModShape.h) whose
// mode decides whether they loop, run once on note-on, or hold.
//
// Modulation is evaluated PER VOICE: shapes keep their own phase and state per
// voice, so shapes retrigger per note rather than running globally. Slot depths
// are smoothed per block, and everything is allocation-free.
namespace mixxynth
{
    constexpr int kNumModSources = 17;
    constexpr int kNumModDests   = 51;
    constexpr int kNumModSlots   = 32;
    constexpr int kNumMacros     = 4;

    enum ModSource
    {
        srcOff = 0,
        srcLfo1, srcLfo2, srcLfo3, srcLfo4,
        srcEnv1, srcEnv2, srcEnv3,
        srcMacro1, srcMacro2, srcMacro3, srcMacro4,
        srcVelocity, srcModWheel, srcAftertouch, srcKeyTrack,
        srcTimbre // MPE per-note slide (timbre/Y); 0 when MPE is not in use
    };

    enum ModDest
    {
        dstOff = 0,
        dstFmIndex,
        dstOp1Ratio, dstOp2Ratio, dstOp3Ratio, dstOp4Ratio, dstOp5Ratio, dstOp6Ratio,
        dstOp1Level, dstOp2Level, dstOp3Level, dstOp4Level, dstOp5Level, dstOp6Level,
        dstMaster,
        // M3b: VA layer and filter.
        dstVaLevel, dstVaDetune, dstVaPulseWidth, dstFilterCutoff, dstFilterReso, dstFilterDrive,
        dstMtxFirst // 30 matrix depths follow
    };

    struct ModSlot
    {
        int source = srcOff;
        int dest = dstOff;
        float depth = 0.0f;
    };

    struct ModParams
    {
        std::array<ModSlot, kNumModSlots> slots {};
        // 4 LFO shapes followed by 3 envelope shapes.
        std::array<ModShape, kNumShapes> shapes {};
        std::array<float, kNumMacros> macros {};
        float modWheel = 0.0f;
        float aftertouch = 0.0f;
        double sampleRate = 44100.0;
        bool mpeActive = false;
    };

    // Per-voice runtime state for one shape.
    struct ShapeState
    {
        double phase = 0.0;          // 0..1 position through the shape
        float delayRemaining = 0.0f; // seconds of note-on delay still to run
        float smoothed = 0.0f;       // one-pole smoothed output
        float releaseFrom = 0.0f;    // value the release ramp started from
        bool active = false;         // note is driving this shape
        bool releasing = false;
    };

    // Per-voice runtime state for the modulating sources.
    struct ModState
    {
        std::array<ShapeState, kNumShapes> shapeStates {};
        bool gate = false;

        void noteOn (const ModParams& p);
        void noteOff (const ModParams& p);
        void reset();
    };

    // Sync divisions, in cycles per beat (multiplier applied to the BPM rate).
    // Defined in ModMatrix.cpp.
    const char* const* syncDivisionNames();
    int numSyncDivisions();
    float syncDivisionCyclesPerBeat (int index);

    // Canonical modulation source names, indexed by ModSource. Single source of
    // truth shared by the processor's choice parameter, the editor's chips and
    // the knob's right-click menu.
    const char* const* modSourceNames();

    // Computes all source values for one block. notePressure/noteTimbre carry
    // the per-note expression of the voice being rendered (MPE); when MPE is off
    // the caller passes the global aftertouch value and zero timbre.
    void computeSources (ModState& state,
                         const ModParams& params,
                         float velocity,
                         int noteNumber,
                         float notePressure,
                         float noteTimbre,
                         int numSamples,
                         std::array<float, kNumModSources>& out);

    // Applies the slot matrix on top of a base VoiceParams, per voice.
    void applyModulation (const std::array<float, kNumModSources>& sources,
                          const ModParams& params,
                          MiXXFmVoice::VoiceParams& vp);
}
