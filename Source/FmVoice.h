#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

// M1b-ii: mono 6-op FM voice, per-op DX-style ADSR, per-op self-feedback.
// Audible routing: Op1 modulates Op2's phase (FM index) AND every op mixes
// to the output directly (proves all params live). Each op also feeds its own
// previous output back into its phase (smoothed fb amount, 1-sample delay,
// DX-style, stable for fb < 1). Strict algorithm matrix lands in M2.
// Polyphony/unison land in M2.
class MiXXFmVoice
{
public:
    static constexpr int numOps = 6;

    struct VoiceParams
    {
        float ratio[numOps]   = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        float level[numOps]   = { 0.8f, 0.8f, 0.12f, 0.12f, 0.12f, 0.12f };
        float attack[numOps];
        float decay[numOps];
        float sustain[numOps];
        float release[numOps];
        float feedback[numOps] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        float index    = 3.0f;
        float master   = 0.8f;

        VoiceParams()
        {
            for (int i = 0; i < numOps; ++i)
            {
                attack[i]  = 0.01f;
                decay[i]   = 0.10f;
                sustain[i] = 0.90f;
                release[i] = 0.20f;
            }
        }
    };

    MiXXFmVoice();

    void setSampleRate (double sampleRate);
    void renderBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const VoiceParams& params);
    bool isActive() const;

private:
    struct Op
    {
        double phase = 0.0;
        float env = 0.0f;
        int stage = 0; // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
        float releaseFrom = 0.0f;
        float lastOut = 0.0f; // 1-sample delayed output for self-feedback
        float fbSmooth = 0.0f; // smoothed feedback amount (zipper-free)
    };

    void noteOn (int noteNumber, float velocity);
    void noteOff (int noteNumber);
    void renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& params);
    static float updateEnvelope (Op& op, float a, float d, float s, float r, double sampleRate);

    std::array<Op, numOps> ops;
    double sampleRate = 44100.0;
    double frequency = 440.0;
    float velocity = 0.0f;
    int currentNote = -1;
    bool gate = false;
};
