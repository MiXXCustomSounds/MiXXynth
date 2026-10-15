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

void MiXXFmVoice::setDetune (float cents)
{
    detuneCents = juce::jlimit (-100.0f, 100.0f, cents);

    if (currentNote >= 0)
        frequency = midiNoteToFreq (currentNote) * std::pow (2.0, static_cast<double> (detuneCents) / 1200.0);
}

void MiXXFmVoice::setPan (float pan)
{
    // Constant-power-ish stereo placement; centred leaves both channels at 1.
    const float p = juce::jlimit (-1.0f, 1.0f, pan);
    panL = (p <= 0.0f) ? 1.0f : 1.0f - p;
    panR = (p >= 0.0f) ? 1.0f : 1.0f + p;
}

void MiXXFmVoice::reset()
{
    currentNote = -1;
    gate = false;
    frequency = 440.0;
    velocity = 0.0f;

    for (auto& op : ops)
    {
        op.phase = 0.0;
        op.env = 0.0f;
        op.stage = 0;
        op.releaseFrom = 0.0f;
        op.prevOut = 0.0f;
        op.curOut = 0.0f;
        op.fbSmooth = 0.0f;
    }
}

void MiXXFmVoice::noteOn (int noteNumber, float vel)
{
    currentNote = noteNumber;
    frequency = midiNoteToFreq (noteNumber) * std::pow (2.0, static_cast<double> (detuneCents) / 1200.0);
    velocity = juce::jlimit (0.0f, 1.0f, vel);
    gate = true;

    for (auto& op : ops)
    {
        op.phase = 0.0;
        op.env = 0.0f;
        op.stage = 1;
        op.releaseFrom = 0.0f;
        op.prevOut = 0.0f;
        op.curOut = 0.0f;
        op.fbSmooth = 0.0f;
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
        float sum = 0.0f;

        // Descending order: every DX7 edge goes from a higher op number to a
        // lower one, so those edges resolve within the current sample.
        for (int dst = numOps - 1; dst >= 0; --dst)
        {
            auto& op = ops[static_cast<size_t> (dst)];

            updateEnvelope (op, p.attack[dst], p.decay[dst], p.sustain[dst], p.release[dst], sampleRate);
            op.fbSmooth += (p.feedback[dst] - op.fbSmooth) * 0.001f;

            double modSum = 0.0;

            for (int src = 0; src < numOps; ++src)
            {
                const float depth = p.mod[src][dst];

                if (depth == 0.0f)
                    continue;

                const auto& srcOp = ops[static_cast<size_t> (src)];
                modSum += static_cast<double> (depth) * (src > dst ? srcOp.curOut : srcOp.prevOut);
            }

            // Self-feedback uses this op's own previous output.
            modSum += static_cast<double> (op.fbSmooth) * op.prevOut;

            const float s = std::sin (static_cast<float> (twoPi * op.phase + modSum * p.index)) * op.env;
            op.curOut = s;

            if (p.carrier[dst])
                sum += s * p.level[dst];

            op.phase += frequency * p.ratio[dst] / sampleRate;
            if (op.phase >= 1.0)
                op.phase -= std::floor (op.phase);
        }

        for (auto& op : ops)
            op.prevOut = op.curOut;

        const float out = juce::jlimit (-1.0f, 1.0f, sum * velocity * p.master);

        if (numCh >= 2)
        {
            buffer.addSample (0, startSample + n, out * panL);
            buffer.addSample (1, startSample + n, out * panR);
        }
        else
        {
            for (int ch = 0; ch < numCh; ++ch)
                buffer.addSample (ch, startSample + n, out);
        }
    }
}

void MiXXFmVoice::renderAdd (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& params)
{
    renderRange (buffer, startSample, numSamples, params);
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
