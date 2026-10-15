#include "ModMatrix.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
namespace
{
    constexpr double twoPi = 6.28318530717958647692;

    // Musical sync divisions, expressed in cycles per beat.
    const char* const syncNames[] = { "1/1", "1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32",
                                      "2 bar", "4 bar" };
    const float syncCycles[] = { 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 0.125f, 0.0625f };

    float shapeValue (int shape, double phase, float& holdState, bool wrapped)
    {
        const float p = static_cast<float> (phase - std::floor (phase));

        switch (shape)
        {
            case lfoTriangle:   return 1.0f - 4.0f * std::abs (p - 0.5f);
            case lfoSaw:        return 1.0f - 2.0f * p;
            case lfoRamp:       return 2.0f * p - 1.0f;
            case lfoSquare:     return p < 0.5f ? 1.0f : -1.0f;
            case lfoSampleHold:
            {
                if (wrapped)
                {
                    // Deterministic LCG: reproducible for tests, no allocation.
                    static thread_local juce::uint32 lcg = 0x1234567u;
                    lcg = lcg * 1664525u + 1013904223u;
                    holdState = static_cast<float> ((lcg >> 8) & 0xffff) / 32768.0f - 1.0f;
                }

                return holdState;
            }
            default:            return std::sin (static_cast<float> (twoPi * p));
        }
    }

    float envStep (int stage, float value, float releaseFrom, const EnvParams& e,
                   double sr, float dtSeconds)
    {
        switch (stage)
        {
            case 1:
                value += static_cast<float> (dtSeconds / juce::jmax (0.001f, e.attack));
                if (value >= 1.0f)
                    value = 1.0f;
                break;
            case 2:
                value -= static_cast<float> (dtSeconds / juce::jmax (0.001f, e.decay)) * (1.0f - e.sustain);
                if (value <= e.sustain)
                    value = e.sustain;
                break;
            case 3:
                value = e.sustain;
                break;
            case 4:
                value -= static_cast<float> (dtSeconds / juce::jmax (0.001f, e.release)) * releaseFrom;
                if (value <= 0.0f)
                    value = 0.0f;
                break;
            default:
                value = 0.0f;
                break;
        }

        juce::ignoreUnused (sr);
        return value;
    }
}

const char* const* syncDivisionNames() { return syncNames; }
int numSyncDivisions() { return static_cast<int> (sizeof (syncNames) / sizeof (syncNames[0])); }

float syncDivisionCyclesPerBeat (int index)
{
    const int n = numSyncDivisions();
    return syncCycles[static_cast<size_t> (juce::jlimit (0, n - 1, index))];
}

void ModState::reset()
{
    lfoPhase.fill (0.0);
    lfoHold.fill (0.0f);
    envStage.fill (0);
    envValue.fill (0.0f);
    envReleaseFrom.fill (0.0f);
    msegPos = 0.0;
    gate = false;
}

void ModState::noteOn (const ModParams& p)
{
    for (int i = 0; i < kNumLfos; ++i)
        if (p.lfos[static_cast<size_t> (i)].retrig)
            lfoPhase[static_cast<size_t> (i)] = p.lfos[static_cast<size_t> (i)].phase;

    for (int i = 0; i < kNumModEnvs; ++i)
    {
        envStage[static_cast<size_t> (i)] = 1;
        envValue[static_cast<size_t> (i)] = 0.0f;
        envReleaseFrom[static_cast<size_t> (i)] = 0.0f;
    }

    msegPos = 0.0;
    gate = true;
}

void ModState::noteOff (const ModParams& p)
{
    juce::ignoreUnused (p);
    gate = false;

    for (int i = 0; i < kNumModEnvs; ++i)
    {
        if (envStage[static_cast<size_t> (i)] != 0 && envStage[static_cast<size_t> (i)] != 4)
        {
            envStage[static_cast<size_t> (i)] = 4;
            envReleaseFrom[static_cast<size_t> (i)] = envValue[static_cast<size_t> (i)];
        }
    }
}

void computeSources (ModState& state,
                     const ModParams& p,
                     float velocity,
                     int noteNumber,
                     float notePressure,
                     float noteTimbre,
                     int numSamples,
                     std::array<float, kNumModSources>& out)
{
    out.fill (0.0f);

    const double dt = static_cast<double> (numSamples) / juce::jmax (1.0, p.sampleRate);
    const float dtSec = static_cast<float> (dt);

    // LFOs (per voice: phase advances with the block, retriggered on note-on).
    for (int i = 0; i < kNumLfos; ++i)
    {
        const auto& lp = p.lfos[static_cast<size_t> (i)];

        const double prev = state.lfoPhase[static_cast<size_t> (i)];
        const double advanced = prev + static_cast<double> (lp.rateHz) * dt;
        const bool wrapped = std::floor (advanced) > std::floor (prev);
        double ph = advanced - std::floor (advanced);
        state.lfoPhase[static_cast<size_t> (i)] = ph;

        float v = shapeValue (lp.shape, ph, state.lfoHold[static_cast<size_t> (i)], wrapped);

        if (! lp.bipolar)
            v = v * 0.5f + 0.5f;

        out[static_cast<size_t> (srcLfo1 + i)] = v;
    }

    // Mod envelopes.
    for (int i = 0; i < kNumModEnvs; ++i)
    {
        const auto& ep = p.envs[static_cast<size_t> (i)];
        int stage = state.envStage[static_cast<size_t> (i)];

        float v = envStep (stage, state.envValue[static_cast<size_t> (i)],
                           state.envReleaseFrom[static_cast<size_t> (i)], ep, p.sampleRate, dtSec);

        if (stage == 1 && v >= 1.0f)             stage = 2;
        else if (stage == 2 && v <= ep.sustain)  stage = 3;
        else if (stage == 4 && v <= 0.0f)        stage = 0;

        state.envStage[static_cast<size_t> (i)] = stage;
        state.envValue[static_cast<size_t> (i)] = v;
        out[static_cast<size_t> (srcEnv1 + i)] = v;
    }

    // MSEG-lite: linear interpolation across the step levels, looping.
    {
        state.msegPos += static_cast<double> (p.msegRateHz) * dt;

        if (state.msegPos >= 1.0)
            state.msegPos -= std::floor (state.msegPos);

        const double scaled = state.msegPos * static_cast<double> (kNumMsegSteps);
        const int i0 = static_cast<int> (scaled) % kNumMsegSteps;
        const int i1 = (i0 + 1) % kNumMsegSteps;
        const float frac = static_cast<float> (scaled - std::floor (scaled));

        out[static_cast<size_t> (srcMseg)] =
            p.msegLevels[static_cast<size_t> (i0)] * (1.0f - frac)
            + p.msegLevels[static_cast<size_t> (i1)] * frac;
    }

    for (int i = 0; i < kNumMacros; ++i)
        out[static_cast<size_t> (srcMacro1 + i)] = juce::jlimit (0.0f, 1.0f, p.macros[static_cast<size_t> (i)]);

    out[static_cast<size_t> (srcVelocity)]   = juce::jlimit (0.0f, 1.0f, velocity);
    out[static_cast<size_t> (srcModWheel)]   = juce::jlimit (0.0f, 1.0f, p.modWheel);
    out[static_cast<size_t> (srcAftertouch)] = juce::jlimit (0.0f, 1.0f, notePressure);
    out[static_cast<size_t> (srcTimbre)]     = juce::jlimit (0.0f, 1.0f, noteTimbre);
    out[static_cast<size_t> (srcKeyTrack)]   =
        juce::jlimit (-1.0f, 1.0f, static_cast<float> (noteNumber - 60) / 48.0f);
}

namespace
{
    // Matrix destination index -> (src, dst) operator pair, skipping self cells.
    void mtxIndexToPair (int idx, int& src, int& dst)
    {
        int k = 0;

        for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        {
            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            {
                if (s == d)
                    continue;

                if (k == idx)
                {
                    src = s;
                    dst = d;
                    return;
                }

                ++k;
            }
        }

        src = 0;
        dst = 1;
    }
}

void applyModulation (const std::array<float, kNumModSources>& sources,
                      const ModParams& params,
                      MiXXFmVoice::VoiceParams& vp)
{
    for (const auto& slot : params.slots)
    {
        if (slot.source == srcOff || slot.dest == dstOff || slot.depth == 0.0f)
            continue;

        const float amt = slot.depth * sources[static_cast<size_t> (slot.source)];

        if (amt == 0.0f)
            continue;

        if (slot.dest == dstFmIndex)
        {
            vp.index = juce::jlimit (0.0f, 12.0f, vp.index + amt * 12.0f);
        }
        else if (slot.dest >= dstOp1Ratio && slot.dest <= dstOp6Ratio)
        {
            const int i = slot.dest - dstOp1Ratio;
            vp.ratio[static_cast<size_t> (i)] =
                juce::jlimit (0.25f, 16.0f, vp.ratio[static_cast<size_t> (i)] + amt * 16.0f);
        }
        else if (slot.dest >= dstOp1Level && slot.dest <= dstOp6Level)
        {
            const int i = slot.dest - dstOp1Level;
            vp.level[static_cast<size_t> (i)] =
                juce::jlimit (0.0f, 1.0f, vp.level[static_cast<size_t> (i)] + amt);
        }
        else if (slot.dest == dstMaster)
        {
            vp.master = juce::jlimit (0.0f, 1.0f, vp.master + amt);
        }
        else if (slot.dest == dstVaLevel)
        {
            vp.vaLevel = juce::jlimit (0.0f, 1.0f, vp.vaLevel + amt);
        }
        else if (slot.dest == dstVaDetune)
        {
            vp.vaDetune = juce::jlimit (-100.0f, 100.0f, vp.vaDetune + amt * 50.0f);
        }
        else if (slot.dest == dstFilterCutoff)
        {
            // Modulating cutoff by ear needs an exponential sweep, so the depth
            // is applied in octaves rather than hertz.
            vp.cutoff = juce::jlimit (20.0f, 20000.0f,
                                      vp.cutoff * std::pow (2.0f, amt * 5.0f));
        }
        else if (slot.dest == dstFilterReso)
        {
            vp.resonance = juce::jlimit (0.0f, 0.98f, vp.resonance + amt);
        }
        else if (slot.dest == dstFilterDrive)
        {
            vp.drive = juce::jlimit (0.0f, 1.0f, vp.drive + amt);
        }
        else
        {
            int s = 0, d = 1;
            mtxIndexToPair (slot.dest - dstMtxFirst, s, d);
            vp.mod[s][d] = juce::jlimit (0.0f, 1.0f, vp.mod[s][d] + amt);
        }
    }
}
}
