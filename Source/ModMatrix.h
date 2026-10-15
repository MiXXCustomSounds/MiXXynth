#pragma once

#include <array>

#include "FmVoice.h"

// M3a: drag-and-drop modulation engine.
//
// Sources (17): Off, LFO 1-4, Env 1-3, MSEG, Macro 1-4, Velocity, ModWheel,
// Aftertouch, KeyTrack.
// Destinations (45): Off, FM Index, Op1-6 Ratio, Op1-6 Level, Master, then the
// 30 off-diagonal matrix depths.
// Slots (32): each is source + destination + bipolar depth (-1..+1).
//
// Modulation is evaluated PER VOICE: LFOs, envelopes and the MSEG keep their own
// phase/state per voice, so vibrato and mod envelopes retrigger per note rather
// than running globally. Macros and performance sources are shared inputs.
// Slot depths are smoothed per block, and everything is allocation-free.
namespace mixxynth
{
    constexpr int kNumModSources = 17;
    constexpr int kNumModDests   = 50;
    constexpr int kNumModSlots   = 32;
    constexpr int kNumLfos       = 4;
    constexpr int kNumModEnvs    = 3;
    constexpr int kNumMacros     = 4;
    constexpr int kNumMsegSteps  = 8;

    enum ModSource
    {
        srcOff = 0,
        srcLfo1, srcLfo2, srcLfo3, srcLfo4,
        srcEnv1, srcEnv2, srcEnv3,
        srcMseg,
        srcMacro1, srcMacro2, srcMacro3, srcMacro4,
        srcVelocity, srcModWheel, srcAftertouch, srcKeyTrack
    };

    enum ModDest
    {
        dstOff = 0,
        dstFmIndex,
        dstOp1Ratio, dstOp2Ratio, dstOp3Ratio, dstOp4Ratio, dstOp5Ratio, dstOp6Ratio,
        dstOp1Level, dstOp2Level, dstOp3Level, dstOp4Level, dstOp5Level, dstOp6Level,
        dstMaster,
        // M3b: VA layer and filter.
        dstVaLevel, dstVaDetune, dstFilterCutoff, dstFilterReso, dstFilterDrive,
        dstMtxFirst // 30 matrix depths follow
    };

    enum LfoShape
    {
        lfoSine = 0, lfoTriangle, lfoSaw, lfoRamp, lfoSquare, lfoSampleHold
    };

    constexpr int kNumLfoShapes = 6;

    struct ModSlot
    {
        int source = srcOff;
        int dest = dstOff;
        float depth = 0.0f;
    };

    struct LfoParams
    {
        int shape = lfoSine;
        float rateHz = 2.0f;
        bool sync = false;
        int syncIndex = 8;   // index into the sync division table
        float phase = 0.0f;  // start phase 0..1
        bool retrig = true;
        bool bipolar = true;
    };

    struct EnvParams
    {
        float attack = 0.01f;
        float decay = 0.30f;
        float sustain = 0.50f;
        float release = 0.40f;
    };

    struct ModParams
    {
        std::array<ModSlot, kNumModSlots> slots {};
        std::array<LfoParams, kNumLfos> lfos {};
        std::array<EnvParams, kNumModEnvs> envs {};
        std::array<float, kNumMsegSteps> msegLevels {};
        float msegRateHz = 1.0f;
        bool msegLoop = true;
        std::array<float, kNumMacros> macros {};
        float modWheel = 0.0f;
        float aftertouch = 0.0f;
        double sampleRate = 44100.0;
    };

    // Per-voice runtime state for the modulating sources.
    struct ModState
    {
        std::array<double, kNumLfos> lfoPhase {};
        std::array<float, kNumLfos> lfoHold {};
        std::array<int, kNumModEnvs> envStage {};   // 0 idle, 1 A, 2 D, 3 S, 4 R
        std::array<float, kNumModEnvs> envValue {};
        std::array<float, kNumModEnvs> envReleaseFrom {};
        double msegPos = 0.0;
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

    // Computes all source values for one block.
    void computeSources (ModState& state,
                         const ModParams& params,
                         float velocity,
                         int noteNumber,
                         int numSamples,
                         std::array<float, kNumModSources>& out);

    // Applies the slot matrix on top of a base VoiceParams, per voice.
    void applyModulation (const std::array<float, kNumModSources>& sources,
                          const ModParams& params,
                          MiXXFmVoice::VoiceParams& vp);
}
