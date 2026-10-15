#include "VoiceManager.h"

#include <algorithm>
#include <vector>

void MiXXVoiceManager::prepare (double sr)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;

    for (auto& s : slots)
    {
        s.voice.setSampleRate (sampleRate);
        s.voice.reset();
        s.modState.reset();
        s.note = -1;
        s.channel = 1;
        s.order = 0;
        s.held = false;
        s.unisonCents = 0.0f;
        s.noteBendSemis = 0.0f;
        s.pressure = 0.0f;
        s.timbre = 0.0f;
    }
}

void MiXXVoiceManager::reset()
{
    for (auto& s : slots)
    {
        s.voice.reset();
        s.modState.reset();
        s.note = -1;
        s.channel = 1;
        s.order = 0;
        s.held = false;
        s.unisonCents = 0.0f;
        s.noteBendSemis = 0.0f;
        s.pressure = 0.0f;
        s.timbre = 0.0f;
    }
}

int MiXXVoiceManager::countHeldNotes() const
{
    std::array<bool, 128> seen {};

    for (const auto& s : slots)
        if (s.held && s.note >= 0 && s.note < 128)
            seen[static_cast<size_t> (s.note)] = true;

    return static_cast<int> (std::count (seen.begin(), seen.end(), true));
}

void MiXXVoiceManager::releaseNote (int channel, int note, const mixxynth::ModParams& mod)
{
    for (auto& s : slots)
    {
        if (s.held && s.note == note && s.channel == channel)
        {
            s.held = false;
            s.voice.noteOff (note);
            s.modState.noteOff (mod);
        }
    }
}

void MiXXVoiceManager::startNote (int channel, int note, float velocity,
                                  const mixxynth::ModParams& mod, const PolyParams& poly)
{
    const int uni = juce::jlimit (1, 16, poly.unison);

    // Polyphony: cap simultaneous held notes by stealing the oldest note.
    while (countHeldNotes() >= juce::jlimit (1, 32, poly.polyphony))
    {
        int oldestIdx = -1;
        juce::uint64 oldestOrder = 0;

        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (! slots[i].held)
                continue;

            if (oldestIdx < 0 || slots[i].order < oldestOrder)
            {
                oldestIdx = static_cast<int> (i);
                oldestOrder = slots[i].order;
            }
        }

        if (oldestIdx < 0)
            break;

        releaseNote (slots[static_cast<size_t> (oldestIdx)].channel,
                     slots[static_cast<size_t> (oldestIdx)].note, mod);
    }

    // Pick slots: fully idle voices first, then the oldest sounding voices.
    std::vector<int> free;
    std::vector<int> stolen;

    for (size_t i = 0; i < slots.size(); ++i)
        if (! slots[i].voice.isActive())
            free.push_back (static_cast<int> (i));

    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].voice.isActive())
            stolen.push_back (static_cast<int> (i));

    std::sort (stolen.begin(), stolen.end(), [this] (int a, int b)
               { return slots[static_cast<size_t> (a)].order < slots[static_cast<size_t> (b)].order; });

    for (const int idx : stolen)
        free.push_back (idx);

    for (int k = 0; k < uni && k < static_cast<int> (free.size()); ++k)
    {
        auto& s = slots[static_cast<size_t> (free[static_cast<size_t> (k)])];

        // Symmetric detune and pan across the unison stack.
        float pos = 0.0f;

        if (uni > 1)
            pos = (2.0f * static_cast<float> (k) / static_cast<float> (uni - 1)) - 1.0f;

        s.voice.reset();
        s.unisonCents = pos * poly.unisonDetune * 0.5f;
        s.noteBendSemis = 0.0f;
        s.pressure = 0.0f;
        s.timbre = 0.0f;
        applyDetune (s);
        s.voice.setPan (pos * poly.unisonSpread);
        s.voice.noteOn (note, velocity);
        s.modState.reset();
        s.modState.noteOn (mod);
        s.note = note;
        s.channel = channel;
        s.order = counter++;
        s.held = true;
    }
}

void MiXXVoiceManager::applyDetune (Slot& s)
{
    s.voice.setDetune (s.unisonCents + (pitchBendSemitones + s.noteBendSemis) * 100.0f);
}

void MiXXVoiceManager::startNoteOnChannel (int channel, int note, float velocity,
                                           const mixxynth::ModParams& mod,
                                           const PolyParams& poly)
{
    startNote (channel, note, velocity, mod, poly);
}

void MiXXVoiceManager::releaseNoteOnChannel (int channel, int note, const mixxynth::ModParams& mod)
{
    releaseNote (channel, note, mod);
}

void MiXXVoiceManager::setNotePitchBend (int channel, int note, float semitones)
{
    for (auto& s : slots)
    {
        if (s.voice.isActive() && s.note == note && s.channel == channel)
        {
            s.noteBendSemis = semitones;
            applyDetune (s);
        }
    }
}

void MiXXVoiceManager::setNotePressure (int channel, int note, float value)
{
    for (auto& s : slots)
        if (s.voice.isActive() && s.note == note && s.channel == channel)
            s.pressure = juce::jlimit (0.0f, 1.0f, value);
}

void MiXXVoiceManager::setNoteTimbre (int channel, int note, float value)
{
    for (auto& s : slots)
        if (s.voice.isActive() && s.note == note && s.channel == channel)
            s.timbre = juce::jlimit (0.0f, 1.0f, value);
}

void MiXXVoiceManager::renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                                    const MiXXFmVoice::VoiceParams& params, const mixxynth::ModParams& mod)
{
    // Short sub-blocks keep modulation smooth without recomputing per sample.
    int offset = 0;

    while (offset < numSamples)
    {
        const int chunk = juce::jmin (modBlockSize, numSamples - offset);
        std::array<float, mixxynth::kNumModSources> sources {};

        for (auto& s : slots)
        {
            if (! s.voice.isActive())
                continue;

            // Per-note MPE expression, falling back to the global aftertouch so
            // channel pressure still works when MPE is off.
            mixxynth::computeSources (s.modState, mod, s.voice.getVelocity(),
                                      s.voice.getCurrentNote(),
                                      juce::jmax (s.pressure, mod.aftertouch),
                                      s.timbre,
                                      chunk, sources);

            auto vp = params; // base patch plus this voice's modulation
            mixxynth::applyModulation (sources, mod, vp);
            s.voice.renderAdd (buffer, startSample + offset, chunk, vp);
        }

        offset += chunk;
    }
}

void MiXXVoiceManager::renderBlock (juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midi,
                                    const MiXXFmVoice::VoiceParams& params,
                                    const mixxynth::ModParams& mod,
                                    const PolyParams& poly)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    int pos = 0;

    for (const auto metadata : midi)
    {
        const int t = juce::jlimit (0, numSamples, metadata.samplePosition);

        if (t > pos)
        {
            renderRange (buffer, pos, t - pos, params, mod);
            pos = t;
        }

        const auto msg = metadata.getMessage();

        if (msg.isNoteOn())
            startNote (msg.getChannel(), msg.getNoteNumber(), msg.getFloatVelocity(), mod, poly);
        else if (msg.isNoteOff())
            releaseNote (msg.getChannel(), msg.getNoteNumber(), mod);
    }

    if (pos < numSamples)
        renderRange (buffer, pos, numSamples - pos, params, mod);

    // Interim output stage: voice-count compensation plus a soft clip, so a
    // dense chord cannot exceed full scale. The real limiter and EQ land with
    // the FX tail in M4; this keeps the prototype usable meanwhile.
    const float target = 1.0f / std::sqrt (static_cast<float> (juce::jmax (1, getActiveVoiceCount())));
    const int numCh = buffer.getNumChannels();

    for (int i = 0; i < numSamples; ++i)
    {
        mixGain += (target - mixGain) * 0.0005f;

        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i, std::tanh (buffer.getSample (ch, i) * mixGain));
    }

    for (auto& s : slots)
        if (s.held && ! s.voice.isActive())
            s.note = -1;
}

int MiXXVoiceManager::getActiveNoteCount() const
{
    return countHeldNotes();
}

void MiXXVoiceManager::setPitchBendSemitones (float semitones)
{
    if (juce::approximatelyEqual (semitones, pitchBendSemitones))
        return;

    pitchBendSemitones = semitones;

    // Re-derive every sounding voice's detune from its unison position so the
    // bend does not accumulate.
    for (auto& s : slots)
        if (s.voice.isActive())
            applyDetune (s);
}

int MiXXVoiceManager::getActiveVoiceCount() const
{
    int n = 0;

    for (const auto& s : slots)
        if (s.voice.isActive())
            ++n;

    return n;
}

void MiXXVoiceManager::publishShapePhases (std::array<float, mixxynth::kNumShapes>& out) const
{
    out.fill (-1.0f);

    const Slot* newest = nullptr;

    for (const auto& s : slots)
    {
        if (s.note < 0)
            continue;

        if (newest == nullptr || s.order > newest->order)
            newest = &s;
    }

    if (newest == nullptr)
        return;

    for (int i = 0; i < mixxynth::kNumShapes; ++i)
        out[static_cast<size_t> (i)] =
            static_cast<float> (newest->modState.shapeStates[static_cast<size_t> (i)].phase);
}
