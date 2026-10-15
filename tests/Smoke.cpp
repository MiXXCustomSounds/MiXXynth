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
        m.lfos[0].rateHz = 5.0f;
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
        m.envs[0].attack = 0.25f;
        m.envs[0].sustain = 1.0f;
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

    std::printf (ok ? "SMOKE OK\n" : "SMOKE FAILED\n");
    return ok ? 0 : 1;
}
