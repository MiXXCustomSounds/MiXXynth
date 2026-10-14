// M1b offline DSP smoke test: renders the FM voice without a plugin host.
// Checks: audible output, finite samples, envelope decay after note-off.
#include "../Source/FmVoice.h"

#include <cmath>
#include <cstdio>

namespace
{
    double rmsOf (const juce::AudioBuffer<float>& buf, int start, int len)
    {
        double acc = 0.0;
        int count = 0;

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            for (int i = 0; i < len; ++i)
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
}

int main()
{
    constexpr double sr = 44100.0;
    constexpr int len = static_cast<int> (sr); // 1 second

    MiXXFmVoice voice;
    voice.setSampleRate (sr);
    MiXXFmVoice::VoiceParams p; // defaults: 2-op FM pair + 4 spare carriers

    juce::AudioBuffer<float> buf (2, len);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 0);
    voice.renderBlock (buf, midi, p);
    const double rmsOn = rmsOf (buf, static_cast<int> (sr * 0.1), static_cast<int> (sr * 0.5));

    // Release: note off at start, render another second, tail must decay.
    juce::AudioBuffer<float> tail (2, len);
    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
    voice.renderBlock (tail, off, p);
    const double rmsTailHead = rmsOf (tail, 0, static_cast<int> (sr * 0.1));
    const double rmsTailEnd = rmsOf (tail, static_cast<int> (sr * 0.8), static_cast<int> (sr * 0.2));

    bool ok = true;
    auto check = [&] (bool cond, const char* name, double v)
    {
        std::printf ("%s %-32s (%.6f)\n", cond ? "PASS" : "FAIL", name, v);
        ok = cond && ok;
    };

    check (rmsOn > 0.001, "note-on audible (rms>0.001)", rmsOn);
    check (rmsTailHead >= 0.0 && rmsTailEnd >= 0.0, "tail finite", rmsTailEnd);
    check (rmsTailEnd < rmsTailHead, "tail decays after note-off", rmsTailEnd);
    check (rmsTailEnd < 0.05, "tail near silence after 1s", rmsTailEnd);

    std::printf (ok ? "SMOKE OK\n" : "SMOKE FAILED\n");
    return ok ? 0 : 1;
}
