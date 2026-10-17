#include "ModMatrix.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
namespace
{
    // Musical sync divisions, expressed in cycles per beat.
    const char* const syncNames[] = { "1/1", "1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32",
                                      "2 bar", "4 bar" };
    const float syncCycles[] = { 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 0.125f, 0.0625f };

    // Must match ModSource exactly; the editor audit asserts the count.
    const char* const sourceNames[] = { "Off",
                                        "LFO 1", "LFO 2", "LFO 3", "LFO 4",
                                        "LFO 5", "LFO 6", "LFO 7", "LFO 8",
                                        "Env 1", "Env 2", "Env 3", "Env 4",
                                        "Env 5", "Env 6", "Env 7", "Env 8",
                                        "Macro 1", "Macro 2", "Macro 3", "Macro 4",
                                        "Velocity", "Mod Wheel", "Aftertouch", "Key Track",
                                        "Timbre" };
}

const char* const* modSourceNames() { return sourceNames; }


const char* const* syncDivisionNames() { return syncNames; }
int numSyncDivisions() { return static_cast<int> (sizeof (syncNames) / sizeof (syncNames[0])); }

float syncDivisionCyclesPerBeat (int index)
{
    const int n = numSyncDivisions();
    return syncCycles[static_cast<size_t> (juce::jlimit (0, n - 1, index))];
}

void ModState::reset()
{
    for (auto& s : shapeStates)
        s = ShapeState {};

    gate = false;
}

void ModState::noteOn (const ModParams& p)
{
    for (int i = 0; i < p.shapes.count(); ++i)
    {
        const auto& shape = p.shapes.shapes[static_cast<size_t> (i)];
        auto& s = shapeStates[static_cast<size_t> (i)];

        // LFOs only restart if retrigger is on; envelopes and one-shots always do.
        if (shape.mode == shapeLfo && ! shape.retrig)
            continue;

        s.phase = juce::jlimit (0.0, 1.0, static_cast<double> (shape.phase));
        s.delayRemaining = juce::jmax (0.0f, shape.delaySeconds);
        s.active = true;
        s.releasing = false;
        s.releaseFrom = 0.0f;
        s.smoothed = shape.valueAt (static_cast<float> (s.phase));
    }

    gate = true;
}

void ModState::noteOff (const ModParams& p)
{
    gate = false;

    for (int i = 0; i < p.shapes.count(); ++i)
    {
        const auto& shape = p.shapes.shapes[static_cast<size_t> (i)];

        // Only envelope shapes release: LFOs keep running and one-shots hold.
        if (shape.mode != shapeEnv)
            continue;

        auto& s = shapeStates[static_cast<size_t> (i)];
        s.releasing = true;
        s.releaseFrom = s.smoothed;
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

    // Editable shape modules. The first numLfos positions feed LFO 1-8, the rest
    // feed Env 1-8. Everything is per voice, so shapes retrigger per note.
    for (int i = 0; i < p.shapes.count(); ++i)
    {
        const auto& shape = p.shapes.shapes[static_cast<size_t> (i)];
        auto& s = state.shapeStates[static_cast<size_t> (i)];

        float v = 0.0f;

        if (s.delayRemaining > 0.0f)
        {
            // Note-on delay: hold the shape's first point.
            s.delayRemaining = juce::jmax (0.0f, s.delayRemaining - dtSec);
            v = shape.valueAt (0.0f);
        }
        else if (s.releasing)
        {
            // Envelope release ramps to zero.
            s.releaseFrom -= dtSec / juce::jmax (0.001f, shape.releaseSeconds);
            v = juce::jmax (0.0f, s.releaseFrom);

            if (s.releaseFrom <= 0.0f)
            {
                s.releasing = false;
                s.active = false;
            }
        }
        else if (s.active)
        {
            s.phase += static_cast<double> (juce::jmax (0.0f, shape.rateHz)) * dt;

            if (shape.mode == shapeLfo)
            {
                if (s.phase >= 1.0)
                    s.phase -= std::floor (s.phase);
            }
            else
            {
                // ENV and One-Shot make a single pass, then hold the last point.
                s.phase = juce::jmin (1.0, s.phase);
            }

            v = shape.valueAt (static_cast<float> (s.phase));
        }
        else
        {
            v = shape.mode == shapeLfo ? shape.valueAt (0.0f) : 0.0f;
        }

        // Bipolar shapes span -1..1 around the shape's midpoint.
        if (shape.bipolar)
            v = v * 2.0f - 1.0f;

        // One-pole smoothing (the "smooth" control).
        const float coef = shape.smooth <= 0.0f
                             ? 1.0f
                             : juce::jmap (juce::jlimit (0.0f, 1.0f, shape.smooth), 0.6f, 0.02f);
        s.smoothed += (v - s.smoothed) * coef;
        v = s.smoothed;

        if (i < p.shapes.numLfos)
            out[static_cast<size_t> (srcLfo1 + i)] = v;
        else
            out[static_cast<size_t> (srcEnv1 + (i - p.shapes.numLfos))] = v;
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

std::array<float, kNumModDests> modTotals (const std::array<float, kNumModSources>& sources,
                                           const ModParams& params)
{
    std::array<float, kNumModDests> totals {};

    for (const auto& slot : params.slots)
    {
        if (slot.source == srcOff || slot.dest == dstOff || slot.depth == 0.0f)
            continue;

        if (slot.dest < 0 || slot.dest >= kNumModDests)
            continue;

        totals[static_cast<size_t> (slot.dest)] +=
            slot.depth * sources[static_cast<size_t> (slot.source)];
    }

    return totals;
}

void publishModulatedValues (const MiXXFmVoice::VoiceParams& vp,
                             std::array<float, kNumModDests>& out)
{
    out.fill (0.0f);

    out[static_cast<size_t> (dstFmIndex)] = vp.index;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        out[static_cast<size_t> (dstOp1Ratio + i)] = vp.ratio[static_cast<size_t> (i)];
        out[static_cast<size_t> (dstOp1Level + i)] = vp.level[static_cast<size_t> (i)];
    }

    out[static_cast<size_t> (dstMaster)]       = vp.master;
    out[static_cast<size_t> (dstVaLevel)]      = vp.vaLevel;
    out[static_cast<size_t> (dstVaDetune)]     = vp.vaDetune;
    out[static_cast<size_t> (dstVaPulseWidth)] = vp.vaShape;
    out[static_cast<size_t> (dstFilterCutoff)] = vp.cutoff;
    out[static_cast<size_t> (dstFilterReso)]   = vp.resonance;
    out[static_cast<size_t> (dstFilterDrive)]  = vp.drive;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const auto o = static_cast<size_t> (i);

        out[static_cast<size_t> (dstOp1FilterCutoff + i)] = vp.opFilterCutoff[o];
        out[static_cast<size_t> (dstOp1FilterReso + i)]   = vp.opFilterReso[o];
        out[static_cast<size_t> (dstOp1FilterDrive + i)]  = vp.opFilterDrive[o];
        out[static_cast<size_t> (dstOp1Pan + i)]          = vp.pan[o];
    }

    // The 30 matrix depths, walked in the same order mtxIndexToPair walks them. That shared
    // order IS the contract between a slot's destination index and this write: get it wrong
    // in one place and every routing readout is silently off by one pair.
    int k = 0;

    for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            if (s != d)
                out[static_cast<size_t> (dstMtxFirst + k++)] =
                    vp.mod[static_cast<size_t> (s)][static_cast<size_t> (d)];
}

void applyModulation (const std::array<float, kNumModDests>& totals,
                      MiXXFmVoice::VoiceParams& vp)
{
    for (int dest = 0; dest < kNumModDests; ++dest)
    {
        const float amt = totals[static_cast<size_t> (dest)];

        if (amt == 0.0f)
            continue;

        if (dest == dstFmIndex)
        {
            // Matches the parameter's 0..2 range, so modulating FM Index cannot
            // push it past the musical end of the knob.
            vp.index = juce::jlimit (0.0f, 2.0f, vp.index + amt * 2.0f);
        }
        else if (dest >= dstOp1Ratio && dest <= dstOp6Ratio)
        {
            const int i = dest - dstOp1Ratio;
            vp.ratio[static_cast<size_t> (i)] =
                juce::jlimit (0.0f, 16.0f, vp.ratio[static_cast<size_t> (i)] + amt * 16.0f);
        }
        else if (dest >= dstOp1Level && dest <= dstOp6Level)
        {
            const int i = dest - dstOp1Level;
            vp.level[static_cast<size_t> (i)] =
                juce::jlimit (0.0f, 1.0f, vp.level[static_cast<size_t> (i)] + amt);
        }
        else if (dest == dstMaster)
        {
            vp.master = juce::jlimit (0.0f, 1.0f, vp.master + amt);
        }
        else if (dest == dstVaLevel)
        {
            vp.vaLevel = juce::jlimit (0.0f, 1.0f, vp.vaLevel + amt);
        }
        else if (dest == dstVaDetune)
        {
            vp.vaDetune = juce::jlimit (-100.0f, 100.0f, vp.vaDetune + amt * 50.0f);
        }
        else if (dest == dstVaPulseWidth)
        {
            // The slot keeps its index and its place in the destination list, so an old
            // preset's routing still points here - it just drives the shape morph now
            // instead of the old separate pulse width, which is the same idea expressed
            // on the new axis.
            vp.vaShape = juce::jlimit (0.0f, 1.0f, vp.vaShape + amt * 0.45f);
        }
        else if (dest == dstFilterCutoff)
        {
            // Modulating cutoff by ear needs an exponential sweep, so the depth
            // is applied in octaves rather than hertz.
            vp.cutoff = juce::jlimit (20.0f, 20000.0f,
                                      vp.cutoff * std::pow (2.0f, amt * 5.0f));
        }
        else if (dest == dstFilterReso)
        {
            vp.resonance = juce::jlimit (0.0f, 0.98f, vp.resonance + amt);
        }
        else if (dest >= dstOp1FilterCutoff && dest <= dstOp6FilterCutoff)
        {
            // Exponential, like the per-voice filter's cutoff and for the same reason: a
            // linear sweep of a cutoff in hertz spends most of its travel in the top octave
            // where almost nothing is audible.
            const int i = dest - dstOp1FilterCutoff;
            auto& c = vp.opFilterCutoff[static_cast<size_t> (i)];

            c = juce::jlimit (20.0f, 20000.0f, c * std::pow (2.0f, amt * 4.0f));
        }
        else if (dest >= dstOp1FilterReso && dest <= dstOp6FilterReso)
        {
            const int i = dest - dstOp1FilterReso;
            auto& r = vp.opFilterReso[static_cast<size_t> (i)];

            r = juce::jlimit (0.0f, 0.98f, r + amt);
        }
        else if (dest >= dstOp1FilterDrive && dest <= dstOp6FilterDrive)
        {
            const int i = dest - dstOp1FilterDrive;
            auto& d = vp.opFilterDrive[static_cast<size_t> (i)];

            d = juce::jlimit (0.0f, 1.0f, d + amt);
        }
        else if (dest == dstFilterDrive)
        {
            vp.drive = juce::jlimit (0.0f, 1.0f, vp.drive + amt);
        }
        else if (dest >= dstOp1Pan && dest <= dstOp6Pan)
        {
            // Full travel is the whole field, so a slot at +1 sweeps an operator from hard left
            // to hard right. This is auto-pan, and it is the reason pan is a destination at all:
            // a static pan is a placement, a moving one is an effect.
            //
            // It MUST be handled before the matrix fall-through below, which treats any
            // destination it does not recognise as a routing cell. A pan index reaching that
            // branch would silently rewrite the FM routing instead.
            const int i = dest - dstOp1Pan;
            vp.pan[static_cast<size_t> (i)] =
                juce::jlimit (-1.0f, 1.0f, vp.pan[static_cast<size_t> (i)] + amt);
        }
        else
        {
            int s = 0, d = 1;
            mtxIndexToPair (dest - dstMtxFirst, s, d);
            vp.mod[static_cast<size_t> (s)][static_cast<size_t> (d)] =
                juce::jlimit (0.0f, 1.0f,
                              vp.mod[static_cast<size_t> (s)][static_cast<size_t> (d)] + amt);
        }
    }
}

void applyModulation (const std::array<float, kNumModSources>& sources,
                      const ModParams& params,
                      MiXXFmVoice::VoiceParams& vp)
{
    applyModulation (modTotals (sources, params), vp);
}
}
