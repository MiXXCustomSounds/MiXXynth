// M2a offline DSP smoke test: algorithms + free matrix, no plugin host.
// Checks: audible output, finite samples, envelope decay, feedback stability,
// all 32 DX7 algorithms render sanely, matrix fan-out and backward edges stable.
#include "../Source/Algorithms.h"
#include "../Source/FmVoice.h"
#include "../Source/VoiceManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    constexpr double sr = 44100.0;
    constexpr int blockLen = static_cast<int> (sr); // 1 second

    double rmsOf (const juce::AudioBuffer<float>& buf, int start, int length)
    {
        double acc = 0.0;
        int count = 0;

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            for (int i = 0; i < length; ++i)
            {
                const float s = buf.getSample (ch, start + i);
                if (! std::isfinite (s))
                    return -1.0; // sentinel: non-finite sample
                acc += s * s;
                ++count;
            }
        }

        return std::sqrt (acc / count);
    }

    // Voice params from a DX7 algorithm index (free mode when index >= 32).
    MiXXFmVoice::VoiceParams paramsForAlgo (int algoIndex)
    {
        MiXXFmVoice::VoiceParams p;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            p.ratio[i] = 1.0f;
            p.level[i] = 0.8f;
        }

        p.index = 3.0f;
        p.master = 0.5f;

        if (algoIndex >= 0 && algoIndex < mixxynth::kNumAlgorithms)
        {
            const auto& algo = mixxynth::algorithmTable()[static_cast<size_t> (algoIndex)];

            for (int src = 0; src < MiXXFmVoice::numOps; ++src)
                for (int dst = 0; dst < MiXXFmVoice::numOps; ++dst)
                    p.mod[src][dst] = algo.mod[static_cast<size_t> (src)][static_cast<size_t> (dst)];

            for (int i = 0; i < MiXXFmVoice::numOps; ++i)
                p.carrier[i] = algo.carrier[static_cast<size_t> (i)];
        }

        return p;
    }

    bool renderAndCheck (const MiXXFmVoice::VoiceParams& p, double& rmsOut, float& peakOut)
    {
        MiXXFmVoice v;
        v.setSampleRate (sr);

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        v.renderBlock (buf, midi, p);

        rmsOut = rmsOf (buf, static_cast<int> (sr * 0.1), static_cast<int> (sr * 0.5));
        peakOut = 0.0f;

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            peakOut = juce::jmax (peakOut, buf.getMagnitude (ch, 0, blockLen));

        return std::isfinite (rmsOut) && rmsOut > 0.001 && peakOut <= 1.0f;
    }
}

int main()
{
    bool ok = true;

    mixxynth::ModParams mod; // default: all slots Off, so tests see the base patch
    mod.sampleRate = sr;

    auto check = [] (bool cond, const char* name, double v)
    {
        std::printf ("%s %-34s (%.6f)\n", cond ? "PASS" : "FAIL", name, v);
        return cond;
    };

    // --- 1. 2-op pair (op1 modulates op2), the M1b reference sound ---
    {
        MiXXFmVoice v;
        v.setSampleRate (sr);

        MiXXFmVoice::VoiceParams p;
        p.mod[0][1] = 1.0f;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            p.carrier[i] = (i == 1);

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 0);
        v.renderBlock (buf, midi, p);
        const double rmsOn = rmsOf (buf, static_cast<int> (sr * 0.1), static_cast<int> (sr * 0.5));

        juce::AudioBuffer<float> tail (2, blockLen);
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
        v.renderBlock (tail, off, p);
        const double tailHead = rmsOf (tail, 0, static_cast<int> (sr * 0.1));
        const double tailEnd = rmsOf (tail, static_cast<int> (sr * 0.8), static_cast<int> (sr * 0.2));

        ok = check (rmsOn > 0.001, "2-op pair audible", rmsOn) && ok;
        ok = check (tailEnd >= 0.0, "2-op tail finite", tailEnd) && ok;
        ok = check (tailEnd < tailHead, "2-op tail decays", tailEnd) && ok;
    }

    // --- 2. All 32 DX7 algorithms: finite, audible, bounded ---
    {
        int bad = 0;
        int quiet = 0;

        for (int a = 0; a < mixxynth::kNumAlgorithms; ++a)
        {
            double rms = 0.0;
            float peak = 0.0f;

            if (! renderAndCheck (paramsForAlgo (a), rms, peak))
            {
                ++bad;
                std::printf ("FAIL algorithm %2d  rms=%.6f peak=%.6f\n", a + 1, rms, peak);
            }

            if (rms <= 0.001)
                ++quiet;
        }

        ok = check (bad == 0, "all 32 algorithms sane", static_cast<double> (bad)) && ok;
        ok = check (quiet == 0, "all 32 algorithms audible", static_cast<double> (quiet)) && ok;
    }

    // --- 3. Free matrix: fan-out, fan-in, backward edge ---
    {
        MiXXFmVoice::VoiceParams p;
        p.index = 4.0f;
        p.master = 0.5f;

        // op6 modulates op2 and op3 (fan-out); op5 also modulates op3 (fan-in).
        p.mod[5][1] = 1.0f;
        p.mod[5][2] = 1.0f;
        p.mod[4][2] = 1.0f;
        // Backward edge (op1 -> op6) exercises the 1-sample-delay path.
        p.mod[0][5] = 0.7f;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            p.carrier[i] = (i == 2);

        double rms = 0.0;
        float peak = 0.0f;
        ok = check (renderAndCheck (p, rms, peak), "matrix fan-out/backward stable", rms) && ok;
    }

    // --- 4. Max per-op feedback stays finite and bounded ---
    {
        auto p = paramsForAlgo (30);

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            p.feedback[i] = 0.95f;

        p.index = 8.0f;

        double rms = 0.0;
        float peak = 0.0f;
        ok = check (renderAndCheck (p, rms, peak), "max feedback finite+bounded", rms) && ok;
    }

    // --- 5. Polyphony: three held notes allocate three voices ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 64, 1.0f), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 67, 1.0f), 0);
        vm.renderBlock (buf, midi, paramsForAlgo (4), mod, poly);

        const double rms = rmsOf (buf, static_cast<int> (sr * 0.1), static_cast<int> (sr * 0.5));
        ok = check (vm.getActiveNoteCount() == 3, "poly: 3 notes held", vm.getActiveNoteCount()) && ok;
        ok = check (vm.getActiveVoiceCount() == 3, "poly: 3 voices active", vm.getActiveVoiceCount()) && ok;
        ok = check (rms > 0.001, "poly: chord audible", rms) && ok;
    }

    // --- 6. Unison: one note allocates N detuned voices, stereo-spread ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 8;
        poly.unisonDetune = 20.0f;
        poly.unisonSpread = 1.0f;

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        vm.renderBlock (buf, midi, paramsForAlgo (4), mod, poly);

        // Channel energy difference proves the stereo spread is applied.
        double sumL = 0.0, sumR = 0.0;

        for (int i = 0; i < blockLen; ++i)
        {
            const float l = buf.getSample (0, i);
            const float r = buf.getSample (1, i);
            sumL += l * l;
            sumR += r * r;
        }

        const double diff = std::abs (sumL - sumR);

        ok = check (vm.getActiveVoiceCount() == 8, "unison: 8 voices", vm.getActiveVoiceCount()) && ok;
        ok = check (diff > 1e-6, "unison: stereo spread applied", diff) && ok;
    }

    // --- 7. Polyphony cap: 4 notes with polyphony 2 keeps only 2 held ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 2;
        poly.unison = 1;

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;

        for (int n = 0; n < 4; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60 + n, 1.0f), 0);

        vm.renderBlock (buf, midi, paramsForAlgo (4), mod, poly);

        ok = check (vm.getActiveNoteCount() == 2, "poly cap: 2 notes held", vm.getActiveNoteCount()) && ok;
    }

    // --- 8. Voice pool is never exceeded, even at max unison ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 32;
        poly.unison = 16;

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;

        for (int n = 0; n < 12; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, 48 + n * 2, 1.0f), 0);

        vm.renderBlock (buf, midi, paramsForAlgo (4), mod, poly);

        const int voices = vm.getActiveVoiceCount();
        ok = check (voices <= MiXXVoiceManager::maxVoices, "pool cap respected", voices) && ok;
        ok = check (voices > 0, "pool cap: still sounding", voices) && ok;
    }

    // --- 9. Dense polyphony never exceeds full scale (interim output stage) ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 32;
        poly.unison = 4;
        poly.unisonDetune = 30.0f;
        poly.unisonSpread = 1.0f;

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer midi;

        for (int n = 0; n < 10; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, 40 + n * 3, 1.0f), 0);

        auto loud = paramsForAlgo (4);
        loud.master = 1.0f;
        loud.index = 10.0f;
        vm.renderBlock (buf, midi, loud, mod, poly);

        float peak = 0.0f;

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockLen));

        ok = check (peak <= 1.0f, "dense poly bounded (<=1.0)", peak) && ok;
        ok = check (peak > 0.001f, "dense poly audible", peak) && ok;
    }

    // --- 10. Modulation identity: an Off source is a no-op ---
    {
        auto base = paramsForAlgo (4);
        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto offSlot = mod;
        offSlot.slots[0] = { mixxynth::srcOff, mixxynth::dstFmIndex, 1.0f };

        juce::AudioBuffer<float> a (2, blockLen), b (2, blockLen);
        juce::MidiBuffer m1, m2;
        m1.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        m2.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

        MiXXVoiceManager v1, v2;
        v1.prepare (sr);
        v2.prepare (sr);
        v1.renderBlock (a, m1, base, mod, poly);
        v2.renderBlock (b, m2, base, offSlot, poly);

        float maxDiff = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockLen; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

        ok = check (maxDiff < 1e-9f, "mod: Off source is a no-op", maxDiff) && ok;
    }

    // --- 11. LFO -> FM Index audibly changes the output ---
    {
        auto base = paramsForAlgo (4);
        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto m = mod;
        m.shapes[0].rateHz = 5.0f;
        m.slots[0] = { mixxynth::srcLfo1, mixxynth::dstFmIndex, 1.0f };

        juce::AudioBuffer<float> a (2, blockLen), b (2, blockLen);
        juce::MidiBuffer m1, m2;
        m1.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        m2.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

        MiXXVoiceManager v1, v2;
        v1.prepare (sr);
        v2.prepare (sr);
        v1.renderBlock (a, m1, base, mod, poly);
        v2.renderBlock (b, m2, base, m, poly);

        double diff = 0.0;

        for (int i = 0; i < blockLen; ++i)
            diff += std::abs (a.getSample (0, i) - b.getSample (0, i));

        ok = check (diff > 1.0, "mod: LFO->FM Index changes output", diff) && ok;
    }

    // --- 12. Mod envelope -> master acts over time ---
    {
        auto base = paramsForAlgo (4);
        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto m = mod;
        // A slow one-shot ramp: flat for the first half, then rising to 1, so the
        // master is only pulled down later in the note.
        auto& env = m.shapes[mixxynth::kNumLfos];
        env.numPoints = 3;
        env.points[0] = { 0.0f, 0.0f, 0.0f };
        env.points[1] = { 0.5f, 0.0f, 0.0f };
        env.points[2] = { 1.0f, 1.0f, 0.0f };
        env.mode = mixxynth::shapeOneShot;
        env.rateHz = 1.0f;
        env.bipolar = false;
        m.slots[0] = { mixxynth::srcEnv1, mixxynth::dstMaster, -0.8f };

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer mm;
        mm.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

        MiXXVoiceManager v;
        v.prepare (sr);
        v.renderBlock (buf, mm, base, m, poly);

        const double early = rmsOf (buf, 0, static_cast<int> (sr * 0.05));
        const double late = rmsOf (buf, static_cast<int> (sr * 0.6), static_cast<int> (sr * 0.3));

        ok = check (late < early, "mod: env lowers master over time", late - early) && ok;
    }

    // --- Shape engine: tension, interpolation, ordering, state round-trip ---
    {
        using namespace mixxynth;

        // Tension is an identity at the endpoints and monotonic in between.
        ok = check (std::abs (applyTension (0.0f, 0.7f)) < 1.0e-5f
                      && std::abs (applyTension (1.0f, -0.7f) - 1.0f) < 1.0e-5f,
                    "shape: tension preserves endpoints",
                    applyTension (1.0f, -0.7f)) && ok;

        ok = check (applyTension (0.5f, 0.8f) > 0.5f, "shape: positive tension rises fast",
                    applyTension (0.5f, 0.8f)) && ok;
        ok = check (applyTension (0.5f, -0.8f) < 0.5f, "shape: negative tension rises slow",
                    applyTension (0.5f, -0.8f)) && ok;

        ModShape ramp;
        ramp.numPoints = 2;
        ramp.points[0] = { 0.0f, 0.0f, 0.0f };
        ramp.points[1] = { 1.0f, 1.0f, 0.0f };

        ok = check (std::abs (ramp.valueAt (0.25f) - 0.25f) < 1.0e-4f,
                    "shape: linear ramp at 0.25", ramp.valueAt (0.25f)) && ok;
        ok = check (std::abs (ramp.valueAt (1.5f) - 1.0f) < 1.0e-5f
                      && std::abs (ramp.valueAt (-0.5f)) < 1.0e-5f,
                    "shape: clamps outside the point range",
                    ramp.valueAt (1.5f)) && ok;

        // sortPoints orders x and pins the endpoints.
        ModShape unsorted;
        unsorted.numPoints = 3;
        unsorted.points[0] = { 0.8f, 0.2f, 0.0f };
        unsorted.points[1] = { 0.2f, 0.9f, 0.0f };
        unsorted.points[2] = { 0.5f, 0.5f, 0.0f };
        unsorted.sortPoints();

        ok = check (unsorted.points[0].x == 0.0f && unsorted.points[2].x == 1.0f
                      && std::abs (unsorted.points[0].y - 0.9f) < 1.0e-6f,
                    "shape: sortPoints orders x and pins endpoints",
                    unsorted.points[0].y) && ok;

        const auto dl = ModShape::defaultLfo();
        const auto de = ModShape::defaultEnv();

        ok = check (dl.mode == shapeLfo && dl.bipolar && dl.numPoints >= 2,
                    "shape: default LFO is a bipolar LFO", dl.numPoints) && ok;
        ok = check (de.mode == shapeEnv && ! de.bipolar,
                    "shape: default ENV is a unipolar envelope", de.mode) && ok;

        // State round-trip through a ValueTree.
        auto edited = ModShape::defaultEnv();
        edited.numPoints = 4;
        edited.points[0] = { 0.0f, 0.1f, -0.4f };
        edited.points[1] = { 0.3f, 0.95f, 0.25f };
        edited.points[2] = { 0.6f, 0.5f, 0.0f };
        edited.points[3] = { 1.0f, 0.0f, 0.0f };
        edited.rateHz = 3.5f;
        edited.smooth = 0.42f;
        edited.delaySeconds = 0.25f;
        edited.sync = true;
        edited.syncIndex = 5;
        edited.retrig = false;
        edited.mode = shapeOneShot;

        ModShape restored;
        shapeFromValueTree (ModShape::defaultLfo(), shapeToValueTree (edited, "shape"), restored);

        bool same = restored.numPoints == edited.numPoints
                      && restored.mode == edited.mode
                      && std::abs (restored.rateHz - edited.rateHz) < 1.0e-4f
                      && std::abs (restored.smooth - edited.smooth) < 1.0e-4f
                      && std::abs (restored.delaySeconds - edited.delaySeconds) < 1.0e-4f
                      && restored.sync == edited.sync
                      && restored.syncIndex == edited.syncIndex
                      && restored.retrig == edited.retrig;

        for (int i = 0; same && i < edited.numPoints; ++i)
        {
            const auto& p = edited.points[static_cast<size_t> (i)];
            const auto& q = restored.points[static_cast<size_t> (i)];
            same = std::abs (q.x - p.x) < 1.0e-4f && std::abs (q.y - p.y) < 1.0e-4f
                     && std::abs (q.tension - p.tension) < 1.0e-4f;
        }

        ok = check (same, "shape: state round-trip preserves the shape", restored.numPoints) && ok;

        ModShape fallbackTest;
        shapeFromValueTree (ModShape::defaultLfo(), juce::ValueTree(), fallbackTest);
        ok = check (fallbackTest.numPoints == ModShape::defaultLfo().numPoints,
                    "shape: invalid node keeps the fallback", fallbackTest.numPoints) && ok;
    }


    // --- 13. Matrix-depth destination: modulating a routing depth works ---
    {
        MiXXFmVoice::VoiceParams base;
        base.index = 6.0f;
        base.master = 0.5f;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            base.ratio[i] = 1.0f;
            base.level[i] = 0.5f;
        }


        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto m = mod;
        m.macros[0] = 1.0f;
        // Destination 0 past dstMtxFirst is the first off-diagonal cell: Op1 > Op2.
        m.slots[0] = { mixxynth::srcMacro1, mixxynth::dstMtxFirst, 1.0f };

        juce::AudioBuffer<float> a (2, blockLen), b (2, blockLen);
        juce::MidiBuffer m1, m2;
        m1.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        m2.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

        MiXXVoiceManager v1, v2;
        v1.prepare (sr);
        v2.prepare (sr);
        v1.renderBlock (a, m1, base, mod, poly);
        v2.renderBlock (b, m2, base, m, poly);

        double diff = 0.0;

        for (int i = 0; i < blockLen; ++i)
            diff += std::abs (a.getSample (0, i) - b.getSample (0, i));

        ok = check (diff > 1.0, "mod: matrix-depth destination works", diff) && ok;
    }
    // --- Shape modes: ENV holds then releases, one-shot ignores note-off ---
    {
        using namespace mixxynth;

        ModShape envShape;
        envShape.numPoints = 3;
        envShape.points[0] = { 0.0f, 0.0f, 0.0f };
        envShape.points[1] = { 0.3f, 1.0f, 0.0f };
        envShape.points[2] = { 1.0f, 0.8f, 0.0f };
        envShape.mode = shapeEnv;
        envShape.rateHz = 4.0f;
        envShape.bipolar = false;
        envShape.releaseSeconds = 0.05f;

        auto run = [&] (const ModShape& shape, bool releaseMidway)
        {
            ModParams mp;
            mp.sampleRate = sr;
            mp.shapes[mixxynth::kNumLfos] = shape; // shape index 4 is Env 1

            ModState st;
            st.reset();
            st.noteOn (mp);

            std::array<float, kNumModSources> src {};

            for (int b = 0; b < 200; ++b)
                computeSources (st, mp, 1.0f, 60, 0.0f, 0.0f, 512, src);

            const float held = src[static_cast<size_t> (srcEnv1)];

            if (releaseMidway)
                st.noteOff (mp);

            for (int b = 0; b < 40; ++b)
                computeSources (st, mp, 1.0f, 60, 0.0f, 0.0f, 512, src);

            return std::pair<float, float> { held, src[static_cast<size_t> (srcEnv1)] };
        };

        const auto envMode = run (envShape, true);
        ok = check (envMode.first > 0.7f, "shape: ENV holds its last point while held",
                    envMode.first) && ok;
        ok = check (envMode.second < 0.05f, "shape: ENV releases to zero on note-off",
                    envMode.second) && ok;

        auto oneShotShape = envShape;
        oneShotShape.mode = shapeOneShot;
        const auto oneShot = run (oneShotShape, true);
        ok = check (oneShot.first > 0.7f, "shape: one-shot reaches its last point",
                    oneShot.first) && ok;
        ok = check (oneShot.second > 0.7f, "shape: one-shot ignores note-off and holds",
                    oneShot.second) && ok;
    }



    // --- Operator waveforms: band-limited, bounded, continuously morphing ---
    {
        using namespace mixxynth;

        OpWaveTables tables;
        tables.prepare (sr);
        ok = check (tables.isPrepared(), "opwave: tables build", 1) && ok;

        float worstPeak = 0.0f;
        float lowestPeak = 1.0e9f;
        bool allFinite = true;

        for (int s = 0; s < 16; ++s)
        {
            const float wave = static_cast<float> (s) / 15.0f;
            float peak = 0.0f;

            for (int i = 0; i < 512; ++i)
            {
                const float v = tables.sample (static_cast<float> (i) / 512.0f, wave, 220.0);

                if (! std::isfinite (v))
                    allFinite = false;

                peak = juce::jmax (peak, std::abs (v));
            }

            worstPeak = juce::jmax (worstPeak, peak);
            lowestPeak = juce::jmin (lowestPeak, peak);
        }

        ok = check (allFinite, "opwave: every shape is finite", allFinite ? 1.0 : 0.0) && ok;
        // The upper bound allows the usual Gibbs overshoot on a truncated saw
        // and square.
        ok = check (worstPeak < 1.5f, "opwave: every shape is bounded", worstPeak) && ok;
        ok = check (lowestPeak > 0.5f, "opwave: every shape is audible", lowestPeak) && ok;

        // Wave 0 really is a sine.
        {
            double err = 0.0;

            for (int i = 0; i < 256; ++i)
            {
                const float ph = static_cast<float> (i) / 256.0f;
                err += std::abs (tables.sample (ph, 0.0f, 220.0)
                                   - std::sin (6.28318530717958647692 * static_cast<double> (ph)));
            }

            ok = check (err < 0.05, "opwave: wave 0 is a sine", err) && ok;
        }

        // Morphing is continuous: a tiny knob move must not jump the output.
        {
            float worstJump = 0.0f;

            for (int s = 0; s < 30; ++s)
            {
                const float a = static_cast<float> (s) / 30.0f;
                const float b = a + 0.001f;

                for (int i = 0; i < 128; ++i)
                {
                    const float ph = static_cast<float> (i) / 128.0f;
                    worstJump = juce::jmax (worstJump,
                                            std::abs (tables.sample (ph, a, 220.0)
                                                        - tables.sample (ph, b, 220.0)));
                }
            }

            ok = check (worstJump < 0.02f, "opwave: morph is continuous", worstJump) && ok;
        }

        // The four stops are actually different from one another.
        {
            double sineVsSaw = 0.0, sawVsSquare = 0.0;

            for (int i = 0; i < 256; ++i)
            {
                const float ph = static_cast<float> (i) / 256.0f;
                sineVsSaw += std::abs (tables.sample (ph, 0.0f, 220.0)
                                         - tables.sample (ph, 0.667f, 220.0));
                sawVsSquare += std::abs (tables.sample (ph, 0.667f, 220.0)
                                           - tables.sample (ph, 1.0f, 220.0));
            }

            ok = check (sineVsSaw > 20.0, "opwave: saw differs from sine", sineVsSaw) && ok;
            ok = check (sawVsSquare > 5.0, "opwave: square differs from saw", sawVsSquare) && ok;
        }

        // Prove all four stops are genuinely distinct using harmonic energy above
        // the 6th harmonic, which is a perceptual stand-in for brightness. This is
        // the check that would catch a morph collapsing to two shapes.
        {
            auto highEnergy = [&tables] (float wave)
            {
                constexpr int N = 512;
                double total = 0.0;

                for (int h = 7; h <= 60; ++h)
                {
                    double re = 0.0, im = 0.0;

                    for (int i = 0; i < N; ++i)
                    {
                        const double ph = static_cast<double> (i) / N;
                        const double v = tables.sample (static_cast<float> (ph), wave, 220.0);
                        const double a = 6.28318530717958647692 * h * ph;
                        re += v * std::cos (a);
                        im += v * std::sin (a);
                    }

                    total += (re * re + im * im) / (N * N);
                }

                return total;
            };

            const double eSine = highEnergy (0.0f);
            const double eTri = highEnergy (1.0f / 3.0f);
            const double eSaw = highEnergy (2.0f / 3.0f);
            const double eSqr = highEnergy (1.0f);

            ok = check (eSine < 1.0e-9, "opwave: sine has no upper harmonics", eSine) && ok;
            ok = check (eTri > 1.0e-6, "opwave: triangle adds upper harmonics", eTri) && ok;
            ok = check (eSaw > eTri * 5.0, "opwave: saw is far richer than triangle",
                        eSaw / eTri) && ok;
            ok = check (eSqr > eTri * 5.0 && std::abs (eSqr - eSaw) > 1.0e-4,
                        "opwave: square differs from both", eSqr / eTri) && ok;
        }
    }

    // --- Operator enable and waveform reach the audio path ---
    {
        using namespace mixxynth;

        OpWaveTables tables;
        tables.prepare (sr);

        auto base = paramsForAlgo (4);
        base.master = 1.0f;
        base.waveTables = &tables;

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto render = [&] (const MiXXFmVoice::VoiceParams& p, juce::AudioBuffer<float>& buf)
        {
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            MiXXVoiceManager v;
            v.prepare (sr);
            v.renderBlock (buf, m, p, mod, poly);
        };

        juce::AudioBuffer<float> sineBuf (2, blockLen), squareBuf (2, blockLen);
        render (base, sineBuf);

        auto square = base;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            square.opWave[i] = 1.0f;

        render (square, squareBuf);

        double waveDiff = 0.0;

        for (int i = 0; i < blockLen; ++i)
            waveDiff += std::abs (sineBuf.getSample (0, i) - squareBuf.getSample (0, i));

        ok = check (waveDiff > 1.0, "op: waveform changes the sound", waveDiff) && ok;
        ok = check (rmsOf (squareBuf, 0, blockLen) > 0.0,
                    "op: square operators stay finite", rmsOf (squareBuf, 0, blockLen)) && ok;

        // Every operator off must be silent.
        auto off = base;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            off.opOn[i] = false;

        juce::AudioBuffer<float> offBuf (2, blockLen);
        render (off, offBuf);

        const double offRms = rmsOf (offBuf, 0, blockLen);
        ok = check (offRms >= 0.0 && offRms < 1.0e-6, "op: all operators off is silent", offRms) && ok;
        ok = check (rmsOf (sineBuf, 0, blockLen) > 0.001,
                    "op: the same patch with ops on is audible",
                    rmsOf (sineBuf, 0, blockLen)) && ok;

        // Turning off the modulators must change the sound without silencing it.
        auto oneOff = base;

        for (int i = 1; i < MiXXFmVoice::numOps; ++i)
            oneOff.opOn[i] = false;

        juce::AudioBuffer<float> oneOffBuf (2, blockLen);
        render (oneOff, oneOffBuf);

        const double oneOffRms = rmsOf (oneOffBuf, 0, blockLen);
        ok = check (oneOffRms > 0.0001, "op: carriers stay audible with modulators off",
                    oneOffRms) && ok;
    }

    // --- Unison phase randomisation decorrelates voices ---
    {
        auto p = paramsForAlgo (4);
        p.master = 1.0f;

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 8;

        auto render = [&] (float phaseRand)
        {
            auto pp = poly;
            pp.phaseRand = phaseRand;

            juce::AudioBuffer<float> buf (2, blockLen);
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

            MiXXVoiceManager v;
            v.prepare (sr);
            v.renderBlock (buf, m, p, mod, pp);
            return buf;
        };

        auto diffOf = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            double d = 0.0;

            for (int i = 0; i < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++i)
                d += std::abs (a.getSample (0, i) - b.getSample (0, i));

            return d;
        };

        // At 0 every voice starts aligned, so repeated notes are identical.
        const auto alignedA = render (0.0f);
        const auto alignedB = render (0.0f);
        const double alignedDiff = diffOf (alignedA, alignedB);

        ok = check (alignedDiff < 1.0e-9, "phase rand 0 is deterministic", alignedDiff) && ok;

        // At 1 each voice starts anywhere, so repeated notes differ.
        const auto randA = render (1.0f);
        const auto randB = render (1.0f);
        const double randDiff = diffOf (randA, randB);

        ok = check (randDiff > 1.0, "phase rand decorrelates repeated notes", randDiff) && ok;

        const double randRms = rmsOf (randA, 0, blockLen);
        ok = check (randRms > 0.0001 && randRms < 2.0,
                    "phase rand stays audible and bounded", randRms) && ok;
    }

    // --- 14. VA layer: every waveform renders finite, audible and bounded ---
    {
        for (int w = 0; w < 4; ++w)
        {
            auto p = paramsForAlgo (4);
            p.vaLevel = 1.0f; // VA only
            p.vaWave = w;
            p.vaOctave = 0;
            p.index = 0.0f;

            double rms = 0.0;
            float peak = 0.0f;
            const bool good = renderAndCheck (p, rms, peak);

            if (! good)
                std::printf ("FAIL VA wave %d  rms=%.6f peak=%.6f\n", w, rms, peak);

            ok = check (good, w == 0 ? "VA saw sane" : (w == 1 ? "VA square sane"
                             : (w == 2 ? "VA sub sane" : "VA noise sane")), rms) && ok;
        }
    }

    // --- 15. VA + FM blend produces a different signal to FM alone ---
    {
        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto fmOnly = paramsForAlgo (4);
        auto blended = paramsForAlgo (4);
        blended.vaLevel = 0.5f;
        blended.vaWave = 0;

        juce::AudioBuffer<float> a (2, blockLen), b (2, blockLen);
        juce::MidiBuffer m1, m2;
        m1.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        m2.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

        MiXXVoiceManager v1, v2;
        v1.prepare (sr);
        v2.prepare (sr);
        v1.renderBlock (a, m1, fmOnly, mod, poly);
        v2.renderBlock (b, m2, blended, mod, poly);

        double diff = 0.0;

        for (int i = 0; i < blockLen; ++i)
            diff += std::abs (a.getSample (0, i) - b.getSample (0, i));

        ok = check (diff > 1.0, "VA blends with FM", diff) && ok;
    }

    // --- 16. Filter: low pass reduces energy of a bright source ---
    {
        auto open = paramsForAlgo (4);
        open.vaLevel = 1.0f;
        open.vaWave = 0; // saw: bright
        open.index = 0.0f;
        open.filterOn = true;
        open.filterType = 0; // LP
        open.cutoff = 18000.0f;

        auto closed = open;
        closed.cutoff = 200.0f;

        double rmsOpen = 0.0, rmsClosed = 0.0;
        float peakOpen = 0.0f, peakClosed = 0.0f;
        const bool okOpen = renderAndCheck (open, rmsOpen, peakOpen);
        const bool okClosed = renderAndCheck (closed, rmsClosed, peakClosed);

        ok = check (okOpen && okClosed, "filter stable (open+closed)", rmsClosed) && ok;
        ok = check (rmsClosed < rmsOpen, "low pass reduces energy", rmsClosed - rmsOpen) && ok;
    }

    // --- 17. Filter: high resonance and drive stay bounded ---
    {
        auto p = paramsForAlgo (4);
        p.vaLevel = 1.0f;
        p.vaWave = 1;
        p.index = 0.0f;
        p.filterOn = true;
        p.filterType = 2; // band pass
        p.cutoff = 800.0f;
        p.resonance = 0.98f;
        p.drive = 1.0f;

        double rms = 0.0;
        float peak = 0.0f;
        ok = check (renderAndCheck (p, rms, peak), "resonant driven filter bounded", rms) && ok;
    }

    // --- 18. Pitch bend actually changes pitch ---
    {
        auto p = paramsForAlgo (4);
        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto renderWithBend = [&] (float semitones, juce::AudioBuffer<float>& buf)
        {
            MiXXVoiceManager vm;
            vm.prepare (sr);
            vm.setPitchBendSemitones (semitones);

            juce::MidiBuffer mm;
            mm.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            vm.renderBlock (buf, mm, p, mod, poly);
        };

        juce::AudioBuffer<float> flat (2, blockLen), bent (2, blockLen);
        renderWithBend (0.0f, flat);
        renderWithBend (12.0f, bent);

        // Count zero crossings: an octave up should cross roughly twice as often.
        auto crossings = [] (const juce::AudioBuffer<float>& b)
        {
            int n = 0;
            const auto* d = b.getReadPointer (0);

            for (int i = 1; i < b.getNumSamples(); ++i)
                if ((d[i - 1] < 0.0f) != (d[i] < 0.0f))
                    ++n;

            return n;
        };

        const int flatX = crossings (flat);
        const int bentX = crossings (bent);
        const double ratio = flatX > 0 ? static_cast<double> (bentX) / flatX : 0.0;

        ok = check (ratio > 1.6 && ratio < 2.4, "pitch bend +12st doubles pitch",
                    ratio) && ok;
    }

    // --- 19. Pitch bend is applied on top of unison detune, not instead of it ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);
        vm.setPitchBendSemitones (5.0f);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 4;
        poly.unisonDetune = 20.0f;

        juce::AudioBuffer<float> buf (2, blockLen);
        juce::MidiBuffer mm;
        mm.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        vm.renderBlock (buf, mm, paramsForAlgo (4), mod, poly);

        double rms = 0.0;
        float peak = 0.0f;
        ok = check (renderAndCheck (paramsForAlgo (4), rms, peak) && vm.getActiveVoiceCount() == 4,
                    "bend keeps unison voices alive", vm.getActiveVoiceCount()) && ok;
    }

    // --- 20. MPE: per-note pitch bend only affects the note on that channel ---
    {
        auto p = paramsForAlgo (4);
        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        auto renderNoteOnChannel = [&] (int channel, int bendChannel, float semis,
                                        juce::AudioBuffer<float>& buf)
        {
            MiXXVoiceManager vm;
            vm.prepare (sr);

            juce::MidiBuffer mm;
            mm.addEvent (juce::MidiMessage::noteOn (channel, 60, 1.0f), 0);
            vm.renderBlock (buf, mm, p, mod, poly);

            // Next block: expression arrives with no new note events, so the
            // note is not retriggered (retriggering correctly resets expression).
            vm.setNotePitchBend (bendChannel, 60, semis);
            juce::MidiBuffer none;
            vm.renderBlock (buf, none, p, mod, poly);
        };

        auto crossings = [] (const juce::AudioBuffer<float>& b)
        {
            int n = 0;
            const auto* d = b.getReadPointer (0);

            for (int i = 1; i < b.getNumSamples(); ++i)
                if ((d[i - 1] < 0.0f) != (d[i] < 0.0f))
                    ++n;

            return n;
        };

        juce::AudioBuffer<float> bentOnTwo (2, blockLen), bentOnThree (2, blockLen);
        renderNoteOnChannel (2, 2, 12.0f, bentOnTwo);
        renderNoteOnChannel (2, 3, 12.0f, bentOnThree);

        const double ratioOnTwo = static_cast<double> (crossings (bentOnTwo))
                                  / juce::jmax (1, crossings (bentOnThree));

        // Bending channel 2 (where the note lives) must raise its pitch; bending
        // channel 3 must leave it alone.
        ok = check (ratioOnTwo > 1.6 && ratioOnTwo < 2.4,
                    "MPE bend affects only its own channel", ratioOnTwo) && ok;
    }

    // --- 21. MPE: per-note pressure and timbre reach the voice ---
    {
        MiXXVoiceManager vm;
        vm.prepare (sr);

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 1;

        juce::MidiBuffer mm;
        mm.addEvent (juce::MidiMessage::noteOn (4, 60, 1.0f), 0);
        juce::AudioBuffer<float> buf (2, blockLen);
        vm.renderBlock (buf, mm, paramsForAlgo (4), mod, poly);

        // These must not disturb voice count or produce non-finite output.
        vm.setNotePressure (4, 60, 0.8f);
        vm.setNoteTimbre (4, 60, 0.9f);
        vm.setNotePressure (9, 60, 0.5f); // no note on channel 9: must be ignored

        double rms = 0.0;
        float peak = 0.0f;
        ok = check (renderAndCheck (paramsForAlgo (4), rms, peak) && vm.getActiveVoiceCount() == 1,
                    "MPE pressure/timbre are safe", vm.getActiveVoiceCount()) && ok;
    }

    std::printf (ok ? "SMOKE OK\n" : "SMOKE FAILED\n");
    return ok ? 0 : 1;
}
