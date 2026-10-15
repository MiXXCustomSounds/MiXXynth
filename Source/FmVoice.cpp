#include "FmVoice.h"

namespace
{
    constexpr double twoPi = 6.28318530717958647692;

    float midiNoteToFreq (int note)
    {
        return 440.0f * std::pow (2.0f, static_cast<float> (note - 69) / 12.0f);
    }

    // PolyBLEP correction: removes most of the aliasing from the saw/square
    // discontinuities without a wavetable.
    inline float polyBlep (double t, double dt)
    {
        if (dt <= 0.0)
            return 0.0f;

        if (t < dt)
        {
            const double x = t / dt;
            return static_cast<float> (x + x - x * x - 1.0);
        }

        if (t > 1.0 - dt)
        {
            const double x = (t - 1.0) / dt;
            return static_cast<float> (x * x + x + x + 1.0);
        }

        return 0.0f;
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
    // Range covers a 24-semitone pitch bend plus unison spread.
    detuneCents = juce::jlimit (-2400.0f, 2400.0f, cents);

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
    vaPhase = 0.0;
    vaSubPhase = 0.0;
    noiseState = 0x2545f491u;
    svfIc1 = 0.0f;
    svfIc2 = 0.0f;

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
    vaPhase = 0.0;
    vaSubPhase = 0.0;
    svfIc1 = 0.0f;
    svfIc2 = 0.0f;

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

        // --- M3b: VA layer, blended against the FM stack ---
        float va = 0.0f;

        if (p.vaLevel > 0.0f)
        {
            const double vaFreq = frequency * std::pow (2.0, static_cast<double> (p.vaOctave))
                                  * std::pow (2.0, static_cast<double> (p.vaDetune) / 1200.0);
            const double inc = juce::jlimit (0.0, 0.49, vaFreq / sampleRate);

            vaPhase += inc;

            if (vaPhase >= 1.0)
                vaPhase -= std::floor (vaPhase);

            switch (p.vaWave)
            {
                case 1: // square with pulse width
                {
                    const double duty = juce::jlimit (0.05, 0.95, static_cast<double> (p.vaPulseWidth));
                    float s = vaPhase < duty ? 1.0f : -1.0f;
                    s -= polyBlep (vaPhase, inc);
                    s += polyBlep (std::fmod (vaPhase + (1.0 - duty), 1.0), inc);
                    va = s;
                    break;
                }
                case 2: // sub square, one octave down
                {
                    const double subInc = inc * 0.5;
                    vaSubPhase += subInc;

                    if (vaSubPhase >= 1.0)
                        vaSubPhase -= std::floor (vaSubPhase);

                    float s = vaSubPhase < 0.5 ? 1.0f : -1.0f;
                    s -= polyBlep (vaSubPhase, subInc);
                    va = s;
                    break;
                }
                case 3: // white noise
                {
                    noiseState = noiseState * 1664525u + 1013904223u;
                    va = static_cast<float> ((noiseState >> 8) & 0xffffu) / 32768.0f - 1.0f;
                    break;
                }
                default: // saw
                {
                    float s = static_cast<float> (2.0 * vaPhase - 1.0);
                    s -= polyBlep (vaPhase, inc);
                    va = s;
                    break;
                }
            }
        }

        const float mixed = sum * (1.0f - p.vaLevel) + va * p.vaLevel;

        // --- M3b: per-voice TPT state-variable filter ---
        float filtered = mixed;

        if (p.filterOn)
        {
            float in = mixed;

            if (p.drive > 0.0f)
                in = std::tanh (in * (1.0f + p.drive * 8.0f));

            const float sr = static_cast<float> (sampleRate);
            const float fc = juce::jlimit (20.0f, sr * 0.45f, p.cutoff);
            const float g = std::tan (juce::MathConstants<float>::pi * fc / sr);
            const float k = 2.0f - 2.0f * juce::jlimit (0.0f, 0.98f, p.resonance);
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;

            const float v3 = in - svfIc2;
            const float v1 = a1 * svfIc1 + a2 * v3;
            const float v2 = svfIc2 + a2 * svfIc1 + a3 * v3;
            svfIc1 = 2.0f * v1 - svfIc1;
            svfIc2 = 2.0f * v2 - svfIc2;

            if (p.filterType == 1)       filtered = in - k * v1 - v2; // high pass
            else if (p.filterType == 2)  filtered = v1;               // band pass
            else                         filtered = v2;               // low pass
        }

        const float out = juce::jlimit (-1.0f, 1.0f, filtered * velocity * p.master);

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
