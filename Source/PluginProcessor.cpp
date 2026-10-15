#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Algorithms.h"

juce::AudioProcessorValueTreeState::ParameterLayout MiXXynthProcessor::createLayout()
{
    // M1b-ii: per-op ratio/level/feedback/ADSR x6 + index + master.
    // 6*7 + 2 = 44 params. All automatable, all in DAW state.
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const juce::String n = juce::String (i + 1);
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "ratio", "Op " + n + " Ratio",
            juce::NormalisableRange<float> (0.25f, 16.0f, 0.001f, 0.4f), 1.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "level", "Op " + n + " Level",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
            (i < 2 ? 0.8f : 0.12f)));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "feedback", "Op " + n + " Feedback",
            juce::NormalisableRange<float> (0.0f, 0.95f, 0.001f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "attack", "Op " + n + " Attack",
            juce::NormalisableRange<float> (0.001f, 2.0f, 0.001f, 0.4f), 0.01f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "decay", "Op " + n + " Decay",
            juce::NormalisableRange<float> (0.005f, 4.0f, 0.001f, 0.4f), 0.10f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "sustain", "Op " + n + " Sustain",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.90f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "release", "Op " + n + " Release",
            juce::NormalisableRange<float> (0.005f, 8.0f, 0.001f, 0.4f), 0.20f));
    }

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "fmIndex", "FM Index",
        juce::NormalisableRange<float> (0.0f, 12.0f, 0.001f), 3.0f));

    // M2a: DX7 algorithm selector (32 algos + "Free" matrix mode).
    juce::StringArray algoNames;

    for (int i = 0; i < mixxynth::kNumAlgorithms; ++i)
        algoNames.add (juce::String (i + 1));

    algoNames.add ("Free (matrix)");

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "algorithm", "Algorithm", algoNames, 0));

    // M2a: free-routing matrix, 30 off-diagonal depths (src modulates dst).
    // Self cells are omitted: per-op feedback covers self-modulation.
    for (int src = 0; src < mixxynth::kNumOps; ++src)
    {
        for (int dst = 0; dst < mixxynth::kNumOps; ++dst)
        {
            if (src == dst)
                continue;

            params.push_back (std::make_unique<juce::AudioParameterFloat> (
                "mtx" + juce::String (src + 1) + juce::String (dst + 1),
                "Mod " + juce::String (src + 1) + " > " + juce::String (dst + 1),
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        }
    }

    // M2a: carrier switches for free-routing mode.
    for (int i = 0; i < mixxynth::kNumOps; ++i)
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "carrier" + juce::String (i + 1),
            "Op " + juce::String (i + 1) + " Carrier", true));

    // M2b: polyphony and unison.
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        "polyphony", "Polyphony", 1, 32, 16));
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        "unison", "Unison Voices", 1, 16, 1));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "unisonDetune", "Unison Detune",
        juce::NormalisableRange<float> (0.0f, 50.0f, 0.01f), 10.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "unisonSpread", "Unison Spread",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

    // M3a: modulation matrix. 32 slots, each source -> destination with a
    // bipolar depth. Destination list mirrors mixxynth::ModDest exactly.
    juce::StringArray modSources { "Off", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
                                   "Env 1", "Env 2", "Env 3", "MSEG",
                                   "Macro 1", "Macro 2", "Macro 3", "Macro 4",
                                   "Velocity", "Mod Wheel", "Aftertouch", "Key Track" };

    juce::StringArray modDests { "Off", "FM Index" };

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        modDests.add ("Op" + juce::String (i + 1) + " Ratio");

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        modDests.add ("Op" + juce::String (i + 1) + " Level");

    modDests.add ("Master");

    for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            if (s != d)
                modDests.add ("Op" + juce::String (s + 1) + ">" + juce::String (d + 1));

    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            "mod" + id + "src", "Mod " + id + " Source", modSources, 0));
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            "mod" + id + "dst", "Mod " + id + " Dest", modDests, 0));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "mod" + id + "depth", "Mod " + id + " Depth",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    }

    // LFOs.
    juce::StringArray lfoShapes { "Sine", "Triangle", "Saw", "Ramp", "Square", "S&H" };
    juce::StringArray syncDivs;

    for (int i = 0; i < mixxynth::numSyncDivisions(); ++i)
        syncDivs.add (mixxynth::syncDivisionNames()[i]);

    for (int n = 0; n < mixxynth::kNumLfos; ++n)
    {
        const juce::String id = juce::String (n + 1);
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            "lfo" + id + "shape", "LFO " + id + " Shape", lfoShapes, 0));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "lfo" + id + "rate", "LFO " + id + " Rate",
            juce::NormalisableRange<float> (0.01f, 40.0f, 0.001f, 0.35f), 2.0f));
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "lfo" + id + "sync", "LFO " + id + " Sync", false));
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            "lfo" + id + "syncDiv", "LFO " + id + " Division", syncDivs, 3));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "lfo" + id + "phase", "LFO " + id + " Phase",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "lfo" + id + "retrig", "LFO " + id + " Retrigger", true));
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "lfo" + id + "bipolar", "LFO " + id + " Bipolar", true));
    }

    // Modulation envelopes.
    for (int n = 0; n < mixxynth::kNumModEnvs; ++n)
    {
        const juce::String id = juce::String (n + 1);
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "menv" + id + "a", "Env " + id + " Attack",
            juce::NormalisableRange<float> (0.001f, 10.0f, 0.001f, 0.35f), 0.01f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "menv" + id + "d", "Env " + id + " Decay",
            juce::NormalisableRange<float> (0.005f, 10.0f, 0.001f, 0.35f), 0.30f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "menv" + id + "s", "Env " + id + " Sustain",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.50f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "menv" + id + "r", "Env " + id + " Release",
            juce::NormalisableRange<float> (0.005f, 10.0f, 0.001f, 0.35f), 0.40f));
    }

    // MSEG-lite: 8 interpolated steps.
    {
        const float defaults[mixxynth::kNumMsegSteps] = { 0.0f, 0.35f, 1.0f, 0.75f, 0.2f, 0.6f, 0.3f, 0.1f };

        for (int i = 0; i < mixxynth::kNumMsegSteps; ++i)
            params.push_back (std::make_unique<juce::AudioParameterFloat> (
                "mseg" + juce::String (i + 1), "MSEG Step " + juce::String (i + 1),
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), defaults[i]));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "msegRate", "MSEG Rate",
            juce::NormalisableRange<float> (0.01f, 20.0f, 0.001f, 0.35f), 1.0f));
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "msegLoop", "MSEG Loop", true));
    }

    for (int i = 0; i < mixxynth::kNumMacros; ++i)
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "macro" + juce::String (i + 1), "Macro " + juce::String (i + 1),
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "masterGain", "Master Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.6f));

    return { params.begin(), params.end() };
}

MiXXynthProcessor::MiXXynthProcessor()
    : AudioProcessor (BusesProperties()
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MiXXynthState", createLayout())
{
}

MiXXynthProcessor::~MiXXynthProcessor() = default;

void MiXXynthProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = static_cast<float> (sampleRate);
    voiceManager.prepare (sampleRate);
}

void MiXXynthProcessor::releaseResources() {}

static float getParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
{
    if (auto* p = apvts.getRawParameterValue (id))
        return p->load();
    return 0.0f;
}

void MiXXynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    MiXXFmVoice::VoiceParams vp;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const juce::String n = juce::String (i + 1);
        vp.ratio[i]     = getParam (apvts, "op" + n + "ratio");
        vp.level[i]     = getParam (apvts, "op" + n + "level");
        vp.feedback[i]  = getParam (apvts, "op" + n + "feedback");
        vp.attack[i]    = getParam (apvts, "op" + n + "attack");
        vp.decay[i]     = getParam (apvts, "op" + n + "decay");
        vp.sustain[i]   = getParam (apvts, "op" + n + "sustain");
        vp.release[i]   = getParam (apvts, "op" + n + "release");
    }

    vp.index    = getParam (apvts, "fmIndex");
    vp.master   = getParam (apvts, "masterGain");

    const int algoIndex = static_cast<int> (getParam (apvts, "algorithm"));

    if (algoIndex >= 0 && algoIndex < mixxynth::kNumAlgorithms)
    {
        // DX7 algorithm mode: routing comes from the table.
        const auto& algo = mixxynth::algorithmTable()[static_cast<size_t> (algoIndex)];

        for (int src = 0; src < MiXXFmVoice::numOps; ++src)
            for (int dst = 0; dst < MiXXFmVoice::numOps; ++dst)
                vp.mod[src][dst] = algo.mod[static_cast<size_t> (src)][static_cast<size_t> (dst)];

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            vp.carrier[i] = algo.carrier[static_cast<size_t> (i)];
    }
    else
    {
        // Free mode: routing comes from the 30 matrix depths + 6 carriers.
        for (int src = 0; src < MiXXFmVoice::numOps; ++src)
            for (int dst = 0; dst < MiXXFmVoice::numOps; ++dst)
                vp.mod[src][dst] = (src == dst)
                    ? 0.0f
                    : getParam (apvts, "mtx" + juce::String (src + 1) + juce::String (dst + 1));

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            vp.carrier[i] = getParam (apvts, "carrier" + juce::String (i + 1)) > 0.5f;
    }

    MiXXVoiceManager::PolyParams poly;
    poly.polyphony      = static_cast<int> (getParam (apvts, "polyphony"));
    poly.unison         = static_cast<int> (getParam (apvts, "unison"));
    poly.unisonDetune   = getParam (apvts, "unisonDetune");
    poly.unisonSpread   = getParam (apvts, "unisonSpread");

    // Performance sources come from incoming MIDI.
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();

        if (msg.isController() && msg.getControllerNumber() == 1)
            modWheel = msg.getControllerValue() / 127.0f;
        else if (msg.isChannelPressure())
            aftertouch = msg.getChannelPressureValue() / 127.0f;
    }

    mixxynth::ModParams mod;
    mod.sampleRate = currentSampleRate;
    mod.modWheel = modWheel;
    mod.aftertouch = aftertouch;

    for (int n = 0; n < mixxynth::kNumModSlots; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto& slot = mod.slots[static_cast<size_t> (n)];
        slot.source = static_cast<int> (getParam (apvts, "mod" + id + "src"));
        slot.dest   = static_cast<int> (getParam (apvts, "mod" + id + "dst"));
        slot.depth  = getParam (apvts, "mod" + id + "depth");
    }

    for (int n = 0; n < mixxynth::kNumLfos; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto& lfo = mod.lfos[static_cast<size_t> (n)];
        lfo.shape     = static_cast<int> (getParam (apvts, "lfo" + id + "shape"));
        lfo.rateHz    = getParam (apvts, "lfo" + id + "rate");
        lfo.sync      = getParam (apvts, "lfo" + id + "sync") > 0.5f;
        lfo.syncIndex = static_cast<int> (getParam (apvts, "lfo" + id + "syncDiv"));
        lfo.phase     = getParam (apvts, "lfo" + id + "phase");
        lfo.retrig    = getParam (apvts, "lfo" + id + "retrig") > 0.5f;
        lfo.bipolar   = getParam (apvts, "lfo" + id + "bipolar") > 0.5f;

        if (lfo.sync)
        {
            // Host BPM drives synced LFOs; falls back to the free rate if unknown.
            const double bpm = getPlayHead() != nullptr
                               ? [this] { double b = 120.0;
                                          if (auto pos = getPlayHead()->getPosition())
                                              if (auto t = pos->getBpm())
                                                  b = *t;
                                          return b; }()
                               : 120.0;
            lfo.rateHz = static_cast<float> (bpm / 60.0 * mixxynth::syncDivisionCyclesPerBeat (lfo.syncIndex));
        }
    }

    for (int n = 0; n < mixxynth::kNumModEnvs; ++n)
    {
        const juce::String id = juce::String (n + 1);
        auto& env = mod.envs[static_cast<size_t> (n)];
        env.attack  = getParam (apvts, "menv" + id + "a");
        env.decay   = getParam (apvts, "menv" + id + "d");
        env.sustain = getParam (apvts, "menv" + id + "s");
        env.release = getParam (apvts, "menv" + id + "r");
    }

    for (int i = 0; i < mixxynth::kNumMsegSteps; ++i)
        mod.msegLevels[static_cast<size_t> (i)] = getParam (apvts, "mseg" + juce::String (i + 1));

    mod.msegRateHz = getParam (apvts, "msegRate");
    mod.msegLoop   = getParam (apvts, "msegLoop") > 0.5f;

    for (int i = 0; i < mixxynth::kNumMacros; ++i)
        mod.macros[static_cast<size_t> (i)] = getParam (apvts, "macro" + juce::String (i + 1));

    voiceManager.renderBlock (buffer, midi, vp, mod, poly);
}

juce::AudioProcessorEditor* MiXXynthProcessor::createEditor()
{
    return new MiXXynthEditor (*this);
}

void MiXXynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MiXXynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiXXynthProcessor();
}
