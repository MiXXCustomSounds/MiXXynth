#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Algorithms.h"
#include "OpWave.h"

namespace
{
    // Reads a parameter's current value by id. Used by the modulation snapshot,
    // the FX rack, and the state helpers, so it lives at the top rather than next
    // to whichever caller happened to need it first.
    float getParam (const juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        if (auto* p = apvts.getRawParameterValue (id))
            return p->load();

        return 0.0f;
    }
}

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

    // FM Index is applied as a phase offset in CYCLES (see FmVoice::renderRange),
    // so index 1.0 is already a modulation index of about 2*pi = 6.3 radians.
    // The musical range is roughly 0..0.6 and anything past ~2 is noise, so the
    // old 0..12 range wasted 95% of the knob. Squaring the response puts the
    // useful region across the first half of the travel and keeps the bright end
    // available for extreme sounds.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "fmIndex", "FM Index",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.0001f, 0.5f), 0.0f));

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

    // M4b: how far each unison voice's start phase is randomised. 0 keeps the
    // stack phase-aligned (a punchy attack); 1 starts each voice anywhere in the
    // cycle (a wide, soft attack). It applies to single-voice notes too, so
    // repeated notes vary.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "unisonPhaseRand", "Phase Random",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    // M6: analog-style drift. A slow random walk per operator, so the pitch wanders
    // over seconds instead of sitting still. It is a property of the voice rather than
    // of the unison stack, which is the point: detune separates voices and does nothing
    // at unison = 1, whereas drift moves a single voice and is audible there.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "drift", "Drift",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    // M6: velocity response. The voice applied velocity as a bare `* velocity` on its
    // output, so the sensitivity was permanently 100% with no way to reduce it.
    // velToLevel = 1 is exactly that behaviour, so the default leaves existing patches
    // sounding as they did and the control only ever softens the response.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "velToLevel", "Velocity to Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));

    // How much velocity opens the FM index. Off by default, because this one changes
    // the timbre with touch rather than the loudness - expressive, but not what an
    // existing patch expects.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "velToIndex", "Velocity to Index",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

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
    modDests.add ("VA Shape");
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

    // vaWave used to be a choice of four shapes. It is a 0..1 morph now, through the same
    // shared tables the operators use, so the VA sweeps sine -> triangle -> saw -> square
    // -> narrowing pulse instead of jumping between four waveforms.
    //
    // The parameter TYPE changed, so an old preset's shape selection does not carry over
    // as the same shape: its normalised value maps onto the new axis, which means a patch
    // that sat on Square lands somewhere in the middle of the sweep. Flagged deliberately
    // rather than discovered.
    //
    // Defaults to saw, which is where the old list started.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaShape", "VA Shape",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f / 3.0f));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        "vaOctave", "VA Octave", -2, 2, 0));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaDetune", "VA Detune",
        juce::NormalisableRange<float> (-50.0f, 50.0f, 0.01f), 0.0f));

    // The sub and the noise are their own levels rather than two more entries in the shape
    // list, so they blend with any shape instead of replacing it.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaSubLevel", "VA Sub Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaNoiseLevel", "VA Noise Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    // The VA's own envelope. It was a hardwired one-pole gate - fixed ~8 ms attack and
    // ~50 ms release, sustain pinned at 1 - which made the VA's amplitude shape the one
    // thing in the layer you could not touch, and the main reason it sounded abrupt
    // rather than subtle next to the FM stack.
    //
    // The defaults reproduce the old gate closely enough that nothing changes by
    // surprise: no decay, full sustain, and a short attack and release.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaAttack", "VA Attack",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f, 0.4f), 0.008f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaDecay", "VA Decay",
        juce::NormalisableRange<float> (0.0f, 4.0f, 0.001f, 0.4f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaSustain", "VA Sustain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "vaRelease", "VA Release",
        juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.4f), 0.05f));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "filterOn", "Filter On", true));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "filterType", "Filter Type",
        juce::StringArray { "Low", "High", "Band" }, 0));

    // Cutoff uses a TRUE logarithmic mapping rather than JUCE's power-curve skew.
    // The old range (skew 0.25, i.e. value = 20 + 19980 * v^4) was very uneven:
    // the bottom third of the knob covered 20-180 Hz while a tenth of a turn in
    // the middle jumped 531 Hz to 1269 Hz. A log map puts equal octaves under
    // equal knob travel, which is what a frequency control needs.
    juce::NormalisableRange<float> cutoffRange (
        20.0f, 20000.0f,
        [] (float start, float end, float normalised)
        {
            return start * std::pow (end / start, juce::jlimit (0.0f, 1.0f, normalised));
        },
        [] (float start, float end, float value)
        {
            return std::log (juce::jlimit (start, end, value) / start) / std::log (end / start);
        });

    // Wide open by default, so the filter is transparent until you move Cutoff
    // but is immediately audible the moment you do.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "filterCutoff", "Filter Cutoff", cutoffRange, 20000.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "filterReso", "Filter Resonance",
        juce::NormalisableRange<float> (0.0f, 0.98f, 0.001f), 0.2f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "filterDrive", "Filter Drive",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    // M6: a filter per operator. Same control set as the global filter above, once per
    // operator, plus its own envelope and the routing choice that decides whether it sits on
    // the operator's output, its modulation, or both.
    //
    // Cutoff uses the same true-logarithmic map as the global one, so equal control travel is
    // equal octaves here too. A filter sweep is a musical interval, not a frequency.
    for (int i = 0; i < mixxynth::kNumOps; ++i)
    {
        const juce::String n = juce::String (i + 1);

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            "op" + n + "fltOn", "Op " + n + " Filter", false));

        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            "op" + n + "fltType", "Op " + n + " Filter Type",
            juce::StringArray { "Low", "High", "Band" }, 0));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltCutoff", "Op " + n + " Filter Cutoff", cutoffRange, 20000.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltReso", "Op " + n + " Filter Resonance",
            juce::NormalisableRange<float> (0.0f, 0.98f, 0.001f), 0.2f));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltDrive", "Op " + n + " Filter Drive",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

        // The filter's envelope. A short attack and a long decay by default, because that is
        // the shape a filter envelope is almost always wanted in - and because an envelope
        // starting at full would make switching a filter on sound like nothing happened.
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltAttack", "Op " + n + " Filter Attack",
            juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.4f), 0.005f));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltDecay", "Op " + n + " Filter Decay",
            juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.4f), 0.30f));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltSustain", "Op " + n + " Filter Sustain",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "op" + n + "fltRelease", "Op " + n + " Filter Release",
            juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.4f), 0.20f));

        // Where the filter sits. Both by default, so switching one on does the obvious thing
        // rather than half of it.
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            "op" + n + "fltRoute", "Op " + n + " Filter Route",
            juce::StringArray { "Out", "Mod", "Both" }, 2));
    }

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "masterGain", "Master Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.6f));

    // The output safety limiter. On by default, because a safety net that has to be
    // switched on is not one.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "limiterOn", "Limiter", true));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "limiterRelease", "Limiter Release",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f));

    // M4b workstream 4: the FX rack.
    //
    // A fixed pool of eight slots, because an APVTS parameter's items cannot
    // change after construction, so "add a module" cannot mean "add a parameter".
    // Each slot gets a type, a bypass, and eight generic float parameters whose
    // meaning the module declares (see Fx/FxModule.h). The alternative - a named
    // parameter per control per module - would need a parameter per control per
    // module and the pool sized for the worst case anyway, so it costs more for
    // no extra clarity.
    //
    // The generic params default to 0 here rather than to a module's defaults,
    // because a default cannot depend on a type that the user can change. The
    // editor applies the module's descriptor defaults when a slot's type is set.
    //
    // Every slot starts Off, so the rack is empty and a pure passthrough.
    {
        juce::StringArray typeNames;

        for (int t = 0; t < mixxynth::kNumFxTypes; ++t)
            typeNames.add (mixxynth::fxTypeName (t));

        for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
        {
            const juce::String n = juce::String (s + 1);

            params.push_back (std::make_unique<juce::AudioParameterChoice> (
                "fx" + n + "type", "FX " + n + " Type", typeNames, 0));

            params.push_back (std::make_unique<juce::AudioParameterBool> (
                "fx" + n + "bypass", "FX " + n + " Bypass", false));

            for (int p = 0; p < mixxynth::kMaxFxParams; ++p)
                params.push_back (std::make_unique<juce::AudioParameterFloat> (
                    "fx" + n + "p" + juce::String (p + 1),
                    "FX " + n + " P" + juce::String (p + 1),
                    juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
        }
    }

    return { params.begin(), params.end() };
}

MiXXynthProcessor::MiXXynthProcessor()
    : AudioProcessor (BusesProperties()
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MiXXynthState", createLayout())
{
    mpeInstrument.addListener (&mpeHandler);
    mpeInstrument.enableLegacyMode(); // replaced by a zone layout when MPE is on

    // Default modulation modules: one LFO and one envelope.
    for (int i = 0; i < mixxynth::kDefaultLfos; ++i)
        shapeBuffers[0].shapes[static_cast<size_t> (i)] = mixxynth::ModShape::defaultLfo();

    for (int i = 0; i < mixxynth::kDefaultEnvs; ++i)
        shapeBuffers[0].shapes[static_cast<size_t> (mixxynth::kDefaultLfos + i)] =
            mixxynth::ModShape::defaultEnv();

    shapeBuffers[0].numLfos = mixxynth::kDefaultLfos;
    shapeBuffers[0].numEnvs = mixxynth::kDefaultEnvs;
    shapeBuffers[1] = shapeBuffers[0];
}

MiXXynthProcessor::~MiXXynthProcessor() = default;

mixxynth::ModShape MiXXynthProcessor::defaultShapeFor (int index) const
{
    return shapeIsLfo (index) ? mixxynth::ModShape::defaultLfo()
                              : mixxynth::ModShape::defaultEnv();
}

int MiXXynthProcessor::getNumShapeModules() const
{
    return shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)].count();
}

bool MiXXynthProcessor::shapeIsLfo (int index) const
{
    return shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)].isLfo (index);
}

int MiXXynthProcessor::getNumLfoModules() const
{
    return shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)].numLfos;
}

int MiXXynthProcessor::shapeSourceIndex (int index) const
{
    const auto& set = shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)];

    if (! set.isLfo (index))
        return mixxynth::srcEnv1 + (index - set.numLfos);

    return mixxynth::srcLfo1 + index;
}

bool MiXXynthProcessor::canAddShapeModule (bool lfo) const
{
    return shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)].canAdd (lfo);
}

int MiXXynthProcessor::addShapeModule (bool lfo)
{
    const int current = activeShapeBuffer.load (std::memory_order_acquire);

    if (! shapeBuffers[current].canAdd (lfo))
        return -1;

    const int next = 1 - current;
    shapeBuffers[next] = shapeBuffers[current];

    auto& set = shapeBuffers[next];
    const int index = lfo ? set.numLfos : set.numLfos + set.numEnvs;

    // The new module goes at the end of its own family, so the LFOs stay
    // contiguous at the front and the envelopes follow.
    for (int i = set.count(); i > index; --i)
        set.shapes[static_cast<size_t> (i)] = set.shapes[static_cast<size_t> (i - 1)];

    set.shapes[static_cast<size_t> (index)] = lfo ? mixxynth::ModShape::defaultLfo()
                                                  : mixxynth::ModShape::defaultEnv();

    if (lfo)
        ++set.numLfos;
    else
        ++set.numEnvs;

    activeShapeBuffer.store (next, std::memory_order_release);
    return index;
}

void MiXXynthProcessor::removeShapeModule (int index)
{
    const int current = activeShapeBuffer.load (std::memory_order_acquire);

    if (index < 0 || index >= shapeBuffers[current].count())
        return;

    const int next = 1 - current;
    shapeBuffers[next] = shapeBuffers[current];

    auto& set = shapeBuffers[next];
    const bool wasLfo = set.isLfo (index);

    // Compact: everything after the removed module slides down one, so the LFOs
    // stay contiguous at the front.
    for (int i = index; i < set.count() - 1; ++i)
        set.shapes[static_cast<size_t> (i)] = set.shapes[static_cast<size_t> (i + 1)];

    set.shapes[static_cast<size_t> (set.count() - 1)] = mixxynth::ModShape {};

    if (wasLfo)
        --set.numLfos;
    else
        --set.numEnvs;

    activeShapeBuffer.store (next, std::memory_order_release);
}

void MiXXynthProcessor::clearShapeModules()
{
    const int current = activeShapeBuffer.load (std::memory_order_acquire);
    const int next = 1 - current;

    shapeBuffers[next] = shapeBuffers[current];
    shapeBuffers[next].numLfos = 0;
    shapeBuffers[next].numEnvs = 0;
    activeShapeBuffer.store (next, std::memory_order_release);
}

mixxynth::ModShape MiXXynthProcessor::getShape (int index) const
{
    index = juce::jlimit (0, mixxynth::kMaxShapes - 1, index);
    return shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)]
             .shapes[static_cast<size_t> (index)];
}

void MiXXynthProcessor::setShape (int index, const mixxynth::ModShape& shape)
{
    index = juce::jlimit (0, mixxynth::kMaxShapes - 1, index);

    const int current = activeShapeBuffer.load (std::memory_order_acquire);
    const int next = 1 - current;

    shapeBuffers[next] = shapeBuffers[current];
    shapeBuffers[next].shapes[static_cast<size_t> (index)] = shape;
    activeShapeBuffer.store (next, std::memory_order_release);
}

void MiXXynthProcessor::resetShapeToDefault (int index)
{
    setShape (index, defaultShapeFor (index));
}

void MiXXynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = static_cast<float> (sampleRate);
    voiceManager.prepare (sampleRate);

    // Band-limited operator waveform tables. Built here, never on the audio
    // thread, and shared by every voice.
    waveTables.prepare (sampleRate);

    // The rack is stereo. Modules are created on demand; this prepares the ones
    // that already exist, and then publishes whatever the current parameters ask
    // for, which matters when a host restores state before preparing.
    rack.prepare (sampleRate, samplesPerBlock, 2);
    limiter.prepare (sampleRate, samplesPerBlock, 2);
    syncFxRack();
}

void MiXXynthProcessor::syncFxRack()
{
    for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
    {
        const int type = static_cast<int> (getParam (apvts, "fx" + juce::String (s + 1) + "type"));

        rack.setSlotType (s, type);

        // The file as well as the type: after a state load the module has to be
        // handed its impulse, and the file can be present even when the type has not
        // changed.
        rack.setSlotFile (s, type, getFxSlotFileName (s), getFxSlotFileData (s));
    }
}

mixxynth::FxSlotState MiXXynthProcessor::readFxSlot (int slot) const
{
    mixxynth::FxSlotState state;

    if (slot < 0 || slot >= mixxynth::kMaxFxSlots)
        return state;

    const juce::String n = juce::String (slot + 1);

    state.type = static_cast<int> (getParam (apvts, "fx" + n + "type"));
    state.bypass = getParam (apvts, "fx" + n + "bypass") > 0.5f;

    for (int p = 0; p < mixxynth::kMaxFxParams; ++p)
        state.params[static_cast<size_t> (p)] = getParam (apvts, "fx" + n + "p" + juce::String (p + 1));

    state.fileName = getFxSlotFileName (slot);
    state.fileData = getFxSlotFileData (slot);

    return state;
}

// The per-slot files live in the state tree rather than the parameter tree, for the
// same reason the shapes do: a parameter is a number and this is not. One child per
// slot, with the bytes base64-encoded, because base64 is what the XML round-trip is
// guaranteed to preserve - and a preset has to survive being written to disk and read
// back by a different build of the plugin.
void MiXXynthProcessor::setFxSlotFile (int slot, const juce::String& name,
                                       const juce::MemoryBlock& data)
{
    if (slot < 0 || slot >= mixxynth::kMaxFxSlots)
        return;

    auto node = apvts.state.getChildWithName ("fxFiles");

    if (! node.isValid())
    {
        // Nothing to clear and no node yet: do not create one just to leave it empty.
        if (name.isEmpty() && data.isEmpty())
            return;

        node = juce::ValueTree ("fxFiles");
        apvts.state.appendChild (node, nullptr);
    }

    auto child = node.getChildWithProperty ("slot", slot);

    if (name.isEmpty() && data.isEmpty())
    {
        if (child.isValid())
            node.removeChild (child, nullptr);

        return;
    }

    if (! child.isValid())
    {
        child = juce::ValueTree ("file");
        child.setProperty ("slot", slot, nullptr);
        node.appendChild (child, nullptr);
    }

    child.setProperty ("name", name, nullptr);
    child.setProperty ("data", data.toBase64Encoding(), nullptr);
}

juce::String MiXXynthProcessor::getFxSlotFileName (int slot) const
{
    const auto node = apvts.state.getChildWithName ("fxFiles");

    if (! node.isValid())
        return {};

    const auto child = node.getChildWithProperty ("slot", slot);

    return child.isValid() ? child.getProperty ("name").toString() : juce::String {};
}

juce::MemoryBlock MiXXynthProcessor::getFxSlotFileData (int slot) const
{
    const auto node = apvts.state.getChildWithName ("fxFiles");

    if (! node.isValid())
        return {};

    const auto child = node.getChildWithProperty ("slot", slot);

    if (! child.isValid())
        return {};

    juce::MemoryBlock data;

    if (! data.fromBase64Encoding (child.getProperty ("data").toString()))
        return {};

    return data;
}

void MiXXynthProcessor::writeFxSlot (int slot, const mixxynth::FxSlotState& state)
{
    if (slot < 0 || slot >= mixxynth::kMaxFxSlots)
        return;

    const juce::String n = juce::String (slot + 1);

    auto setChoice = [this] (const juce::String& id, int index)
    {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id)))
        {
            c->beginChangeGesture();
            c->setValueNotifyingHost (c->convertTo0to1 (static_cast<float> (index)));
            c->endChangeGesture();
        }
    };

    auto setFloat = [this] (const juce::String& id, float value)
    {
        if (auto* f = apvts.getParameter (id))
        {
            f->beginChangeGesture();
            f->setValueNotifyingHost (f->convertTo0to1 (value));
            f->endChangeGesture();
        }
    };

    setChoice ("fx" + n + "type", state.type);

    if (auto* b = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("fx" + n + "bypass")))
    {
        b->beginChangeGesture();
        b->setValueNotifyingHost (state.bypass ? 1.0f : 0.0f);
        b->endChangeGesture();
    }

    for (int p = 0; p < mixxynth::kMaxFxParams; ++p)
        setFloat ("fx" + n + "p" + juce::String (p + 1), state.params[static_cast<size_t> (p)]);

    // The file travels with the slot, so a reorder moves the module's impulse too.
    setFxSlotFile (slot, state.fileName, state.fileData);

    // The type is part of the state, so the rack has to be told what to run.
    rack.setSlotType (slot, state.type);
    rack.setSlotFile (slot, state.type, state.fileName, state.fileData);
}

void MiXXynthProcessor::releaseResources() {}

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
    vp.vaShape      = getParam (apvts, "vaShape");
    vp.vaOctave     = static_cast<int> (getParam (apvts, "vaOctave"));
    vp.vaDetune     = getParam (apvts, "vaDetune");
    vp.vaSubLevel   = getParam (apvts, "vaSubLevel");
    vp.vaNoiseLevel = getParam (apvts, "vaNoiseLevel");
    vp.vaAttack     = getParam (apvts, "vaAttack");
    vp.vaDecay      = getParam (apvts, "vaDecay");
    vp.vaSustain    = getParam (apvts, "vaSustain");
    vp.vaRelease    = getParam (apvts, "vaRelease");
    vp.filterOn     = getParam (apvts, "filterOn") > 0.5f;
    vp.filterType   = static_cast<int> (getParam (apvts, "filterType"));
    vp.cutoff       = getParam (apvts, "filterCutoff");
    vp.resonance    = getParam (apvts, "filterReso");
    vp.drive        = getParam (apvts, "filterDrive");

    for (int i = 0; i < MiXXFmVoice::numOps; ++i)
    {
        const juce::String n = juce::String (i + 1);

        vp.opFilterOn[i]      = getParam (apvts, "op" + n + "fltOn") > 0.5f;
        vp.opFilterType[i]    = static_cast<int> (getParam (apvts, "op" + n + "fltType"));
        vp.opFilterCutoff[i]  = getParam (apvts, "op" + n + "fltCutoff");
        vp.opFilterReso[i]    = getParam (apvts, "op" + n + "fltReso");
        vp.opFilterDrive[i]   = getParam (apvts, "op" + n + "fltDrive");
        vp.opFilterAttack[i]  = getParam (apvts, "op" + n + "fltAttack");
        vp.opFilterDecay[i]   = getParam (apvts, "op" + n + "fltDecay");
        vp.opFilterSustain[i] = getParam (apvts, "op" + n + "fltSustain");
        vp.opFilterRelease[i] = getParam (apvts, "op" + n + "fltRelease");
        vp.opFilterRouting[i] = static_cast<int> (getParam (apvts, "op" + n + "fltRoute"));
    }

    vp.drift        = getParam (apvts, "drift");
    vp.velToLevel   = getParam (apvts, "velToLevel");
    vp.velToIndex   = getParam (apvts, "velToIndex");

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
    poly.phaseRand      = getParam (apvts, "unisonPhaseRand");

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

        for (int i = 0; i < mod.shapes.count(); ++i)
        {
            auto& shape = mod.shapes.shapes[static_cast<size_t> (i)];

            if (shape.sync)
                shape.rateHz = static_cast<float> (bpm / 60.0
                                 * mixxynth::syncDivisionCyclesPerBeat (shape.syncIndex));
        }

        // The same tempo goes to the FX rack, so a module with a sync control follows
        // the host rather than needing a tempo of its own.
        rack.setTempo (bpm);
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

    // ---- FX rack, at the end of the chain ------------------------------------
    // Read each slot once, then hand the rack flat arrays. A slot that is Off or
    // bypassed is skipped inside the rack, so an empty rack is a passthrough and
    // costs nothing beyond this parameter read.
    for (int s = 0; s < mixxynth::kMaxFxSlots; ++s)
    {
        const juce::String n = juce::String (s + 1);

        fxTypes[static_cast<size_t> (s)] = static_cast<int> (getParam (apvts, "fx" + n + "type"));
        fxBypass[static_cast<size_t> (s)] = getParam (apvts, "fx" + n + "bypass") > 0.5f;

        for (int p = 0; p < mixxynth::kMaxFxParams; ++p)
            fxParams[static_cast<size_t> (s) * mixxynth::kMaxFxParams + static_cast<size_t> (p)]
                = getParam (apvts, "fx" + n + "p" + juce::String (p + 1));
    }

    rack.process (buffer, fxTypes.data(), fxBypass.data(), fxParams.data());

    // ---- the output safety limiter -------------------------------------------
    // The rack is the last stage that can push the output past unity - a reverb at 100%
    // wet has no obligation to stay inside it - so the limiter sits after it, as the
    // last thing in the chain.
    //
    // It replaces a per-sample tanh guard, which was not a limiter: a waveshaper
    // distorts the peaks it is meant to be catching, so it changed the sound of exactly
    // the material it existed to protect.
    if (getParam (apvts, "limiterOn") > 0.5f)
    {
        limiter.setReleaseMs (20.0f * std::pow (25.0f, getParam (apvts, "limiterRelease")));
        limiter.process (buffer);
    }
    else
    {
        // Switched off means switched off, but the output still has to be inside full
        // scale: a host is entitled to receive a signal it can convert, and a hard clamp
        // is the honest behaviour when the user has asked for no limiter.
        limiter.reset();

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = juce::jlimit (-1.0f, 1.0f, data[i]);
        }
    }

    // ---- the oscilloscope ----------------------------------------------------
    // The output, after the limiter, so the scope shows what actually leaves the plugin
    // rather than what the voices produced. Appending into a rolling window and publishing
    // the write index last, so the UI never reads a window that is half-updated.
    {
        int w = scopeWrite.load (std::memory_order_relaxed);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            scopeSamples[static_cast<size_t> (w)].store (buffer.getSample (0, i),
                                                         std::memory_order_relaxed);

            w = (w + 1) % kScopeSize;
        }

        scopeWrite.store (w, std::memory_order_release);
    }

    // Publish the newest voice's shape phases for the editor's playheads. Single
    // writer (audio thread), single reader (UI), so relaxed atomics are enough.
    {
        std::array<float, mixxynth::kMaxShapes> phases;
        voiceManager.publishShapePhases (phases);

        for (int i = 0; i < mixxynth::kMaxShapes; ++i)
            shapePhases[static_cast<size_t> (i)].store (phases[static_cast<size_t> (i)],
                                                        std::memory_order_relaxed);
    }

    // The same for the modulated destination values. This is what lets a control MOVE and
    // turn blue rather than only being able to say that it could.
    {
        std::array<float, mixxynth::kNumModDests> values;
        const bool live = voiceManager.publishModulatedValues (values);

        modValuesLive.store (live, std::memory_order_relaxed);

        // Not stored when nothing is sounding: the reader keys off the flag, and leaving
        // the old numbers in place would make a stale readout look live.
        if (live)
            for (int i = 0; i < mixxynth::kNumModDests; ++i)
                modValues[static_cast<size_t> (i)].store (values[static_cast<size_t> (i)],
                                                          std::memory_order_relaxed);
    }
}

float MiXXynthProcessor::getShapePhase (int index) const
{
    index = juce::jlimit (0, mixxynth::kMaxShapes - 1, index);
    return shapePhases[static_cast<size_t> (index)].load (std::memory_order_relaxed);
}

float MiXXynthProcessor::getModulatedValue (int destIndex) const
{
    destIndex = juce::jlimit (0, mixxynth::kNumModDests - 1, destIndex);
    return modValues[static_cast<size_t> (destIndex)].load (std::memory_order_relaxed);
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

    // The module counts travel with the node, so the patch reloads with the same
    // modules it was saved with.
    shapesNode.setProperty ("numLfos", getNumShapeModules() > 0
                                        ? shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)].numLfos
                                        : 0, nullptr);
    shapesNode.setProperty ("numEnvs", shapeBuffers[activeShapeBuffer.load (std::memory_order_acquire)].numEnvs, nullptr);

    for (int i = 0; i < getNumShapeModules(); ++i)
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

    // Restore the modules before handing the tree to APVTS so a missing or partial
    // "shapes" node simply leaves the defaults in place.
    if (auto shapesNode = tree.getChildWithName ("shapes"); shapesNode.isValid())
    {
        const int lfos = juce::jlimit (0, mixxynth::kMaxLfos,
                                       static_cast<int> (shapesNode.getProperty ("numLfos", -1)));
        const int envs = juce::jlimit (0, mixxynth::kMaxModEnvs,
                                       static_cast<int> (shapesNode.getProperty ("numEnvs", -1)));

        // A node from an older build has no counts; fall back to its child list.
        const int numLfos = lfos >= 0 ? lfos : juce::jmin (mixxynth::kMaxLfos, shapesNode.getNumChildren());
        const int numEnvs = envs >= 0 ? envs : juce::jlimit (0, mixxynth::kMaxModEnvs,
                                                             shapesNode.getNumChildren() - numLfos);

        const int total = juce::jmin (mixxynth::kMaxShapes, numLfos + numEnvs);

        {
            const int current = activeShapeBuffer.load (std::memory_order_acquire);
            const int next = 1 - current;

            shapeBuffers[next] = shapeBuffers[current];
            shapeBuffers[next].numLfos = numLfos;
            shapeBuffers[next].numEnvs = total - numLfos;

            for (int i = 0; i < total; ++i)
            {
                const bool isLfo = i < numLfos;
                const auto fallback = isLfo ? mixxynth::ModShape::defaultLfo()
                                            : mixxynth::ModShape::defaultEnv();

                mixxynth::ModShape shape;
                mixxynth::shapeFromValueTree (fallback, shapesNode.getChild (i), shape);
                shapeBuffers[next].shapes[static_cast<size_t> (i)] = shape;
            }

            activeShapeBuffer.store (next, std::memory_order_release);
        }
    }

    apvts.replaceState (tree);

    // The rack's slot types live in parameters, so it has to be told what the
    // loaded state asks for - and any module it has not built yet is created now.
    syncFxRack();

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
