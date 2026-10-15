#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Algorithms.h"
#include "OpWave.h"

juce::AudioProcessorValueTreeState::ParameterLayout MiXXynthProcessor::createLayout()
{
    // M1b-ii: per-op ratio/level/feedback/ADSR x6 + index + master.
    // 6*7 + 2 = 44 params. All automatable, all in DAW state.
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const juce::String n = juce::String (i + 1);

        // M4b: the init patch is ALL ZERO by spec, so the synth loads silent.
        // Every range therefore opens down to 0 so each knob can genuinely sit
        // there, and the dice button (or a preset, once there is a preset
        // manager) is how you get sound.
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "ratio", "Op " + n + " Ratio",
            juce::NormalisableRange<float> (0.0f, 16.0f, 0.001f, 0.4f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "level", "Op " + n + " Level",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "feedback", "Op " + n + " Feedback",
            juce::NormalisableRange<float> (0.0f, 0.95f, 0.001f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "attack", "Op " + n + " Attack",
            juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f, 0.4f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "decay", "Op " + n + " Decay",
            juce::NormalisableRange<float> (0.0f, 4.0f, 0.001f, 0.4f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "sustain", "Op " + n + " Sustain",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "release", "Op " + n + " Release",
            juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.4f), 0.0f));

        // M4b: per-operator enable and waveform.
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "op" + n + "on", "Op " + n + " On", true));
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "wave", "Op " + n + " Wave",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
    }

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "fmIndex", "FM Index",
        juce::NormalisableRange<float> (0.0f, 12.0f, 0.001f), 0.0f));

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

    // Pitch bend range in semitones, applied to the wheel and to incoming
    // MIDI pitch bend.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "pitchBendRange", "Pitch Bend Range",
        juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 2.0f));

    // MPE. The plugin always declares MPE support to the host; this switch
    // decides whether incoming MIDI is interpreted as per-note expression.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "mpeEnabled", "MPE Enabled", false));
    params.push_back (std::make_unique<juce::AudioParameterInt> (
        "mpeMemberChannels", "MPE Member Channels", 1, 15, 15));

    // M3a: modulation matrix. 32 slots, each source -> destination with a
    // bipolar depth. Destination list mirrors mixxynth::ModDest exactly.
    juce::StringArray modSources;

    for (int i = 0; i < mixxynth::kNumModSources; ++i)
        modSources.add (mixxynth::modSourceNames()[i]);

    juce::StringArray modDests { "Off", "FM Index" };

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        modDests.add ("Op" + juce::String (i + 1) + " Ratio");

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        modDests.add ("Op" + juce::String (i + 1) + " Level");

    modDests.add ("Master");
    modDests.add ("VA Level");
    modDests.add ("VA Detune");
    modDests.add ("VA Pulse Width");
    modDests.add ("Filter Cutoff");
    modDests.add ("Filter Reso");
    modDests.add ("Filter Drive");

    for (int s = 0; s < MiXXFmVoice::numOps; ++s)
        for (int d = 0; d < MiXXFmVoice::numOps; ++d)
            if (s != d)
                modDests.add ("Op" + juce::String (s + 1) + " > Op" + juce::String (d + 1));

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

    for (int i = 0; i < mixxynth::kNumMacros; ++i)
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "macro" + juce::String (i + 1), "Macro " + juce::String (i + 1),
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    // M3b: VA oscillator and multimode filter.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaLevel", "VA Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "vaWave", "VA Wave",
        juce::StringArray { "Saw", "Square", "Sub", "Noise" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        "vaOctave", "VA Octave", -2, 2, 0));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaDetune", "VA Detune",
        juce::NormalisableRange<float> (-50.0f, 50.0f, 0.01f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaPulseWidth", "VA Pulse Width",
        juce::NormalisableRange<float> (0.05f, 0.95f, 0.001f), 0.5f));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "filterOn", "Filter On", true));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "filterType", "Filter Type",
        juce::StringArray { "Low Pass", "High Pass", "Band Pass" }, 0));

    // Wide open by default, so the filter is transparent until you move Cutoff
    // but is immediately audible the moment you do.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "filterCutoff", "Filter Cutoff",
        juce::NormalisableRange<float> (20.0f, 20000.0f, 0.1f, 0.25f), 20000.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "filterReso", "Filter Resonance",
        juce::NormalisableRange<float> (0.0f, 0.98f, 0.001f), 0.2f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "filterDrive", "Filter Drive",
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
    mpeInstrument.addListener (&mpeHandler);
    mpeInstrument.enableLegacyMode(); // replaced by a zone layout when MPE is on

    // Default modulation shapes: four LFOs followed by three envelopes.
    for (int i = 0; i < mixxynth::kNumShapes; ++i)
        shapeBuffers[0][static_cast<size_t> (i)] = defaultShapeFor (i);

    shapeBuffers[1] = shapeBuffers[0];
}

MiXXynthProcessor::~MiXXynthProcessor() = default;

mixxynth::ModShape MiXXynthProcessor::defaultShapeFor (int index)
{
    return index < mixxynth::kNumLfos ? mixxynth::ModShape::defaultLfo()
                                      : mixxynth::ModShape::defaultEnv();
}

mixxynth::ModShape MiXXynthProcessor::getShape (int index) const
{
    index = juce::jlimit (0, mixxynth::kNumShapes - 1, index);
    return shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)][static_cast<size_t> (index)];
}

void MiXXynthProcessor::setShape (int index, const mixxynth::ModShape& shape)
{
    index = juce::jlimit (0, mixxynth::kNumShapes - 1, index);

    const int current = activeShapeBuffer.load (std::memory_order_acquire);
    const int next = 1 - current;

    shapeBuffers[next] = shapeBuffers[current];
    shapeBuffers[next][static_cast<size_t> (index)] = shape;
    activeShapeBuffer.store (next, std::memory_order_release);
}

void MiXXynthProcessor::resetShapeToDefault (int index)
{
    setShape (index, defaultShapeFor (index));
}

void MiXXynthProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = static_cast<float> (sampleRate);
    voiceManager.prepare (sampleRate);

    // Band-limited operator waveform tables. Built here, never on the audio
    // thread, and shared by every voice.
    waveTables.prepare (sampleRate);
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
    // Keeps the editor's on-screen keyboard in sync with incoming notes, and
    // injects its own note-ons/offs back into the stream so the on-screen keys
    // are playable.
    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);

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
        vp.opOn[i]      = getParam (apvts, "op" + n + "on") > 0.5f;
        vp.opWave[i]    = getParam (apvts, "op" + n + "wave");
    }

    vp.waveTables = &waveTables;

    vp.index    = getParam (apvts, "fmIndex");
    vp.master   = getParam (apvts, "masterGain");

    // M3b: VA layer and filter.
    vp.vaLevel      = getParam (apvts, "vaLevel");
    vp.vaWave       = static_cast<int> (getParam (apvts, "vaWave"));
    vp.vaOctave     = static_cast<int> (getParam (apvts, "vaOctave"));
    vp.vaDetune     = getParam (apvts, "vaDetune");
    vp.vaPulseWidth = getParam (apvts, "vaPulseWidth");
    vp.filterOn     = getParam (apvts, "filterOn") > 0.5f;
    vp.filterType   = static_cast<int> (getParam (apvts, "filterType"));
    vp.cutoff       = getParam (apvts, "filterCutoff");
    vp.resonance    = getParam (apvts, "filterReso");
    vp.drive        = getParam (apvts, "filterDrive");

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
        else if (msg.isPitchWheel())
            pitchBend = (static_cast<float> (msg.getPitchWheelValue()) - 8192.0f) / 8192.0f;
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

    // Shapes come from the double-buffered bank rather than parameters, so the
    // audio thread always reads a complete, consistent set.
    mod.shapes = shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)];

    // Synced shapes follow the host tempo; free ones use their own rate.
    {
        double bpm = 120.0;

        if (getPlayHead() != nullptr)
            if (auto pos = getPlayHead()->getPosition())
                if (auto t = pos->getBpm())
                    bpm = *t;

        for (auto& shape : mod.shapes)
            if (shape.sync)
                shape.rateHz = static_cast<float> (bpm / 60.0
                                 * mixxynth::syncDivisionCyclesPerBeat (shape.syncIndex));
    }

    for (int i = 0; i < mixxynth::kNumMacros; ++i)
        mod.macros[static_cast<size_t> (i)] = getParam (apvts, "macro" + juce::String (i + 1));

    // Global pitch offset: the wheel and channel pitch bend. MPE adds a
    // per-note offset on top of this.
    voiceManager.setPitchBendSemitones (pitchBend * getParam (apvts, "pitchBendRange"));

    // MPE: when enabled, incoming MIDI is read as per-note expression and the
    // MPEInstrument drives voice allocation, so the block is then rendered
    // without replaying the note events. (Events are applied at block start
    // rather than sample-accurately; fine for expression, which is continuous.)
    currentMod = mod;
    currentPoly = poly;
    mpeActive = getParam (apvts, "mpeEnabled") > 0.5f;

    if (mpeActive)
    {
        const int members = static_cast<int> (getParam (apvts, "mpeMemberChannels"));

        if (members != appliedMemberChannels)
        {
            juce::MPEZoneLayout layout;
            layout.setLowerZone (members);
            mpeInstrument.setZoneLayout (layout);
            appliedMemberChannels = members;
        }

        for (const auto metadata : midi)
            mpeInstrument.processNextMidiEvent (metadata.getMessage());

        juce::MidiBuffer none;
        voiceManager.renderBlock (buffer, none, vp, mod, poly);
    }
    else
    {
        voiceManager.renderBlock (buffer, midi, vp, mod, poly);
    }

    // Publish the newest voice's shape phases for the editor's playheads. Single
    // writer (audio thread), single reader (UI), so relaxed atomics are enough.
    {
        std::array<float, mixxynth::kNumShapes> phases;
        voiceManager.publishShapePhases (phases);

        for (int i = 0; i < mixxynth::kNumShapes; ++i)
            shapePhases[static_cast<size_t> (i)].store (phases[static_cast<size_t> (i)],
                                                        std::memory_order_relaxed);
    }
}

float MiXXynthProcessor::getShapePhase (int index) const
{
    index = juce::jlimit (0, mixxynth::kNumShapes - 1, index);
    return shapePhases[static_cast<size_t> (index)].load (std::memory_order_relaxed);
}

juce::AudioProcessorEditor* MiXXynthProcessor::createEditor()
{
    return new MiXXynthEditor (*this);
}

void MiXXynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Shapes are not parameters, so they ride along as a child of the state tree.
    state.removeChild (state.getChildWithName ("shapes"), nullptr);

    juce::ValueTree shapesNode ("shapes");

    for (int i = 0; i < mixxynth::kNumShapes; ++i)
        shapesNode.appendChild (mixxynth::shapeToValueTree (getShape (i), "shape"), nullptr);

    state.appendChild (shapesNode, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MiXXynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Restore the shapes before handing the tree to APVTS so a missing or
    // partial "shapes" node simply leaves the defaults in place.
    if (auto shapesNode = tree.getChildWithName ("shapes"); shapesNode.isValid())
    {
        for (int i = 0; i < mixxynth::kNumShapes; ++i)
        {
            mixxynth::ModShape shape;
            mixxynth::shapeFromValueTree (defaultShapeFor (i), shapesNode.getChild (i), shape);
            setShape (i, shape);
        }
    }

    apvts.replaceState (tree);

    // Tell the editor this was a load, not a user edit, so pages that watch
    // parameters for changes can resync instead of reacting.
    ++stateLoadCount;
}

// ---------------------------------------------------------------------------
// MPE per-note expression routing
// ---------------------------------------------------------------------------
void MiXXynthProcessor::MpeHandler::noteAdded (juce::MPENote note)
{
    if (! proc.mpeActive)
        return;

    proc.voiceManager.startNoteOnChannel (note.midiChannel, note.initialNote,
                                          note.noteOnVelocity.asUnsignedFloat(),
                                          proc.currentMod, proc.currentPoly);
}

void MiXXynthProcessor::MpeHandler::noteReleased (juce::MPENote note)
{
    if (! proc.mpeActive)
        return;

    proc.voiceManager.releaseNoteOnChannel (note.midiChannel, note.initialNote,
                                            proc.currentMod);
}

void MiXXynthProcessor::MpeHandler::notePitchbendChanged (juce::MPENote note)
{
    if (! proc.mpeActive)
        return;

    // totalPitchbendInSemitones already folds in the zone's per-note bend range.
    proc.voiceManager.setNotePitchBend (note.midiChannel, note.initialNote,
                                        static_cast<float> (note.totalPitchbendInSemitones));
}

void MiXXynthProcessor::MpeHandler::notePressureChanged (juce::MPENote note)
{
    if (! proc.mpeActive)
        return;

    proc.voiceManager.setNotePressure (note.midiChannel, note.initialNote,
                                       note.pressure.asUnsignedFloat());
}

void MiXXynthProcessor::MpeHandler::noteTimbreChanged (juce::MPENote note)
{
    if (! proc.mpeActive)
        return;

    proc.voiceManager.setNoteTimbre (note.midiChannel, note.initialNote,
                                     note.timbre.asUnsignedFloat());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiXXynthProcessor();
}
