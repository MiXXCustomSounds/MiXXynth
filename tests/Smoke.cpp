// M2a offline DSP smoke test: algorithms + free matrix, no plugin host.
// Checks: audible output, finite samples, envelope decay, feedback stability,
// all 32 DX7 algorithms render sanely, matrix fan-out and backward edges stable.
#include "../Source/Algorithms.h"
#include "../Source/FmVoice.h"
#include "../Source/VoiceManager.h"
#include "../Source/Fx/FxRack.h"
#include "../Source/Fx/WeirdVerbFx.h"
#include "../Source/Fx/ConvolutionFx.h"
#include "../Source/Fx/JunoChorusFx.h"
#include "../Source/Fx/DistortionFx.h"
#include "../Source/Fx/PingPongDelayFx.h"
#include "../Source/Fx/FlangerFx.h"
#include "../Source/Fx/PhaserFx.h"
#include "../Source/Limiter.h"

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
    // JUCE has to be initialised before anything touches its threading or its
    // message queue, and the convolution reverb does both: it loads its impulse on
    // a background thread and asks for the rebuild through an AsyncUpdater. The
    // editor audit has always done this; the smoke test did not need it until a
    // module arrived that uses JUCE's threading rather than just its DSP.
    juce::ScopedJuceInitialiser_GUI gui;

    bool ok = true;

    mixxynth::ModParams mod; // default: all slots Off, so tests see the base patch
    mod.sampleRate = sr;

    auto check = [] (bool cond, const char* name, double v)
    {
        std::printf ("%s %-34s (%.6f)\n", cond ? "PASS" : "FAIL", name, v);
        // Flushed per line: stdout is block-buffered when redirected, so a crash
        // discards everything still in the buffer and hides where it happened.
        std::fflush (stdout);
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
        m.shapes.shapes[0].rateHz = 5.0f;
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
        auto& env = m.shapes.shapes[mixxynth::kDefaultLfos]; // index 1 is Env 1
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
            mp.shapes.shapes[mixxynth::kDefaultLfos] = shape; // index 1 is Env 1

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
                                         - tables.sample (ph, 2.0f / 6.0f, 220.0));
                sawVsSquare += std::abs (tables.sample (ph, 2.0f / 6.0f, 220.0)
                                           - tables.sample (ph, 3.0f / 6.0f, 220.0));
            }

            ok = check (sineVsSaw > 20.0, "opwave: saw differs from sine", sineVsSaw) && ok;
            ok = check (sawVsSquare > 5.0, "opwave: square differs from saw", sawVsSquare) && ok;
        }

        // Prove every stop is genuinely distinct using harmonic energy above the 6th
        // harmonic, which is a perceptual stand-in for brightness. This is the check
        // that would catch a morph collapsing to two shapes.
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

            // Shape indices over the stop count, so extending the axis does not silently
            // repoint these at different shapes - which is exactly what happened when the
            // pulse tail was added.
            const double eSine = highEnergy (0.0f / 6.0f);
            const double eTri = highEnergy (1.0f / 6.0f);
            const double eSaw = highEnergy (2.0f / 6.0f);
            const double eSqr = highEnergy (3.0f / 6.0f);

            // The pulse tail is checked separately below, by measuring its duty.

            ok = check (eSine < 1.0e-9, "opwave: sine has no upper harmonics", eSine) && ok;
            ok = check (eTri > 1.0e-6, "opwave: triangle adds upper harmonics", eTri) && ok;
            ok = check (eSaw > eTri * 5.0, "opwave: saw is far richer than triangle",
                        eSaw / eTri) && ok;
            ok = check (eSqr > eTri * 5.0 && std::abs (eSqr - eSaw) > 1.0e-4,
                        "opwave: square differs from both", eSqr / eTri) && ok;

            // The pulse tail has to actually narrow, which is the whole point of it: the
            // duty is the property that makes the top of the sweep a pulse width control.
            //
            // Measuring the duty directly, rather than measuring brightness, because
            // harmonic energy in a fixed band does NOT rise monotonically as a pulse
            // narrows - the notches move as the duty changes, so the energy shifts up and
            // then back down again. The first version of this check assumed otherwise and
            // failed against correct code.
            auto dutyAt = [&tables] (float wave)
            {
                constexpr int N = 2048;
                int positive = 0;

                for (int i = 0; i < N; ++i)
                    if (tables.sample (static_cast<float> (i) / N, wave, 220.0) > 0.0f)
                        ++positive;

                return static_cast<double> (positive) / static_cast<double> (N);
            };

            const double duty50 = dutyAt (3.0f / 6.0f);
            const double duty25 = dutyAt (4.0f / 6.0f);
            const double duty12 = dutyAt (5.0f / 6.0f);
            const double duty6 = dutyAt (1.0f);

            ok = check (duty25 < duty50 && duty12 < duty25 && duty6 < duty12,
                        "opwave: the pulse tail narrows monotonically", duty6 / duty50) && ok;
            ok = check (std::abs (duty50 - 0.5) < 0.03 && std::abs (duty25 - 0.25) < 0.03,
                        "opwave: the pulse duties land where they should", duty25) && ok;
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

    // --- Phase randomisation decorrelates the unison stack ---
    // The point of the control: with the voices aligned they add coherently and
    // the stack starts with a big transient then combs (the "lasery" sound);
    // with them scattered the sum is far lower from the first sample.
    {
        auto p = paramsForAlgo (4);

        // Very quiet on purpose. The interim output stage is tanh(bus * mixGain),
        // so anything approaching full scale saturates and the two cases collapse
        // to the same peak - which is exactly what hid this the first time.
        p.master = 0.01f;

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 16;
        poly.unison = 8;
        poly.unisonDetune = 20.0f;
        poly.unisonSpread = 0.0f; // mono, so the measured sum is the stack

        auto peakOf = [&] (float phaseRand)
        {
            auto pp = poly;
            pp.phaseRand = phaseRand;

            juce::AudioBuffer<float> buf (2, 2048);
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);

            MiXXVoiceManager v;
            v.prepare (sr);
            v.renderBlock (buf, m, p, mod, pp);

            float peak = 0.0f;

            for (int i = 0; i < 512; ++i)
                peak = juce::jmax (peak, std::abs (buf.getSample (0, i)));

            return peak;
        };

        // Averaged over several draws: a single roll of eight phases can be
        // clustered by chance, and the ratio swings with it.
        float alignedTotal = 0.0f, scatteredTotal = 0.0f;

        for (int pass = 0; pass < 6; ++pass)
        {
            alignedTotal += peakOf (0.0f);
            scatteredTotal += peakOf (1.0f);
        }

        ok = check (alignedTotal > scatteredTotal * 1.3f,
                    "phase rand scatters the unison stack",
                    alignedTotal / juce::jmax (1.0e-6f, scatteredTotal)) && ok;
    }

    // --- 14. VA layer: every waveform renders finite, audible and bounded ---
    {
        // All seven morph stops, not the four this started with. The pulse stops were
        // added later and the test never grew with them, so three of the seven shapes
        // were going untested - which is the kind of gap that only shows up when one of
        // them breaks.
        for (int w = 0; w < mixxynth::OpWaveTables::numShapes; ++w)
        {
            auto p = paramsForAlgo (4);
            p.vaLevel = 1.0f; // VA only
            p.vaShape = static_cast<float> (w)
                          / static_cast<float> (mixxynth::OpWaveTables::numShapes - 1);
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
        blended.vaShape = 0;

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
        open.vaShape = 0; // saw: bright
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
        p.vaShape = 1;
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

    // --- 22. FX rack: an empty rack is a bit-exact passthrough ---
    {
        mixxynth::FxRack rack;
        rack.prepare (sr, 512, 2);

        juce::AudioBuffer<float> buf (2, 512);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                buf.setSample (ch, i, 0.5f * std::sin (0.01f * static_cast<float> (i)));

        juce::AudioBuffer<float> before (buf);

        int types[mixxynth::kMaxFxSlots] {};
        bool bypass[mixxynth::kMaxFxSlots] {};
        float params[mixxynth::kMaxFxSlots * mixxynth::kMaxFxParams] {};

        rack.process (buf, types, bypass, params);

        bool identical = true;

        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int i = 0; i < 512; ++i)
                if (buf.getSample (ch, i) != before.getSample (ch, i))
                    identical = false;

        ok = check (identical, "empty rack is a bit-exact passthrough", 1.0) && ok;
        ok = check (rack.getNumInstances() == 0, "empty rack allocates no modules", 0.0) && ok;
    }

    // --- 23. A bypassed slot is skipped entirely ---
    {
        mixxynth::FxRack rack;
        rack.prepare (sr, 512, 2);

        int types[mixxynth::kMaxFxSlots] {};
        bool bypass[mixxynth::kMaxFxSlots] {};
        float params[mixxynth::kMaxFxSlots * mixxynth::kMaxFxParams] {};

        // Selecting a type is what creates the module, and it happens on the
        // caller's (message) thread - never on the audio thread.
        types[0] = mixxynth::fxWeirdVerb;
        rack.setSlotType (0, mixxynth::fxWeirdVerb);

        ok = check (rack.hasInstance (0, mixxynth::fxWeirdVerb),
                    "selecting a type builds its module", 1.0) && ok;

        // Fully wet, so if the slot ran at all the output could not match.
        params[mixxynth::WeirdVerbFx::pMix] = 1.0f;

        juce::AudioBuffer<float> buf (2, 512);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                buf.setSample (ch, i, 0.4f * std::sin (0.05f * static_cast<float> (i)));

        juce::AudioBuffer<float> before (buf);

        bypass[0] = true;
        rack.process (buf, types, bypass, params);

        bool identical = true;

        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int i = 0; i < 512; ++i)
                if (buf.getSample (ch, i) != before.getSample (ch, i))
                    identical = false;

        ok = check (identical, "a bypassed slot is skipped", 1.0) && ok;

        // ...and un-bypassed, the same slot does change the signal.
        bypass[0] = false;
        juce::AudioBuffer<float> wet (before);
        rack.process (wet, types, bypass, params);

        bool changed = false;

        for (int ch = 0; ch < 2 && ! changed; ++ch)
            for (int i = 0; i < 512; ++i)
                if (std::abs (wet.getSample (ch, i) - before.getSample (ch, i)) > 1.0e-6f)
                    changed = true;

        ok = check (changed, "the same slot is live when not bypassed", 1.0) && ok;
    }

    // --- 24. Reverb: every mode is finite, bounded and audible ---
    {
        const char* names[5] = { "reverb plate audible and bounded",
                                 "reverb room audible and bounded",
                                 "reverb chamber audible and bounded",
                                 "reverb hall audible and bounded",
                                 "reverb spring audible and bounded" };

        for (int mode = 0; mode < 5; ++mode)
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pMode] = static_cast<float> (mode) / 4.0f;
            p[mixxynth::WeirdVerbFx::pDecay] = 0.6f;
            p[mixxynth::WeirdVerbFx::pPreDelay] = 0.1f;
            p[mixxynth::WeirdVerbFx::pDamping] = 0.7f;
            p[mixxynth::WeirdVerbFx::pModDepth] = 0.3f;
            p[mixxynth::WeirdVerbFx::pMix] = 1.0f; // wet only

            // A short burst then silence, so what is measured is the tail.
            juce::AudioBuffer<float> buf (2, blockLen);
            buf.clear();

            for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            verb.process (buf, p);

            const double tailRms = rmsOf (buf, static_cast<int> (sr * 0.2), static_cast<int> (sr * 0.5));
            float peak = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
                peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockLen));

            ok = check (std::isfinite (tailRms) && tailRms > 0.0005 && peak <= 1.0f,
                        names[mode], tailRms) && ok;
        }
    }

    // --- 24b. Decay and Size lengthen the tail monotonically ---
    {
        auto tailEnergy = [] (float size, float decay, float damp, float bass)
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pSize] = size;
            p[mixxynth::WeirdVerbFx::pDecay] = decay;
            p[mixxynth::WeirdVerbFx::pBass] = bass;
            p[mixxynth::WeirdVerbFx::pDamping] = damp;
            p[mixxynth::WeirdVerbFx::pDiffusion] = 0.7f;
            p[mixxynth::WeirdVerbFx::pHighCut] = 1.0f;
            p[mixxynth::WeirdVerbFx::pModDepth] = 0.35f;
            p[mixxynth::WeirdVerbFx::pModRate] = 0.35f;
            p[mixxynth::WeirdVerbFx::pWidth] = 0.5f;
            p[mixxynth::WeirdVerbFx::pMix] = 1.0f;

            juce::AudioBuffer<float> buf (2, blockLen);
            buf.clear();

            for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            verb.process (buf, p);

            // The second half of the second is pure tail.
            return rmsOf (buf, static_cast<int> (sr * 0.5), static_cast<int> (sr * 0.5));
        };

        // This is the check that would have caught the soft ceiling quietly
        // capping the decay: with it in place, 0.2 and 0.9 were almost identical.
        const double d0 = tailEnergy (0.5f, 0.2f, 0.35f, 0.5f);
        const double d1 = tailEnergy (0.5f, 0.55f, 0.35f, 0.5f);
        const double d2 = tailEnergy (0.5f, 0.9f, 0.35f, 0.5f);

        ok = check (d0 < d1 && d1 < d2, "Decay lengthens the tail monotonically",
                    d2 / juce::jmax (1.0e-9, d0)) && ok;

        const double s0 = tailEnergy (0.2f, 0.6f, 0.35f, 0.5f);
        const double s1 = tailEnergy (1.0f, 0.6f, 0.35f, 0.5f);

        ok = check (s1 > s0 * 1.2, "Size lengthens the tail",
                    s1 / juce::jmax (1.0e-9, s0)) && ok;

        // Damping shortens the high band, which on a sine test tone shows up as a
        // modest reduction - it is a brightness control, not a length control.
        // Compared at the SAME Decay, so this is a damping comparison and not a
        // decay comparison.
        const double openTail = tailEnergy (0.5f, 0.6f, 0.0f, 0.5f);
        const double dampedTail = tailEnergy (0.5f, 0.6f, 1.0f, 0.5f);

        ok = check (dampedTail <= openTail * 1.05, "Damping does not lengthen the tail",
                    dampedTail / juce::jmax (1.0e-9, openTail)) && ok;
    }

    // --- 24c. Ducking pulls the wet down while the dry is present ---
    {
        auto duckedLevel = [] (float duck)
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pDecay] = 0.7f;
            p[mixxynth::WeirdVerbFx::pDuck] = duck;
            p[mixxynth::WeirdVerbFx::pDiffusion] = 0.7f;
            p[mixxynth::WeirdVerbFx::pWidth] = 0.5f;
            p[mixxynth::WeirdVerbFx::pHighCut] = 1.0f;
            p[mixxynth::WeirdVerbFx::pMix] = 1.0f; // wet only, or the dry would mask it

            // Continuous input, so the duck envelope is fully engaged.
            juce::AudioBuffer<float> buf (2, blockLen);

            for (int i = 0; i < blockLen; ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            verb.process (buf, p);

            return rmsOf (buf, static_cast<int> (sr * 0.8), static_cast<int> (sr * 0.2));
        };

        const double noDuck = duckedLevel (0.0f);
        const double withDuck = duckedLevel (1.0f);

        ok = check (withDuck < noDuck * 0.9, "ducking pulls the wet down",
                    withDuck / juce::jmax (1.0e-9, noDuck)) && ok;
    }

    // --- 25. Infinite decay sustains, but does not run away ---
    {
        // Freeze is the manual version of the same thing: hold what is in the
        // tank and ignore anything new. It has to be tested in two stages, since
        // freezing from the very start would mean nothing ever got in.
        mixxynth::WeirdVerbFx verb;
        verb.prepare (sr, blockLen, 2);

        float p[mixxynth::kMaxFxParams] {};
        p[mixxynth::WeirdVerbFx::pDecay] = 0.25f; // would normally die away quickly
        p[mixxynth::WeirdVerbFx::pDiffusion] = 0.7f;
        p[mixxynth::WeirdVerbFx::pHighCut] = 1.0f;
        p[mixxynth::WeirdVerbFx::pMix] = 1.0f;

        const int half = static_cast<int> (sr * 0.5);

        juce::AudioBuffer<float> burst (2, half);
        burst.clear();

        for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
        {
            const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                              * static_cast<float> (i) / static_cast<float> (sr));
            burst.setSample (0, i, v);
            burst.setSample (1, i, v);
        }

        verb.process (burst, p);
        const double afterBurst = rmsOf (burst, half - 2000, 2000);

        juce::AudioBuffer<float> silence (2, half);
        silence.clear();

        p[mixxynth::WeirdVerbFx::pFreeze] = 1.0f;
        verb.process (silence, p);
        const double afterFreeze = rmsOf (silence, half - 2000, 2000);

        ok = check (afterFreeze > afterBurst * 0.6, "freeze holds the tail",
                    afterFreeze / juce::jmax (1.0e-9, afterBurst)) && ok;
    }

    // --- 25b. Width controls the stereo image of the wet signal ---
    {
        auto correlation = [] (float width)
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pDecay] = 0.7f;
            p[mixxynth::WeirdVerbFx::pDiffusion] = 0.7f;
            p[mixxynth::WeirdVerbFx::pModDepth] = 0.35f;
            p[mixxynth::WeirdVerbFx::pHighCut] = 1.0f;
            p[mixxynth::WeirdVerbFx::pWidth] = width;
            p[mixxynth::WeirdVerbFx::pMix] = 1.0f;

            juce::AudioBuffer<float> buf (2, blockLen);
            buf.clear();

            for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            verb.process (buf, p);

            const int start = static_cast<int> (sr * 0.3);
            const int len = static_cast<int> (sr * 0.5);
            double ll = 0.0, rr = 0.0, lr = 0.0;

            for (int i = 0; i < len; ++i)
            {
                const double l = buf.getSample (0, start + i);
                const double r = buf.getSample (1, start + i);
                ll += l * l;
                rr += r * r;
                lr += l * r;
            }

            const double denom = std::sqrt (juce::jmax (1.0e-18, ll * rr));

            return lr / denom;
        };

        const double mono = correlation (0.0f);
        const double wide = correlation (1.0f);

        ok = check (mono > 0.99, "width at minimum collapses the wet to mono", mono) && ok;
        ok = check (wide < 0.9, "width at maximum decorrelates the channels", wide) && ok;
    }

    // --- 25c. Shimmer transposes an octave up ---
    {
        // One frequency bin, by Goertzel: enough to ask whether the shimmer is
        // really transposing rather than merely adding noise.
        auto octaveContent = [] (float shimmerAmount)
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pDecay] = 0.8f;
            p[mixxynth::WeirdVerbFx::pDiffusion] = 0.7f;
            p[mixxynth::WeirdVerbFx::pModDepth] = 0.35f;
            p[mixxynth::WeirdVerbFx::pHighCut] = 1.0f;
            p[mixxynth::WeirdVerbFx::pWidth] = 0.5f;
            p[mixxynth::WeirdVerbFx::pShimmer] = shimmerAmount;
            p[mixxynth::WeirdVerbFx::pMix] = 1.0f;

            juce::AudioBuffer<float> buf (2, blockLen);

            for (int i = 0; i < blockLen; ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            verb.process (buf, p);

            const int start = static_cast<int> (sr * 0.5);
            const int len = static_cast<int> (sr * 0.5);
            const double w = 2.0 * juce::MathConstants<double>::pi * 880.0 / sr;
            const double coeff = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0;

            for (int i = 0; i < len; ++i)
            {
                const double s0 = buf.getSample (0, start + i) + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            return std::sqrt (juce::jmax (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / len;
        };

        const double off = octaveContent (0.0f);
        const double on = octaveContent (0.9f);

        ok = check (on > off * 3.0, "shimmer adds octave-up content",
                    on / juce::jmax (1.0e-12, off)) && ok;
    }

    // --- 26. Pre-delay and mix do what they say ---
    {
        mixxynth::WeirdVerbFx verb;
        verb.prepare (sr, blockLen, 2);

        float p[mixxynth::kMaxFxParams] {};
        p[mixxynth::WeirdVerbFx::pDecay] = 1.0f;    // the infinite tail
        p[mixxynth::WeirdVerbFx::pDamping] = 0.0f;  // fully open: least loss, hardest case
        p[mixxynth::WeirdVerbFx::pBass] = 1.0f;     // and the longest low end too
        p[mixxynth::WeirdVerbFx::pMix] = 1.0f;

        // Sustained input is the case that would grow without bound if the loop
        // were not self-limiting.
        juce::AudioBuffer<float> buf (2, blockLen);

        for (int i = 0; i < blockLen; ++i)
        {
            const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f
                                              * static_cast<float> (i) / static_cast<float> (sr));
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }

        verb.process (buf, p);

        float peak = 0.0f;
        bool finite = true;

        for (int ch = 0; ch < 2; ++ch)
        {
            peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockLen));

            for (int i = 0; i < blockLen; ++i)
                if (! std::isfinite (buf.getSample (ch, i)))
                    finite = false;
        }

        const double lateRms = rmsOf (buf, static_cast<int> (sr * 0.7), static_cast<int> (sr * 0.3));

        ok = check (finite && peak <= 1.0f, "infinite decay stays finite and bounded", peak) && ok;
        ok = check (lateRms > 0.0005, "infinite decay still sustains at the end", lateRms) && ok;

        // A finite decay must actually die away, or Decay is doing nothing.
        mixxynth::WeirdVerbFx shortVerb;
        shortVerb.prepare (sr, blockLen, 2);

        float q[mixxynth::kMaxFxParams] {};
        q[mixxynth::WeirdVerbFx::pDecay] = 0.0f;
        q[mixxynth::WeirdVerbFx::pDamping] = 0.5f;
        q[mixxynth::WeirdVerbFx::pMix] = 1.0f;

        juce::AudioBuffer<float> shortBuf (2, blockLen);
        shortBuf.clear();

        for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
        {
            const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                              * static_cast<float> (i) / static_cast<float> (sr));
            shortBuf.setSample (0, i, v);
            shortBuf.setSample (1, i, v);
        }

        shortVerb.process (shortBuf, q);

        const double late = rmsOf (shortBuf, static_cast<int> (sr * 0.6), static_cast<int> (sr * 0.3));

        ok = check (late < 0.0005, "a short decay dies away", late) && ok;
    }

    // --- 26. Pre-delay and mix do what they say ---
    {
        // Mix at zero must leave the signal untouched.
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pMix] = 0.0f;

            juce::AudioBuffer<float> buf (2, 512);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.3f * std::sin (0.02f * static_cast<float> (i)));

            juce::AudioBuffer<float> before (buf);
            verb.process (buf, p);

            double worst = 0.0;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (ch, i)
                                                                               - before.getSample (ch, i))));

            ok = check (worst < 1.0e-7, "mix at zero is dry", worst) && ok;
        }

        // Pre-delay must hold the wet signal back, so a wet-only reverb produces
        // almost nothing for the first part of the pre-delay window.
        {
            mixxynth::WeirdVerbFx verb;
            verb.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::WeirdVerbFx::pPreDelay] = 0.5f; // ~125 ms
            p[mixxynth::WeirdVerbFx::pDecay] = 0.6f;
            p[mixxynth::WeirdVerbFx::pDamping] = 0.8f;
            p[mixxynth::WeirdVerbFx::pMix] = 1.0f;      // wet only

            juce::AudioBuffer<float> buf (2, blockLen);

            for (int i = 0; i < blockLen; ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            verb.process (buf, p);

            const double early = rmsOf (buf, 0, static_cast<int> (sr * 0.03));
            const double later = rmsOf (buf, static_cast<int> (sr * 0.2), static_cast<int> (sr * 0.3));

            ok = check (early < later * 0.25, "pre-delay holds the wet signal back", early) && ok;
        }
    }

    // --- 27. Convolution reverb ---
    {
        // The module rebuilds its impulse on a message-thread timer, and a test has
        // no message loop, so the load is driven directly. juce::dsp::Convolution
        // loads asynchronously, so prepare() afterwards is what forces it to
        // complete - the JUCE-documented way to make a load deterministic.
        auto loadAndRender = [] (int irChoice, float length, float mix, float preDelay,
                                 juce::AudioBuffer<float>& buf)
        {
            static mixxynth::ConvolutionFx verb;
            static bool initialised = false;

            if (! initialised)
            {
                verb.prepare (sr, blockLen, 2);
                initialised = true;
            }

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::ConvolutionFx::pIr] = static_cast<float> (irChoice)
                                                / static_cast<float> (mixxynth::ConvolutionFx::getNumBuiltInIrs());
            p[mixxynth::ConvolutionFx::pLength] = length;
            p[mixxynth::ConvolutionFx::pPreDelay] = preDelay;
            p[mixxynth::ConvolutionFx::pLowCut] = 0.0f;
            p[mixxynth::ConvolutionFx::pHighCut] = 1.0f;
            p[mixxynth::ConvolutionFx::pWidth] = 0.5f;
            p[mixxynth::ConvolutionFx::pMix] = mix;
            p[mixxynth::ConvolutionFx::pNormalise] = 1.0f;

            // One pass records the request, then the load is forced through.
            juce::AudioBuffer<float> scratch (2, blockLen);
            scratch.clear();
            verb.process (scratch, p);
            verb.refreshImpulse();
            verb.prepare (sr, blockLen, 2);
            verb.reset();

            verb.process (buf, p);
        };

        // Every built-in impulse produces a finite, bounded, audible tail.
        for (int ir = 0; ir < mixxynth::ConvolutionFx::getNumBuiltInIrs(); ++ir)
        {
            juce::AudioBuffer<float> buf (2, blockLen);
            buf.clear();

            for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            loadAndRender (ir, 1.0f, 1.0f, 0.0f, buf);

            const double tail = rmsOf (buf, static_cast<int> (sr * 0.1), static_cast<int> (sr * 0.3));
            float peak = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
                peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockLen));

            ok = check (std::isfinite (tail) && tail > 0.0002 && peak <= 1.0f,
                        "convolution built-in IR audible and bounded",
                        tail) && ok;
        }
    }

    // --- 27b. Convolution: mix, Length, pre-delay, and a missing file ---
    {
        auto render = [] (int irChoice, float length, float mix, float preDelay,
                          juce::AudioBuffer<float>& buf)
        {
            static mixxynth::ConvolutionFx verb;
            static bool initialised = false;

            if (! initialised)
            {
                verb.prepare (sr, blockLen, 2);
                initialised = true;
            }

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::ConvolutionFx::pIr] = static_cast<float> (irChoice)
                                                / static_cast<float> (mixxynth::ConvolutionFx::getNumBuiltInIrs());
            p[mixxynth::ConvolutionFx::pLength] = length;
            p[mixxynth::ConvolutionFx::pPreDelay] = preDelay;
            p[mixxynth::ConvolutionFx::pHighCut] = 1.0f;
            p[mixxynth::ConvolutionFx::pWidth] = 0.5f;
            p[mixxynth::ConvolutionFx::pMix] = mix;
            p[mixxynth::ConvolutionFx::pNormalise] = 1.0f;

            juce::AudioBuffer<float> scratch (2, blockLen);
            scratch.clear();
            verb.process (scratch, p);
            verb.refreshImpulse();
            verb.prepare (sr, blockLen, 2);
            verb.reset();

            verb.process (buf, p);
        };

        auto burst = [] (juce::AudioBuffer<float>& buf)
        {
            buf.clear();

            for (int i = 0; i < static_cast<int> (sr * 0.05); ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
        };

        // Mix at zero is dry.
        {
            juce::AudioBuffer<float> buf (2, blockLen);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockLen; ++i)
                    buf.setSample (ch, i, 0.3f * std::sin (0.02f * static_cast<float> (i)));

            juce::AudioBuffer<float> before (buf);
            render (0, 1.0f, 0.0f, 0.0f, buf);

            double worst = 0.0;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockLen; ++i)
                    worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (ch, i)
                                                                               - before.getSample (ch, i))));

            ok = check (worst < 1.0e-7, "convolution mix at zero is dry", worst) && ok;
        }

        // Length trims the tail: a short impulse cannot ring as long as a long one.
        {
            auto lateEnergy = [&burst, &render] (float length)
            {
                juce::AudioBuffer<float> buf (2, blockLen);
                burst (buf);

                // Cathedral, the longest impulse, so the trim has room to matter.
                render (4, length, 1.0f, 0.0f, buf);
                return rmsOf (buf, static_cast<int> (sr * 0.6), static_cast<int> (sr * 0.4));
            };

            const double full = lateEnergy (1.0f);
            const double trimmed = lateEnergy (0.05f);

            ok = check (trimmed < full * 0.5, "convolution Length trims the tail",
                        trimmed / juce::jmax (1.0e-12, full)) && ok;
        }

        // Pre-delay holds the wet signal back.
        {
            juce::AudioBuffer<float> buf (2, blockLen);

            for (int i = 0; i < blockLen; ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            render (0, 1.0f, 1.0f, 0.5f, buf);

            const double early = rmsOf (buf, 0, static_cast<int> (sr * 0.01));
            const double later = rmsOf (buf, static_cast<int> (sr * 0.15), static_cast<int> (sr * 0.3));

            ok = check (early < later * 0.5, "convolution pre-delay holds the wet back", early) && ok;
        }

        // A custom selection with no file loaded must be silent - not the previous
        // impulse, and not a crash.
        {
            juce::AudioBuffer<float> buf (2, blockLen);

            for (int i = 0; i < blockLen; ++i)
            {
                const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f
                                                  * static_cast<float> (i) / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            render (mixxynth::ConvolutionFx::customIrChoice(), 1.0f, 1.0f, 0.0f, buf);

            float peak = 0.0f;
            bool finite = true;

            for (int ch = 0; ch < 2; ++ch)
            {
                peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockLen));

                for (int i = 0; i < blockLen; ++i)
                    if (! std::isfinite (buf.getSample (ch, i)))
                        finite = false;
            }

            ok = check (finite && peak < 0.01f,
                        "convolution with no file loaded is silent, not stale",
                        peak) && ok;
        }
    }

    // --- Drift and velocity (M6) ---
    //
    // Both were gaps rather than bugs. Velocity was applied as a bare `* velocity` on the
    // voice output, so the sensitivity was permanently 100% and could not be reduced;
    // drift did not exist at all.
    {
        // A plain sine carrier with no FM, so any difference between two renders is the
        // pitch or the gain and nothing else.
        auto sinePatch = []
        {
            MiXXFmVoice::VoiceParams p;

            for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            {
                p.ratio[i] = 1.0f;
                p.level[i] = 0.0f;
                p.carrier[i] = (i == 0);
            }

            p.level[0] = 0.8f;
            p.index = 0.0f;
            p.master = 0.8f;
            return p;
        };

        const auto sine = sinePatch();

        auto maxDiff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            float d = 0.0f;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    d = juce::jmax (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

            return d;
        };

        auto windowDiff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b,
                              int from, int len)
        {
            float d = 0.0f;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                for (int i = from; i < from + len; ++i)
                    d = juce::jmax (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

            return d;
        };

        // Drift advances once per BLOCK, so this has to render many blocks rather than one
        // long one: a single renderRange call would advance the walk exactly once and the
        // test would be measuring almost nothing.
        constexpr int driftBlock = 512;
        constexpr int driftBlocks = 200;

        auto renderDrift = [&] (float amount, juce::uint32 seed)
        {
            MiXXFmVoice v;
            v.setSampleRate (sr);
            v.setDriftSeed (seed);
            v.noteOn (69, 1.0f);

            auto p = sine;
            p.drift = amount;

            juce::AudioBuffer<float> buf (2, driftBlock * driftBlocks);
            buf.clear();

            for (int b = 0; b < driftBlocks; ++b)
                v.renderAdd (buf, b * driftBlock, driftBlock, p);

            return buf;
        };

        const auto driftOff1 = renderDrift (0.0f, 1u);
        const auto driftOff2 = renderDrift (0.0f, 987654321u);
        const auto driftOn1  = renderDrift (1.0f, 1u);
        const auto driftOn2  = renderDrift (1.0f, 987654321u);

        // With drift off the seed must make no difference at all. This is what stops
        // "drift at zero" from quietly being a small amount of drift.
        ok = check (maxDiff (driftOff1, driftOff2) == 0.0f,
                    "drift off: seed changes nothing", maxDiff (driftOff1, driftOff2)) && ok;

        ok = check (maxDiff (driftOff1, driftOn1) > 0.01f,
                    "drift moves the pitch", maxDiff (driftOff1, driftOn1)) && ok;

        // Two voices with different seeds must wander differently. If they did not, a
        // unison stack would drift as one voice - which is detune, not drift.
        ok = check (maxDiff (driftOn1, driftOn2) > 0.01f,
                    "drift is per voice, not per patch", maxDiff (driftOn1, driftOn2)) && ok;

        // The one that matters most: it has to BUILD UP rather than appear instantly. A
        // drift that started at full offset would pass every check above while being
        // nothing more than a random fixed detune.
        const int total = driftBlock * driftBlocks;
        const int window = total / 8;
        const float early = windowDiff (driftOff1, driftOn1, 0, window);
        const float late = windowDiff (driftOff1, driftOn1, total - window, window);

        ok = check (late > early * 3.0f, "drift builds up rather than jumping",
                    static_cast<double> (late / juce::jmax (1.0e-9f, early))) && ok;

        // --- velocity ---
        auto renderVel = [&] (const MiXXFmVoice::VoiceParams& base, float vel)
        {
            MiXXFmVoice v;
            v.setSampleRate (sr);
            v.noteOn (69, vel);

            juce::AudioBuffer<float> buf (2, blockLen);
            buf.clear();
            v.renderAdd (buf, 0, blockLen, base);
            return buf;
        };

        auto rmsOfBuffer = [] (const juce::AudioBuffer<float>& b)
        {
            return rmsOf (b, static_cast<int> (sr * 0.1), static_cast<int> (sr * 0.4));
        };

        // velToLevel = 1 is exactly what the voice did before this control existed, so the
        // default has to reproduce it - otherwise the change would have altered the
        // loudness of every patch that ever used velocity.
        auto levelFull = sine;
        levelFull.velToLevel = 1.0f;

        const double loudFull = rmsOfBuffer (renderVel (levelFull, 1.0f));
        const double softFull = rmsOfBuffer (renderVel (levelFull, 0.25f));

        ok = check (softFull < loudFull * 0.5, "velocity scales the level by default",
                    softFull / juce::jmax (1.0e-12, loudFull)) && ok;

        // ...and 0 removes the response entirely, which is what makes it a control rather
        // than a constant.
        auto levelNone = sine;
        levelNone.velToLevel = 0.0f;

        const double loudNone = rmsOfBuffer (renderVel (levelNone, 1.0f));
        const double softNone = rmsOfBuffer (renderVel (levelNone, 0.25f));

        ok = check (std::abs (softNone - loudNone) < 1.0e-6,
                    "velToLevel 0 ignores velocity entirely",
                    std::abs (softNone - loudNone)) && ok;

        // velToIndex needs a patch with FM in it: with no modulation the index multiplies
        // zero, so the control would look dead while being wired perfectly.
        auto fmPatch = sine;
        fmPatch.mod[0][1] = 0.8f;
        fmPatch.level[1] = 0.8f;
        fmPatch.carrier[1] = true;
        fmPatch.carrier[0] = false;
        fmPatch.index = 4.0f;

        auto idxOn = fmPatch;
        idxOn.velToLevel = 0.0f;
        idxOn.velToIndex = 1.0f;

        auto idxOff = fmPatch;
        idxOff.velToLevel = 0.0f;
        idxOff.velToIndex = 0.0f;

        // At full velocity the index scale is exactly 1, so this must be a no-op. A sharp
        // boundary like that is what catches an off-by-one in the scaling.
        const double idxFull = rmsOfBuffer (renderVel (idxOn, 1.0f));
        const double noIdxFull = rmsOfBuffer (renderVel (idxOff, 1.0f));

        ok = check (std::abs (idxFull - noIdxFull) < 1.0e-6,
                    "velToIndex is a no-op at full velocity",
                    std::abs (idxFull - noIdxFull)) && ok;

        // ...and it must do something at low velocity, or it is not wired up at all.
        const double idxSoft = rmsOfBuffer (renderVel (idxOn, 0.25f));
        const double noIdxSoft = rmsOfBuffer (renderVel (idxOff, 0.25f));

        ok = check (std::abs (idxSoft - noIdxSoft) > 1.0e-5,
                    "velToIndex changes the tone at low velocity",
                    std::abs (idxSoft - noIdxSoft)) && ok;
    }

    // --- The published modulated values (M6 step 5) ---
    //
    // The chain that makes a modulated control move: totals -> apply -> read back. Checked
    // end to end, because each link is useless without the others and a break in any of
    // them looks exactly like "the display does nothing" - which is indistinguishable from
    // the display never having been wired up.
    {
        mixxynth::ModParams m;
        m.slots[0].source = mixxynth::srcMacro1;
        m.slots[0].dest = mixxynth::dstFmIndex;
        m.slots[0].depth = 0.5f;

        std::array<float, mixxynth::kNumModSources> sources {};
        sources[static_cast<size_t> (mixxynth::srcMacro1)] = 1.0f;

        const auto totals = mixxynth::modTotals (sources, m);

        ok = check (std::abs (totals[static_cast<size_t> (mixxynth::dstFmIndex)] - 0.5f) < 1.0e-6f,
                    "mod totals: the slot's amount reaches its destination",
                    totals[static_cast<size_t> (mixxynth::dstFmIndex)]) && ok;

        // Every other destination must be untouched. A single-destination check would pass
        // even if the totals pass were writing every slot to the same place.
        float stray = 0.0f;

        for (int i = 0; i < mixxynth::kNumModDests; ++i)
            if (i != mixxynth::dstFmIndex)
                stray = juce::jmax (stray, std::abs (totals[static_cast<size_t> (i)]));

        ok = check (stray == 0.0f, "mod totals: nothing lands on another destination", stray) && ok;

        MiXXFmVoice::VoiceParams vp;
        vp.index = 0.25f;
        mixxynth::applyModulation (totals, vp);

        ok = check (std::abs (vp.index - 1.25f) < 1.0e-6f,
                    "mod apply: FM Index moves by depth x 2", vp.index) && ok;

        std::array<float, mixxynth::kNumModDests> published;
        mixxynth::publishModulatedValues (vp, published);

        ok = check (std::abs (published[static_cast<size_t> (mixxynth::dstFmIndex)] - vp.index) < 1.0e-6f,
                    "mod publish: the readout is what was applied",
                    published[static_cast<size_t> (mixxynth::dstFmIndex)]) && ok;

        // A destination nobody drove must read back as its BASE, not as zero. The editor
        // asks for all of them, and zero would draw a full operator as a silent one.
        MiXXFmVoice::VoiceParams plain;
        plain.level[2] = 0.7f;
        mixxynth::publishModulatedValues (plain, published);

        ok = check (std::abs (published[static_cast<size_t> (mixxynth::dstOp3Level)] - 0.7f) < 1.0e-6f,
                    "mod publish: an unmodulated value reads back as its base",
                    published[static_cast<size_t> (mixxynth::dstOp3Level)]) && ok;
    }

    // --- Per-operator filters (M6) ---
    //
    // The routing is the part worth testing hardest. It decides whether the filter shapes
    // what you HEAR or what the operator DOES, and getting it backwards still produces
    // sound - just the wrong sound, which is the hardest kind of bug to notice by ear.
    {
        auto maxDiff2 = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            float d = 0.0f;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    d = juce::jmax (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

            return d;
        };

        auto windowDiff2 = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b,
                               int from, int len)
        {
            float d = 0.0f;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                for (int i = from; i < from + len; ++i)
                    d = juce::jmax (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

            return d;
        };

        // A two-operator pair: op 1 modulates op 2, op 2 is the carrier.
        auto fmPair = []
        {
            MiXXFmVoice::VoiceParams p;
            p.mod[0][1] = 0.9f;
            p.level[1] = 0.8f;
            p.index = 4.0f;
            p.master = 0.8f;

            for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            {
                p.ratio[i] = 1.0f;
                p.carrier[i] = (i == 1);
            }

            return p;
        };

        auto renderPatch = [&] (const MiXXFmVoice::VoiceParams& base)
        {
            MiXXFmVoice v;
            v.setSampleRate (sr);
            v.noteOn (69, 1.0f);

            juce::AudioBuffer<float> buf (2, blockLen);
            buf.clear();
            v.renderAdd (buf, 0, blockLen, base);
            return buf;
        };

        // Holds the filter open, so the measurement is of the filter and not of the sweep.
        auto lowPassHeld = [] (MiXXFmVoice::VoiceParams& p, int op)
        {
            p.opFilterOn[op] = true;
            p.opFilterType[op] = 0;
            p.opFilterCutoff[op] = 200.0f;
            p.opFilterReso[op] = 0.2f;
            p.opFilterSustain[op] = 1.0f;
        };

        const auto plain = renderPatch (fmPair());

        auto modFiltered = fmPair();
        lowPassHeld (modFiltered, 0);
        modFiltered.opFilterRouting[0] = 1;   // the modulator only

        const auto filteredMod = renderPatch (modFiltered);

        ok = check (maxDiff2 (plain, filteredMod) > 0.01f,
                    "filter on a modulator changes the tone",
                    maxDiff2 (plain, filteredMod)) && ok;

        auto outFiltered = fmPair();
        lowPassHeld (outFiltered, 1);
        outFiltered.opFilterRouting[1] = 0;   // the output only

        const auto filteredOut = renderPatch (outFiltered);

        ok = check (maxDiff2 (plain, filteredOut) > 0.01f,
                    "filter on a carrier output changes the tone",
                    maxDiff2 (plain, filteredOut)) && ok;

        // The two must not be the same thing. If routing were ignored they would be, and
        // the control would be decorative.
        ok = check (maxDiff2 (filteredMod, filteredOut) > 0.01f,
                    "Out and Mod are genuinely different routes",
                    maxDiff2 (filteredMod, filteredOut)) && ok;

        // An operator that is neither a carrier nor a modulator of anything downstream has
        // no path to the output, so its filter cannot be audible. This is the check that
        // catches a filter being applied somewhere it does not belong.
        auto idleFiltered = fmPair();
        lowPassHeld (idleFiltered, 5);

        ok = check (maxDiff2 (plain, renderPatch (idleFiltered)) == 0.0f,
                    "an idle operator's filter cannot be heard",
                    maxDiff2 (plain, renderPatch (idleFiltered))) && ok;

        // And the envelope must actually move the cutoff, which means moving the BRIGHTNESS.
        //
        // Measured in zero crossings, not in how much the render differs from the plain one:
        // a darker filter removes more of the signal, so it differs MORE, and using
        // difference as a proxy for brightness gets the answer exactly backwards. Zero
        // crossings measure the thing itself and do not care about amplitude.
        auto zeroCrossings = [] (const juce::AudioBuffer<float>& b, int from, int len)
        {
            int n = 0;

            for (int i = from + 1; i < from + len; ++i)
                if ((b.getSample (0, i - 1) < 0.0f) != (b.getSample (0, i) < 0.0f))
                    ++n;

            return n;
        };

        auto swept = fmPair();
        swept.opFilterOn[0] = true;
        swept.opFilterType[0] = 0;
        swept.opFilterCutoff[0] = 150.0f;
        swept.opFilterAttack[0] = 0.4f;
        swept.opFilterSustain[0] = 1.0f;
        swept.opFilterRouting[0] = 1;

        const auto sweptBuf = renderPatch (swept);
        const int earlyBright = zeroCrossings (sweptBuf, 0, static_cast<int> (sr * 0.05));
        const int lateBright  = zeroCrossings (sweptBuf, static_cast<int> (sr * 0.7),
                                               static_cast<int> (sr * 0.2));

        ok = check (lateBright > earlyBright, "the filter envelope sweeps the cutoff open",
                    lateBright / juce::jmax (1.0, static_cast<double> (earlyBright))) && ok;
    }

    // --- 28. Juno chorus: the LFO is a triangle, not a cosine ---
    //
    // The single most important detail in the effect, and the one a corner check
    // cannot see: a triangle and a cosine pass through exactly the same four
    // points at phase 0, 0.25, 0.5 and 0.75 (+1, 0, -1, 0). They differ in the
    // MIDDLE of each segment, where the triangle is straight and the cosine is not.
    // So the corners are checked too, but the midpoints are the real test.
    {
        const float corners = std::abs (mixxynth::JunoChorusFx::triangle (0.0f) - 1.0f)
                                + std::abs (mixxynth::JunoChorusFx::triangle (0.25f))
                                + std::abs (mixxynth::JunoChorusFx::triangle (0.5f) + 1.0f)
                                + std::abs (mixxynth::JunoChorusFx::triangle (0.75f));

        ok = check (corners < 1.0e-5f, "Juno LFO corners at +1, 0, -1, 0", corners) && ok;

        // A triangle is at 0.5 halfway from +1 to 0; a cosine is at 0.707.
        const float mid = mixxynth::JunoChorusFx::triangle (0.125f);
        const float cosine = std::cos (2.0f * juce::MathConstants<float>::pi * 0.125f);

        ok = check (std::abs (mid - 0.5f) < 1.0e-5f && std::abs (cosine - mid) > 0.2f,
                    "Juno LFO is straight between the corners, not a cosine", mid) && ok;
    }

    // --- 29. Juno chorus: the mode table is the measured hardware ---
    //
    // These are the numbers the emulation stands on, so they are asserted rather
    // than trusted. The relationships matter as much as the values: I and II share
    // a depth and differ only in rate, and I+II is different in EVERY column.
    {
        using Chorus = mixxynth::JunoChorusFx;

        const auto& one = Chorus::specFor (Chorus::modeI);
        const auto& two = Chorus::specFor (Chorus::modeII);
        const auto& both = Chorus::specFor (Chorus::modeBoth);

        ok = check (std::abs (one.rateHz - 0.513f) < 1.0e-5f
                      && std::abs (two.rateHz - 0.863f) < 1.0e-5f
                      && std::abs (both.rateHz - 9.75f) < 1.0e-4f,
                    "Juno mode rates are the measured ones", one.rateHz) && ok;

        // 1.66 to 5.35 ms is a centre of 3.505 with a depth of 1.845.
        ok = check (std::abs (one.centreMs - 3.505f) < 1.0e-4f
                      && std::abs (one.depthMs - 1.845f) < 1.0e-4f
                      && std::abs (one.centreMs - one.depthMs - 1.66f) < 1.0e-3f
                      && std::abs (one.centreMs + one.depthMs - 5.35f) < 1.0e-3f,
                    "Juno mode I sweeps 1.66 to 5.35 ms", one.centreMs) && ok;

        ok = check (std::abs (one.depthMs - two.depthMs) < 1.0e-6f && two.rateHz > one.rateHz,
                    "I and II share a depth and differ in rate", two.rateHz) && ok;

        ok = check (both.rateHz > two.rateHz * 5.0f
                      && both.depthMs < two.depthMs * 0.2f
                      && both.mono && ! one.mono && ! two.mono,
                    "I+II is faster, shallower and mono", both.depthMs) && ok;
    }

    // --- 30. Juno chorus: I and II widen, I+II cannot ---
    //
    // The measurement the naive implementation fails. On the hardware the two BBD
    // lines share one triangle LFO with the right channel's modulation INVERTED, so
    // modes I and II widen the signal. Mode I+II moves both lines together, so it
    // cannot widen at all - which is why the hardware's own documentation calls it
    // mono, and why it sounds like a rotary speaker rather than a chorus.
    //
    // Feeding both channels the same signal makes that directly observable: with
    // anti-phase modulation the two channels are delayed by different amounts and
    // diverge; in mono they are delayed identically and stay bit-identical.
    {
        auto spreadFor = [&] (int mode)
        {
            mixxynth::JunoChorusFx chorus;
            chorus.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::JunoChorusFx::pMode] = static_cast<float> (mode) / 2.0f;
            p[mixxynth::JunoChorusFx::pRate] = 0.5f;   // 1.0x - the hardware rate
            p[mixxynth::JunoChorusFx::pDepth] = 0.5f;  // 1.0x - the hardware depth
            p[mixxynth::JunoChorusFx::pTone] = 0.70f;
            p[mixxynth::JunoChorusFx::pMix] = 1.0f;    // wet only

            juce::AudioBuffer<float> buf (2, blockLen);
            double worst = 0.0;

            // Several blocks, so the LFO has run and the delay slew has settled. The
            // LFO starts at +1, so the two lines are already at opposite extremes of
            // the sweep from the first sample - the divergence does not need time to
            // develop, but the mono case needs time to prove it stays at zero.
            for (int b = 0; b < 16; ++b)
            {
                for (int i = 0; i < blockLen; ++i)
                {
                    const float v = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * 220.0f * static_cast<float> (i)
                                                       / static_cast<float> (sr));
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                chorus.process (buf, p);

                for (int i = 0; i < blockLen; ++i)
                    worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (0, i))
                                                           - buf.getSample (1, i)));
            }

            return worst;
        };

        const double spreadOne = spreadFor (mixxynth::JunoChorusFx::modeI);
        const double spreadTwo = spreadFor (mixxynth::JunoChorusFx::modeII);
        const double spreadBoth = spreadFor (mixxynth::JunoChorusFx::modeBoth);

        ok = check (spreadOne > 0.01 && spreadTwo > 0.01,
                    "Juno I and II widen: the two lines are anti-phase", spreadOne) && ok;

        // Bit-identical, not merely similar: in mono both lines get the same input,
        // the same delay and the same filter, so nothing can make them differ.
        ok = check (spreadBoth < 1.0e-9,
                    "Juno I+II is mono: both lines move together", spreadBoth) && ok;
    }

    // --- 31. Juno chorus: every mode is bounded and audible ---
    {
        const char* names[3] = { "Juno mode I audible and bounded",
                                 "Juno mode II audible and bounded",
                                 "Juno mode I+II audible and bounded" };

        for (int mode = 0; mode < 3; ++mode)
        {
            mixxynth::JunoChorusFx chorus;
            chorus.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::JunoChorusFx::pMode] = static_cast<float> (mode) / 2.0f;
            p[mixxynth::JunoChorusFx::pRate] = 0.5f;
            p[mixxynth::JunoChorusFx::pDepth] = 0.5f;
            p[mixxynth::JunoChorusFx::pTone] = 0.70f;
            p[mixxynth::JunoChorusFx::pMix] = 1.0f; // wet only

            juce::AudioBuffer<float> buf (2, blockLen);
            float peak = 0.0f;

            for (int b = 0; b < 8; ++b)
            {
                for (int i = 0; i < blockLen; ++i)
                {
                    const float v = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * 330.0f * static_cast<float> (i)
                                                       / static_cast<float> (sr));
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                chorus.process (buf, p);

                for (int ch = 0; ch < 2; ++ch)
                    peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockLen));
            }

            // Bounded: a chorus has no feedback, so it cannot grow.
            ok = check (std::isfinite (peak) && peak > 0.05f && peak <= 1.0f,
                        names[mode], peak) && ok;
        }
    }

    // --- 32. Juno chorus: Tone darkens the wet path, and mode changes do not step ---
    {
        // Tone at the bottom should pass noticeably less high frequency than at the
        // top.
        //
        // Measured as the total CURVATURE of the output - the sum of the second
        // difference, |x[n] - 2x[n-1] + x[n-2]|. Not zero crossings, which was the
        // first attempt and does not work: a low-passed square still crosses zero
        // twice per cycle, so the crossings are set by the fundamental and barely
        // move. Not the first difference either, which is worse than it looks - the
        // total variation of a square and of its own fundamental are both 4*f*A, so
        // it cannot tell them apart at all. Curvature is what distinguishes a sharp
        // edge from a rounded one, which is exactly what Tone does.
        auto curvatureForTone = [&] (float tone)
        {
            mixxynth::JunoChorusFx chorus;
            chorus.prepare (sr, blockLen, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[mixxynth::JunoChorusFx::pMode] = 0.0f;
            p[mixxynth::JunoChorusFx::pRate] = 0.5f;
            p[mixxynth::JunoChorusFx::pDepth] = 0.5f;
            p[mixxynth::JunoChorusFx::pTone] = tone;
            p[mixxynth::JunoChorusFx::pMix] = 1.0f;

            juce::AudioBuffer<float> buf (2, blockLen);
            double curvature = 0.0;

            for (int b = 0; b < 6; ++b)
            {
                for (int i = 0; i < blockLen; ++i)
                {
                    // A square-ish input: lots of high frequency to lose.
                    const float phase = std::fmod (660.0f * static_cast<float> (i)
                                                     / static_cast<float> (sr), 1.0f);
                    const float v = phase < 0.5f ? 0.35f : -0.35f;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                chorus.process (buf, p);

                if (b >= 4)
                {
                    const auto* d = buf.getReadPointer (0);

                    for (int i = 2; i < blockLen; ++i)
                        curvature += std::abs (static_cast<double> (d[i])
                                                 - 2.0 * static_cast<double> (d[i - 1])
                                                 + static_cast<double> (d[i - 2]));
                }
            }

            return curvature;
        };

        const double dark = curvatureForTone (0.0f);
        const double bright = curvatureForTone (1.0f);

        ok = check (dark < bright * 0.5, "Juno Tone darkens the BBD wet path",
                    dark / juce::jmax (1.0, bright)) && ok;
    }

    // --- 33. Juno chorus: a mode change does not step the delay ---
    //
    // Switching I to I+II moves the centre barely at all but drops the depth by a
    // factor of nine. An unslewed depth jump moves the read head 70-odd samples in
    // one step, which is a full-scale discontinuity rather than a slope.
    {
        mixxynth::JunoChorusFx chorus;
        chorus.prepare (sr, blockLen, 2);

        float p[mixxynth::kMaxFxParams] {};
        p[mixxynth::JunoChorusFx::pMode] = 0.0f;  // start on I
        p[mixxynth::JunoChorusFx::pRate] = 0.5f;
        p[mixxynth::JunoChorusFx::pDepth] = 0.5f;
        p[mixxynth::JunoChorusFx::pTone] = 0.70f;
        p[mixxynth::JunoChorusFx::pMix] = 1.0f;

        juce::AudioBuffer<float> buf (2, blockLen);

        auto fill = [&]
        {
            for (int i = 0; i < blockLen; ++i)
            {
                const float v = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                   * 220.0f * static_cast<float> (i)
                                                   / static_cast<float> (sr));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
        };

        // Settle on I.
        for (int b = 0; b < 8; ++b)
        {
            fill();
            chorus.process (buf, p);
        }

        p[mixxynth::JunoChorusFx::pMode] = 1.0f;

        float worstJump = 0.0f;

        for (int b = 0; b < 8; ++b)
        {
            fill();
            chorus.process (buf, p);

            const auto* d = buf.getReadPointer (0);

            for (int i = 1; i < blockLen; ++i)
                worstJump = juce::jmax (worstJump, std::abs (d[i] - d[i - 1]));
        }

        // The input's own slope at 220 Hz is about 0.0125, so this leaves room for
        // the slope plus the slew while still failing a hard step.
        ok = check (worstJump < 0.05f, "a Juno mode change does not step the delay",
                    worstJump) && ok;
    }

    // --- 34. Juno chorus: the rack builds it, and its descriptor is well formed ---
    {
        const auto desc = mixxynth::FxRack::descriptorFor (mixxynth::fxJunoChorus);

        ok = check (juce::String (desc.name) == "Juno Chorus"
                      && desc.numParams == mixxynth::JunoChorusFx::kNumParams
                      && desc.params[mixxynth::JunoChorusFx::pMode].isChoice
                      && desc.params[mixxynth::JunoChorusFx::pMode].choices.size() == 3
                      && ! desc.hasFile,
                    "the Juno chorus descriptor is complete",
                    static_cast<double> (desc.numParams)) && ok;

        // Both trims default to the centre, which is exactly 1.0x - the hardware
        // value. That is what makes a freshly loaded module the emulation rather
        // than a chorus that happens to be nearby.
        ok = check (std::abs (desc.params[mixxynth::JunoChorusFx::pRate].defaultValue - 0.5f) < 1.0e-6f
                      && std::abs (desc.params[mixxynth::JunoChorusFx::pDepth].defaultValue - 0.5f) < 1.0e-6f,
                    "the Juno trims default to the hardware value",
                    desc.params[mixxynth::JunoChorusFx::pRate].defaultValue) && ok;

        // And the type name the choice parameter shows matches the enum's order,
        // which is what the slot's saved index depends on.
        ok = check (juce::String (mixxynth::fxTypeName (mixxynth::fxJunoChorus)) == "Juno Chorus",
                    "the Juno type name matches its enum", 1.0) && ok;

        mixxynth::FxRack rack;
        rack.prepare (sr, blockLen, 2);
        rack.setSlotType (0, mixxynth::fxJunoChorus);

        ok = check (rack.hasInstance (0, mixxynth::fxJunoChorus)
                      && rack.getSlotType (0) == mixxynth::fxJunoChorus,
                    "the rack builds the Juno chorus on demand", 1.0) && ok;
    }

    // --- 35. VA: a long VA release is not cut off by the operators' envelopes ---
    //
    // The voice was declared finished as soon as the OPERATORS' envelopes ended,
    // because isActive() only looked at those. The VA has its own envelope and its
    // own release, so a long VA release was truncated the moment the operators let
    // go - an early cut-off and a click. The VA and the sub died together, which is
    // the signature: they share one envelope.
    //
    // This has to run through the VOICE MANAGER, not the voice. A voice rendered
    // directly keeps rendering whatever its state, so it cannot reproduce the bug at
    // all; it is the manager that asks isActive() and stops. An earlier version of
    // this test drove the voice directly and passed on the bug for exactly that
    // reason.
    //
    // It also uses a 512-sample buffer rather than the shared `blockLen`, which is a
    // full 44100 samples. Measuring "the last of 40 blocks" with that would have been
    // measuring 40 seconds of silence.
    {
        constexpr int shortBlock = 512;

        auto p = paramsForAlgo (4);

        // The VA reads the band-limited morph tables, and renders NOTHING without
        // them. paramsForAlgo does not set this, so a test that forgets it measures
        // silence and passes or fails for reasons unrelated to the VA.
        mixxynth::OpWaveTables tables;
        p.waveTables = &tables;

        p.vaLevel = 1.0f;
        p.vaShape = 0.0f;
        p.vaSubLevel = 0.0f;
        p.vaNoiseLevel = 0.0f;
        p.vaAttack = 0.0f;
        p.vaDecay = 0.0f;
        p.vaSustain = 1.0f;
        p.vaRelease = 1.0f;      // long, so the tail outlives the operators by far
        p.index = 0.0f;
        p.filterOn = false;

        // The operators are silent and let go instantly, so the VA is the only thing
        // left sounding once the key is released - which is the situation the bug
        // needs.
        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            p.carrier[i] = false;
            p.decay[i] = 0.0f;
            p.sustain[i] = 0.0f;
            p.release[i] = 0.0f;
        }

        MiXXVoiceManager::PolyParams poly;
        poly.polyphony = 1;
        poly.unison = 1;

        MiXXVoiceManager vm;
        vm.prepare (sr);

        juce::AudioBuffer<float> buf (2, shortBlock);
        juce::MidiBuffer noteOn, noteOff, nothing;
        noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        noteOff.addEvent (juce::MidiMessage::noteOff (1, 60), 0);

        for (int i = 0; i < 8; ++i)
        {
            buf.clear();
            vm.renderBlock (buf, i == 0 ? noteOn : nothing, p, mod, poly);
        }

        buf.clear();
        vm.renderBlock (buf, noteOff, p, mod, poly);

        // About 0.23 s of tail, so a 1 s release is barely a quarter done at the end.
        double tailRms = 0.0;
        float worstJump = 0.0f;

        for (int b = 0; b < 20; ++b)
        {
            buf.clear();
            vm.renderBlock (buf, nothing, p, mod, poly);

            tailRms = rmsOf (buf, 0, shortBlock);

            const auto* d = buf.getReadPointer (0);

            for (int i = 1; i < shortBlock; ++i)
                worstJump = juce::jmax (worstJump, std::abs (d[i] - d[i - 1]));
        }

        ok = check (tailRms > 0.01, "a long VA release survives the operators ending",
                    tailRms) && ok;

        // And it fades rather than stopping: an input at 261 Hz has a sample-to-sample
        // slope of about 0.037, so anything near full scale is a hard cut, not a fade.
        ok = check (worstJump < 0.05f, "a long VA release fades instead of cutting",
                    worstJump) && ok;
    }

    // --- 36. Distortion: the curves are what make it warm, and Bias is why ---
    //
    // The warmth claim rests on four things, and each is checked rather than asserted:
    // the curves are smooth, they pass through the origin, Bias really does add even
    // harmonics, and Drive really does add harmonics at all.
    {
        using Dist = mixxynth::DistortionFx;

        // Smooth through the origin, and finite. A curve that steps at zero, or that
        // leaves the linear region at a corner, is what makes distortion sound cheap.
        // Crush is the deliberate exception - a quantiser is a staircase by definition.
        {
            float worstJump = 0.0f;

            for (int mode = 0; mode < Dist::kNumModes; ++mode)
            {
                if (mode == Dist::modeCrush)
                    continue;

                // The largest change in slope anywhere in the curve.
                for (int i = 0; i < 400; ++i)
                {
                    const float x = -2.0f + 4.0f * static_cast<float> (i) / 400.0f;
                    constexpr float h = 0.002f;

                    const float d1 = (Dist::shape (mode, x + h, 0.0f)
                                        - Dist::shape (mode, x, 0.0f)) / h;
                    const float d2 = (Dist::shape (mode, x + 2.0f * h, 0.0f)
                                        - Dist::shape (mode, x + h, 0.0f)) / h;

                    worstJump = juce::jmax (worstJump, std::abs (d2 - d1));
                }
            }

            // A true corner would be a slope change of order 1 over one step; these
            // curves stay far below that.
            ok = check (worstJump < 0.1f, "every smooth distortion mode has a smooth curve",
                        worstJump) && ok;

            // And every mode passes through the origin, which is what stops the effect
            // changing the level as well as the character.
            float worstZero = 0.0f;

            for (int mode = 0; mode < Dist::kNumModes; ++mode)
                worstZero = juce::jmax (worstZero,
                                        std::abs (Dist::shape (mode, 0.0f, 0.3f)));

            ok = check (worstZero < 1.0e-6f, "every distortion mode passes through zero",
                        worstZero) && ok;
        }

        // Bias is the whole difference between warm and buzzy, so it is measured
        // directly: a curve with even harmonics is not odd-symmetric, i.e.
        // f(x) != -f(-x). With bias at zero it must be symmetric; with bias up it must
        // not be.
        {
            auto asymmetry = [] (int mode, float bias)
            {
                float worst = 0.0f;

                for (int i = 1; i <= 20; ++i)
                {
                    const float x = 0.1f * static_cast<float> (i);
                    worst = juce::jmax (worst, std::abs (Dist::shape (mode, x, bias)
                                                           + Dist::shape (mode, -x, bias)));
                }

                return worst;
            };

            const float symmetric = asymmetry (Dist::modeTube, 0.0f);
            const float biased = asymmetry (Dist::modeTube, 0.3f);

            ok = check (symmetric < 1.0e-5f && biased > 0.05f,
                        "Bias adds even harmonics, and zero Bias does not",
                        biased - symmetric) && ok;
        }
    }

    // --- 36b. Distortion: it is bounded, audible, and does not leak DC ---
    {
        using Dist = mixxynth::DistortionFx;

        // Every mode at every oversampling setting.
        {
            int bad = 0;
            float worstPeak = 0.0f;

            for (int mode = 0; mode < Dist::kNumModes; ++mode)
            {
                for (int ov = 0; ov < 4; ++ov)
                {
                    mixxynth::DistortionFx dist;
                    dist.prepare (sr, 512, 2);

                    float p[mixxynth::kMaxFxParams] {};
                    p[Dist::pMode] = static_cast<float> (mode)
                                       / static_cast<float> (Dist::kNumModes - 1);
                    p[Dist::pDrive] = 0.45f;
                    p[Dist::pBias] = 0.4f;
                    p[Dist::pTone] = 0.5f;
                    p[Dist::pLowCut] = 0.15f;
                    p[Dist::pOutput] = 0.5f;
                    p[Dist::pOversample] = static_cast<float> (ov) / 3.0f;
                    p[Dist::pMix] = 1.0f;

                    juce::AudioBuffer<float> buf (2, 512);
                    float peak = 0.0f;

                    for (int b = 0; b < 8; ++b)
                    {
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                               * 330.0f * static_cast<float> (i)
                                                               / static_cast<float> (sr));
                            buf.setSample (0, i, v);
                            buf.setSample (1, i, v);
                        }

                        dist.process (buf, p);

                        for (int ch = 0; ch < 2; ++ch)
                            peak = juce::jmax (peak, buf.getMagnitude (ch, 0, 512));
                    }

                    if (! std::isfinite (peak) || peak <= 0.01f || peak > 1.5f)
                        ++bad;

                    worstPeak = juce::jmax (worstPeak, peak);
                }
            }

            ok = check (bad == 0, "every distortion mode and oversampling setting is sane",
                        worstPeak) && ok;
        }

        // The DC blocker. An asymmetric curve makes DC, and DC downstream is worse than
        // the distortion it came from.
        {
            mixxynth::DistortionFx dist;
            dist.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[Dist::pMode] = 0.0f;
            p[Dist::pDrive] = 0.8f;
            p[Dist::pBias] = 1.0f;   // as asymmetric as it gets
            p[Dist::pTone] = 0.5f;
            p[Dist::pLowCut] = 0.0f;
            p[Dist::pOutput] = 0.5f;
            p[Dist::pOversample] = 2.0f / 3.0f;
            p[Dist::pMix] = 1.0f;

            juce::AudioBuffer<float> buf (2, 512);
            double sum = 0.0;

            for (int b = 0; b < 24; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * 330.0f * static_cast<float> (i)
                                                       / static_cast<float> (sr));
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                dist.process (buf, p);

                if (b >= 22)
                    for (int i = 0; i < 512; ++i)
                        sum += static_cast<double> (buf.getSample (0, i));
            }

            const double mean = sum / 1024.0;

            ok = check (std::abs (mean) < 0.01, "an asymmetric curve does not leak DC", mean) && ok;
        }
    }

    // --- 37. Ping-pong delay: the cross-feedback actually bounces ---
    //
    // The routing is the whole point of the module, so it is measured rather than
    // assumed. An impulse into the left channel should come back on the LEFT at one
    // delay time - the first echo is always on the side it went in - and on the RIGHT
    // at two, then back to the left at three. With the cross at zero nothing should
    // ever reach the right at all.
    {
        using Delay = mixxynth::PingPongDelayFx;

        auto echoEnergy = [&] (float cross, float atSeconds, int channel)
        {
            mixxynth::PingPongDelayFx delay;
            delay.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[Delay::pSync] = 0.0f;                 // free time
            p[Delay::pTimeL] = 0.5f;                // 200 ms
            p[Delay::pTimeR] = 0.5f;
            p[Delay::pFeedback] = 0.7f;
            p[Delay::pPingPong] = cross;
            p[Delay::pDamping] = 0.0f;
            p[Delay::pMod] = 0.0f;
            p[Delay::pDrift] = 0.0f;
            p[Delay::pWidth] = 0.5f;
            p[Delay::pMix] = 1.0f;                  // wet only

            juce::AudioBuffer<float> buf (2, 512);

            // An impulse into the left only, then enough blocks to cover three echoes.
            const int totalBlocks = 60;
            const int blockOfInterest = static_cast<int> (atSeconds * static_cast<float> (sr)
                                                           / 512.0f);

            float peak = 0.0f;

            for (int b = 0; b < totalBlocks; ++b)
            {
                buf.clear();

                if (b == 0)
                    buf.setSample (0, 0, 1.0f);

                delay.process (buf, p);

                // One block either side of the expected arrival, so a small time error
                // does not make the test lie.
                if (std::abs (b - blockOfInterest) <= 1)
                    for (int i = 0; i < 512; ++i)
                        peak = juce::jmax (peak, std::abs (buf.getSample (channel, i)));
            }

            return peak;
        };

        constexpr float delaySeconds = 0.2f;

        // First echo: left, where it went in.
        const float left1 = echoEnergy (1.0f, delaySeconds, 0);
        const float right1 = echoEnergy (1.0f, delaySeconds, 1);

        // Second echo: the other side.
        const float left2 = echoEnergy (1.0f, 2.0f * delaySeconds, 0);
        const float right2 = echoEnergy (1.0f, 2.0f * delaySeconds, 1);

        ok = check (left1 > 0.2f && right1 < 0.02f,
                    "the first echo comes back on the side it went in", left1) && ok;

        ok = check (right2 > 0.05f && right2 > left2 * 3.0f,
                    "the second echo has bounced to the other side", right2) && ok;

        // Cross at zero: two independent delays, so the right never hears the left.
        const float right1None = echoEnergy (0.0f, delaySeconds, 1);
        const float right2None = echoEnergy (0.0f, 2.0f * delaySeconds, 1);

        ok = check (right1None < 0.02f && right2None < 0.02f,
                    "with ping-pong at zero the channels stay independent", right2None) && ok;
    }

    // --- 37b. Ping-pong delay: Drift makes the echoes wander in pitch ---
    //
    // The feature the module exists for. Drift moves each channel's delay time slowly
    // and INDEPENDENTLY, so a repeat comes back very slightly sharp or flat and the two
    // sides pull apart - tape wow and flutter.
    //
    // Two earlier versions of this test tried to measure the pitch directly, as the
    // variance of the wet signal's zero crossings. Both were abandoned: the movement is
    // about 10 cents, which on a 1 kHz tone is a quarter of one sample per cycle, and
    // below the floor of the measurement - so the test was reading its own noise and
    // reported the same number with drift on and off.
    //
    // This measures the independence instead, which has a floor of exactly zero. With
    // drift off both channels have the same delay and the same input, so they are
    // bit-identical; with drift on they cannot be.
    {
        using Delay = mixxynth::PingPongDelayFx;

        auto channelDivergence = [&] (float drift)
        {
            mixxynth::PingPongDelayFx delay;
            delay.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[Delay::pSync] = 0.0f;
            p[Delay::pTimeL] = 0.55f;
            p[Delay::pTimeR] = 0.55f;
            p[Delay::pFeedback] = 0.3f;
            p[Delay::pPingPong] = 0.0f;   // independent, so any difference is the drift
            p[Delay::pDamping] = 0.0f;
            p[Delay::pMod] = 0.0f;        // the LFO must not be the thing that moves it
            p[Delay::pDrift] = drift;
            p[Delay::pWidth] = 0.5f;
            p[Delay::pMix] = 1.0f;

            juce::AudioBuffer<float> buf (2, 512);
            double worst = 0.0;

            double phase = 0.0;
            const double phaseInc = 300.0 / sr;

            // Long enough for the walk to pick several new targets.
            for (int b = 0; b < 200; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * static_cast<float> (phase));

                    phase += phaseInc;

                    if (phase >= 1.0)
                        phase -= 1.0;

                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                delay.process (buf, p);

                // Past the first echo, so the lines are full.
                if (b > 60)
                    for (int i = 0; i < 512; ++i)
                        worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (0, i))
                                                               - buf.getSample (1, i)));
            }

            return worst;
        };

        const double steady = channelDivergence (0.0f);
        const double drifting = channelDivergence (1.0f);

        ok = check (steady < 1.0e-6, "with Drift off the two channels stay identical",
                    steady) && ok;

        ok = check (drifting > 0.01, "Drift makes the echoes wander apart", drifting) && ok;
    }

    // --- 38. Flanger: depth moves the notches, polarity is a real difference ---
    {
        using Fl = mixxynth::FlangerFx;

        // The sweep, measured as how much the output LEVEL moves while a steady tone is
        // held. A flanger with no depth is a fixed comb and the level never changes; one
        // with depth sweeps its notches across the tone and the level breathes. That is
        // the whole effect, so it is what gets measured.
        auto levelSwing = [&] (float depth)
        {
            mixxynth::FlangerFx fl;
            fl.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[Fl::pRate] = 0.4f;
            p[Fl::pDepth] = depth;
            p[Fl::pFeedback] = 0.5f;
            p[Fl::pPolarity] = 0.0f;
            p[Fl::pDelay] = 0.4f;
            p[Fl::pStereo] = 0.0f;
            p[Fl::pMix] = 0.5f;

            juce::AudioBuffer<float> buf (2, 512);

            double phase = 0.0;
            const double phaseInc = 1200.0 / sr;

            double lo = 1.0e9, hi = 0.0;

            for (int b = 0; b < 120; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * static_cast<float> (phase));

                    phase += phaseInc;

                    if (phase >= 1.0)
                        phase -= 1.0;

                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                fl.process (buf, p);

                if (b > 30)
                {
                    const double level = rmsOf (buf, 0, 512);
                    lo = juce::jmin (lo, level);
                    hi = juce::jmax (hi, level);
                }
            }

            return hi - lo;
        };

        const double flat = levelSwing (0.0f);
        const double swept = levelSwing (0.9f);

        ok = check (swept > flat * 4.0 && swept > 0.02,
                    "Flanger Depth sweeps the notches", swept) && ok;

        // Stereo: at zero the two channels are identical, offset they cannot be. The same
        // zero-floor test the delay's drift uses.
        {
            auto divergence = [&] (float stereo)
            {
                mixxynth::FlangerFx fl;
                fl.prepare (sr, 512, 2);

                float p[mixxynth::kMaxFxParams] {};
                p[Fl::pRate] = 0.4f;
                p[Fl::pDepth] = 0.8f;
                p[Fl::pFeedback] = 0.5f;
                p[Fl::pPolarity] = 0.0f;
                p[Fl::pDelay] = 0.4f;
                p[Fl::pStereo] = stereo;
                p[Fl::pMix] = 0.5f;

                juce::AudioBuffer<float> buf (2, 512);
                double worst = 0.0;

                double phase = 0.0;
                const double phaseInc = 900.0 / sr;

                for (int b = 0; b < 80; ++b)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                           * static_cast<float> (phase));

                        phase += phaseInc;

                        if (phase >= 1.0)
                            phase -= 1.0;

                        buf.setSample (0, i, v);
                        buf.setSample (1, i, v);
                    }

                    fl.process (buf, p);

                    if (b > 30)
                        for (int i = 0; i < 512; ++i)
                            worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (0, i))
                                                                   - buf.getSample (1, i)));
                }

                return worst;
            };

            const double together = divergence (0.0f);
            const double apart = divergence (0.5f);

            ok = check (together < 1.0e-6, "Flanger Stereo at zero is mono", together) && ok;
            ok = check (apart > 0.05, "Flanger Stereo sweeps the channels apart", apart) && ok;
        }
    }

    // --- 39. Phaser: the stage count is a real control, and feedback sharpens ---
    {
        using Ph = mixxynth::PhaserFx;

        // How much the output level moves while a steady tone is held. The notches
        // sweeping across the tone is the effect; feedback is what sharpens them.
        auto levelSwing = [&] (float feedback, int stageIndex)
        {
            mixxynth::PhaserFx ph;
            ph.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[Ph::pStages] = static_cast<float> (stageIndex)
                               / static_cast<float> (Ph::getNumStageCounts() - 1);
            p[Ph::pRate] = 0.4f;
            p[Ph::pDepth] = 0.8f;
            p[Ph::pFeedback] = feedback;
            p[Ph::pCentre] = 0.5f;
            p[Ph::pStereo] = 0.0f;
            p[Ph::pStageOffset] = 0.0f;
            p[Ph::pMix] = 0.5f;

            juce::AudioBuffer<float> buf (2, 512);

            double phase = 0.0;
            const double phaseInc = 700.0 / sr;

            double lo = 1.0e9, hi = 0.0;

            for (int b = 0; b < 120; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * static_cast<float> (phase));

                    phase += phaseInc;

                    if (phase >= 1.0)
                        phase -= 1.0;

                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                ph.process (buf, p);

                if (b > 30)
                {
                    const double level = rmsOf (buf, 0, 512);
                    lo = juce::jmin (lo, level);
                    hi = juce::jmax (hi, level);
                }
            }

            return hi - lo;
        };

        // Six stages is index 4 in the offered list.
        const double withFeedback = levelSwing (0.9f, 4);

        // Feedback changes the effect, and it is checked as a CHANGE rather than as a
        // bigger level swing. The first version of this asserted the swing grew, and it
        // does not: resonance moves the notches toward peaks, so the swing can fall while
        // the notches get sharper. That is a real property of a phaser, not a bug, and the
        // test was asserting the wrong consequence of the right idea.
        {
            auto renderFeedback = [&] (float feedback, juce::AudioBuffer<float>& out)
            {
                mixxynth::PhaserFx ph;
                ph.prepare (sr, 512, 2);

                float p[mixxynth::kMaxFxParams] {};
                p[Ph::pStages] = 4.0f / static_cast<float> (Ph::getNumStageCounts() - 1);
                p[Ph::pRate] = 0.4f;
                p[Ph::pDepth] = 0.8f;
                p[Ph::pFeedback] = feedback;
                p[Ph::pCentre] = 0.5f;
                p[Ph::pStereo] = 0.0f;
                p[Ph::pStageOffset] = 0.0f;
                p[Ph::pMix] = 0.5f;

                double phase = 0.0;
                const double phaseInc = 700.0 / sr;

                for (int b = 0; b < 40; ++b)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                           * static_cast<float> (phase));

                        phase += phaseInc;

                        if (phase >= 1.0)
                            phase -= 1.0;

                        out.setSample (0, i, v);
                        out.setSample (1, i, v);
                    }

                    ph.process (out, p);
                }
            };

            juce::AudioBuffer<float> none (2, 512), lots (2, 512);
            renderFeedback (0.0f, none);
            renderFeedback (0.9f, lots);

            double difference = 0.0;

            for (int i = 0; i < 512; ++i)
                difference = juce::jmax (difference,
                                         std::abs (static_cast<double> (none.getSample (0, i))
                                                     - lots.getSample (0, i)));

            ok = check (difference > 0.05, "Phaser Feedback changes the notches",
                        difference) && ok;
        }

        ok = check (withFeedback > 0.02, "the Phaser sweeps at full feedback", withFeedback) && ok;

        // Every stage count on offer produces a working phaser, and they do not all
        // produce the SAME one - the selector is a character control, not decoration.
        {
            int bad = 0;
            std::vector<double> swings;

            for (int i = 0; i < Ph::getNumStageCounts(); ++i)
            {
                const double swing = levelSwing (0.6f, i);

                swings.push_back (swing);

                if (! std::isfinite (swing) || swing <= 0.001)
                    ++bad;
            }

            // Comparing 2 and 3 stages is the clearest pair: an odd chain leaves a notch
            // at DC and an even one does not.
            const double difference = std::abs (swings[0] - swings[1]);

            ok = check (bad == 0, "every Phaser stage count works",
                        static_cast<double> (bad)) && ok;
            ok = check (difference > 0.005, "odd and even stage counts are different effects",
                        difference) && ok;

            // And the offered list really does include odd numbers, which is the whole
            // reason the selector is not a 2/4/6/8 knob.
            int oddCounts = 0;

            for (int i = 0; i < Ph::getNumStageCounts(); ++i)
                if (Ph::stageCountFor (i) % 2 == 1)
                    ++oddCounts;

            ok = check (oddCounts >= 2, "the Phaser offers odd stage counts",
                        static_cast<double> (oddCounts)) && ok;
        }
    }

    // --- 39b. Phaser: the stereo stage offset separates the channels ---
    //
    // Two allpasses more on the right is a different notch pattern rather than a wider
    // version of the same one. With the offset off and no LFO offset the two channels are
    // bit-identical, so the floor here is exactly zero.
    {
        using Ph = mixxynth::PhaserFx;

        auto divergence = [&] (bool offset)
        {
            mixxynth::PhaserFx ph;
            ph.prepare (sr, 512, 2);

            float p[mixxynth::kMaxFxParams] {};
            p[Ph::pStages] = 4.0f / static_cast<float> (Ph::getNumStageCounts() - 1);
            p[Ph::pRate] = 0.4f;
            p[Ph::pDepth] = 0.8f;
            p[Ph::pFeedback] = 0.5f;
            p[Ph::pCentre] = 0.5f;
            p[Ph::pStereo] = 0.0f;    // no LFO offset, so any difference is the stages
            p[Ph::pStageOffset] = offset ? 1.0f : 0.0f;
            p[Ph::pMix] = 0.5f;

            juce::AudioBuffer<float> buf (2, 512);
            double worst = 0.0;

            double phase = 0.0;
            const double phaseInc = 700.0 / sr;

            for (int b = 0; b < 60; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * static_cast<float> (phase));

                    phase += phaseInc;

                    if (phase >= 1.0)
                        phase -= 1.0;

                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }

                ph.process (buf, p);

                if (b > 20)
                    for (int i = 0; i < 512; ++i)
                        worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (0, i))
                                                               - buf.getSample (1, i)));
            }

            return worst;
        };

        const double same = divergence (false);
        const double offset = divergence (true);

        ok = check (same < 1.0e-6, "Phaser with no stage offset is mono", same) && ok;
        ok = check (offset > 0.05, "the Phaser stage offset separates the channels",
                    offset) && ok;
    }

    // --- 40. The output limiter is a brickwall, and transparent below it ---
    //
    // Two claims, and both matter. The first is that nothing above the threshold ever
    // reaches the output - that is what makes it a safety net rather than a suggestion.
    // The second is that below the threshold it does nothing at all, because a limiter
    // that colours ordinary material is not a safety net either.
    {
        // ---- transparent below the threshold ----
        //
        // Down there it is a DELAY and nothing else, so the output is the input shifted by
        // the lookahead. Checking that, rather than "roughly the same", is what proves the
        // gain really is exactly 1 and not merely close to it.
        {
            mixxynth::Limiter lim;
            lim.prepare (sr, 512, 2);
            lim.setReleaseMs (100.0f);

            constexpr int lookahead = static_cast<int> (mixxynth::Limiter::kLookaheadMs
                                                          * 0.001f * 44100.0f);

            const int len = 4096;
            juce::AudioBuffer<float> buf (2, len);
            std::vector<float> input (static_cast<size_t> (len));

            for (int i = 0; i < len; ++i)
            {
                const float v = 0.5f * std::sin (0.03f * static_cast<float> (i));
                input[static_cast<size_t> (i)] = v;
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            lim.process (buf);

            double worst = 0.0;

            // Past the first lookahead, so both sides are real signal.
            for (int i = lookahead + 4; i < len; ++i)
                worst = juce::jmax (worst, std::abs (static_cast<double> (buf.getSample (0, i))
                                                       - input[static_cast<size_t> (i - lookahead)]));

            ok = check (worst < 1.0e-6, "the limiter is transparent below the threshold",
                        worst) && ok;
        }

        // ---- a brickwall above it ----
        //
        // Deliberately absurd input: thirty times full scale, and a DC-like plateau rather
        // than a transient, so a limiter that only caught the first sample of a peak would
        // be caught out. Nothing above the threshold may survive either case.
        {
            mixxynth::Limiter lim;
            lim.prepare (sr, 512, 2);
            lim.setReleaseMs (50.0f);

            const int len = 8192;
            juce::AudioBuffer<float> buf (2, len);

            for (int i = 0; i < len; ++i)
            {
                // A square wave at 30x, so every edge is a step far past the threshold.
                const float v = (i / 97) % 2 == 0 ? 30.0f : -30.0f;
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            lim.process (buf);

            float peak = 0.0f;

            for (int i = 0; i < len; ++i)
                peak = juce::jmax (peak, std::abs (buf.getSample (0, i)));

            ok = check (std::isfinite (peak) && peak <= mixxynth::Limiter::kThreshold + 1.0e-5f,
                        "nothing above the limiter's threshold reaches the output", peak) && ok;

            // And it reports the reduction it applied, which is what the meter shows.
            ok = check (lim.getGainReductionDb () < -20.0f,
                        "the limiter reports its gain reduction",
                        lim.getGainReductionDb()) && ok;
        }

        // ---- below the threshold it reports nothing ----
        {
            mixxynth::Limiter lim;
            lim.prepare (sr, 512, 2);
            lim.setReleaseMs (100.0f);

            juce::AudioBuffer<float> buf (2, 512);

            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.5f * std::sin (0.03f * static_cast<float> (i));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }

            lim.process (buf);

            ok = check (lim.getGainReductionDb() > -0.01f,
                        "the limiter reports no reduction when it is not working",
                        lim.getGainReductionDb()) && ok;
        }
    }

    std::printf (ok ? "SMOKE OK\n" : "SMOKE FAILED\n");
    return ok ? 0 : 1;
}
