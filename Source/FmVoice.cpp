#include "FmVoice.h"

namespace
{
    constexpr double twoPi = 6.28318530717958647692;

    float midiNoteToFreq (int note)
    {
        return 440.0f * std::pow (2.0f, static_cast<float> (note - 69) / 12.0f);
    }
}

MiXXFmVoice::MiXXFmVoice() = default;

void MiXXFmVoice::setSampleRate (double sr)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
}

bool MiXXFmVoice::isActive() const
{
    if (gate)
        return true;

    for (const auto& op : ops)
        if (op.stage != 0)
            return true;

    return false;
}

void MiXXFmVoice::noteOn (int noteNumber, float vel)
{
    currentNote = noteNumber;
    frequency = midiNoteToFreq (noteNumber);
    velocity = juce::jlimit (0.0f, 1.0f, vel);
    gate = true;

    for (auto& op : ops)
    {
        op.phase = 0.0;
        op.env = 0.0f;
        op.stage = 1;
        op.releaseFrom = 0.0f;
    }
}

void MiXXFmVoice::noteOff (int noteNumber)
{
    if (noteNumber != currentNote)
        return;

    gate = false;

    for (auto& op : ops)
    {
        if (op.stage != 0 && op.stage != 4)
        {
            op.stage = 4;
            op.releaseFrom = op.env;
        }
    }
}

float MiXXFmVoice::updateEnvelope (Op& op, float a, float d, float s, float r, double sr)
{
    const float sampleRateF = static_cast<float> (sr);

    switch (op.stage)
    {
        case 1: // attack
        {
            op.env += 1.0f / juce::jmax (1.0f, a * sampleRateF);
            if (op.env >= 1.0f) { op.env = 1.0f; op.stage = 2; }
            break;
        }
        case 2: // decay
        {
            op.env -= (1.0f - s) / juce::jmax (1.0f, d * sampleRateF);
            if (op.env <= s) { op.env = s; op.stage = 3; }
            break;
        }
        case 3: // sustain
            op.env = s;
            break;
        case 4: // release
        {
            op.env -= op.releaseFrom / juce::jmax (1.0f, r * sampleRateF);
            if (op.env <= 0.0f) { op.env = 0.0f; op.stage = 0; }
            break;
        }
        default:
            op.env = 0.0f;
            break;
    }

    return op.env;
}

void MiXXFmVoice::renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& p)
{
    const int numCh = buffer.getNumChannels();

    for (int n = 0; n < numSamples; ++n)
    {
        // Op1 doubles as the FM modulator source.
        updateEnvelope (ops[0], p.attack[0], p.decay[0], p.sustain[0], p.release[0], sampleRate);
        const float modOut = std::sin (static_cast<float> (twoPi * ops[0].phase)) * ops[0].env;

        float sum = 0.0f;

        for (int i = 0; i < numOps; ++i)
        {
            updateEnvelope (ops[i], p.attack[i], p.decay[i], p.sustain[i], p.release[i], sampleRate);

            double phase = ops[i].phase;
            if (i == 1) // carrier: phase-modulated by Op1
                phase += (static_cast<double> (p.index) * modOut) / twoPi;

            sum += std::sin (static_cast<float> (twoPi * phase)) * ops[i].env * p.level[i];

            ops[i].phase += frequency * p.ratio[i] / sampleRate;
            if (ops[i].phase >= 1.0)
                ops[i].phase -= std::floor (ops[i].phase);
        }

        const float out = juce::jlimit (-1.0f, 1.0f, sum * velocity * p.master);

        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, startSample + n, out);
    }
}

void MiXXFmVoice::renderBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const VoiceParams& params)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    int pos = 0;

    for (const auto metadata : midi)
    {
        const int t = juce::jlimit (0, numSamples, metadata.samplePosition);

        if (t > pos)
        {
            renderRange (buffer, pos, t - pos, params);
            pos = t;
        }

        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            noteOn (msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            noteOff (msg.getNoteNumber());
    }

    if (pos < numSamples)
        renderRange (buffer, pos, numSamples - pos, params);

    if (! isActive())
        currentNote = -1;
}
