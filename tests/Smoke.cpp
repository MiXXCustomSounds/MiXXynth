// M2a offline DSP smoke test: algorithms + free matrix, no plugin host.
// Checks: audible output, finite samples, envelope decay, feedback stability,
// all 32 DX7 algorithms render sanely, matrix fan-out and backward edges stable.
#include "../Source/Algorithms.h"
#include "../Source/FmVoice.h"

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

    std::printf (ok ? "SMOKE OK\n" : "SMOKE FAILED\n");
    return ok ? 0 : 1;
}
