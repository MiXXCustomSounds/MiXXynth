#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "FmVoice.h"
#include "VoiceManager.h"
#include "ModMatrix.h"
#include "Fx/FxRack.h"

// Polyphonic 6-op FM synth with per-voice modulation.
// 32 simultaneous notes, unison stacks up to 16, 64-voice pool, and a 32-slot
// drag-and-drop modulation matrix. The four LFOs and three modulation envelopes
// are editable breakpoint shapes (see ModShape.h) edited in the sidebar, plus
// 4 macros and velocity/mod-wheel/aftertouch/key-track/timbre.
//
// The VA layer, the per-voice filter and the FX tail have all landed since this
// comment was written. Presets and randomize are still outstanding.
class MiXXynthProcessor : public juce::AudioProcessor
{
public:
    MiXXynthProcessor();
    ~MiXXynthProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // Declares MPE capability to the host. This must be a constant (JUCE calls it
    // on the audio thread and expects the same answer every time), so it always
    // reports true; the "MPE Enabled" parameter decides how we interpret MIDI.
    bool supportsMPE() const override { return true; }

    const juce::String getName() const override { return "MiXXynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Init"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Shared with the editor's performance bar so the on-screen keyboard and
    // wheels can show (and drive) live MIDI state.
    juce::MidiKeyboardState keyboardState;
    float getModWheelValue() const { return modWheel; }
    float getAftertouchValue() const { return aftertouch; }
    float getPitchBendValue() const { return pitchBend; }
    void setPitchBendFromUI (float v) { pitchBend = juce::jlimit (-1.0f, 1.0f, v); }
    void setModWheelFromUI (float v) { modWheel = juce::jlimit (0.0f, 1.0f, v); }

    // Editable modulation modules. Shapes 0..numLfos-1 are the LFOs and the rest
    // are the envelopes. Edited on the message thread and read on the audio
    // thread, so they are double-buffered with an atomic publish index: the audio
    // thread never sees a half-written set.
    //
    // `addShapeModule` / `removeShapeModule` change the list. Removing compacts
    // it, so the CALLER must remap any matrix slot that referenced a later module
    // before calling it - the editor does that in removeShapeModule.
    mixxynth::ModShape getShape (int index) const;
    void setShape (int index, const mixxynth::ModShape& shape);
    void resetShapeToDefault (int index);
    mixxynth::ModShape defaultShapeFor (int index) const;

    int getNumShapeModules() const;
    int getNumLfoModules() const;
    bool shapeIsLfo (int index) const;
    bool canAddShapeModule (bool lfo) const;

    // Modulation source index for a module position, matching ModSource ordering.
    int shapeSourceIndex (int index) const;

    // Appends a module of the given kind and returns its index, or -1 if full.
    int addShapeModule (bool lfo);

    // Removes the module at `index`. Callers that own modulation slots must remap
    // them first (see MiXXynthEditor::removeShapeModule).
    void removeShapeModule (int index);

    // Clear every module (used by tests).
    void clearShapeModules();

    // Published once per block for the editor's playheads (negative = idle).
    float getShapePhase (int index) const;

    // The newest voice's modulated value of one destination, in that destination's own
    // parameter units. Meaningless unless getModulatedValuesLive() is true.
    float getModulatedValue (int destIndex) const;

    // Whether anything is currently sounding, and therefore whether the modulated values
    // above mean anything at all.
    bool getModulatedValuesLive() const
    {
        return modValuesLive.load (std::memory_order_relaxed);
    }

    // The shared band-limited morph tables, for the editor's shape cells to draw from.
    //
    // Safe to read off the audio thread. prepare() tracks into vectors that are already
    // the right size, so it never reallocates, and `prepared` is only set once the tables
    // are built - so a reader either sees a complete set or skips. The worst a re-prepare
    // can do is show a half-rebuilt waveform for a single frame.
    const mixxynth::OpWaveTables& getWaveTables() const { return waveTables; }

    // ---- FX rack (M4b workstream 4) ----------------------------------------
    // The rack sits at the end of processBlock, after the voice mix. An Off or
    // bypassed slot is skipped, so an empty rack is a bit-exact passthrough.
    //
    // Slot state lives in ordinary APVTS parameters (fx{n}type / fx{n}bypass /
    // fx{n}p1..p8), so it is host-visible, automatable and serialised for free.
    mixxynth::FxRack& fxRack() { return rack; }
    const mixxynth::FxRack& fxRack() const { return rack; }

    // The type the rack is actually running for a slot, which can lag the
    // parameter by one call while a module is being created.
    int getFxSlotType (int slot) const { return rack.getSlotType (slot); }

    // Publishes every slot's current type to the rack, creating any modules the
    // state asks for. Message thread. Called after a state load.
    void syncFxRack();

    // Slot state in and out of the parameters. The editor reads all eight slots,
    // permutes them for a reorder, and writes them back, so a module's settings
    // always travel with it.
    mixxynth::FxSlotState readFxSlot (int slot) const;
    void writeFxSlot (int slot, const mixxynth::FxSlotState& state);

    // The file a slot's module is using (its impulse response, today). This is
    // STATE rather than a parameter: an APVTS parameter is a number, and audio data
    // is not a number. Stored in the state tree beside the shapes, and carried in
    // presets so a patch does not break when the original file moves.
    void setFxSlotFile (int slot, const juce::String& name, const juce::MemoryBlock& data);
    juce::String getFxSlotFileName (int slot) const;
    juce::MemoryBlock getFxSlotFileData (int slot) const;

    // Bumped whenever the host loads a state, so the editor can resync its
    // snapshots instead of mistaking a preset load for a user edit.
    int getStateLoadCount() const { return stateLoadCount.load (std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Routes per-note MPE expression into the voice manager.
    class MpeHandler : public juce::MPEInstrument::Listener
    {
    public:
        explicit MpeHandler (MiXXynthProcessor& p) : proc (p) {}

        void noteAdded (juce::MPENote note) override;
        void noteReleased (juce::MPENote note) override;
        void notePressureChanged (juce::MPENote note) override;
        void notePitchbendChanged (juce::MPENote note) override;
        void noteTimbreChanged (juce::MPENote note) override;
        void noteKeyStateChanged (juce::MPENote) override {}

    private:
        MiXXynthProcessor& proc;
    };

    MiXXVoiceManager voiceManager;
    juce::MPEInstrument mpeInstrument;
    MpeHandler mpeHandler { *this };
    int appliedMemberChannels = -1; // last zone layout pushed to the instrument

    // Snapshot of the current block's parameters, so the MPE callbacks can reach
    // the modulation and polyphony settings.
    mixxynth::ModParams currentMod;
    MiXXVoiceManager::PolyParams currentPoly;
    bool mpeActive = false;

    float currentSampleRate = 44100.0f;
    float modWheel = 0.0f;
    float aftertouch = 0.0f;
    float pitchBend = 0.0f; // -1..+1, from the host or the on-screen wheel

    std::array<mixxynth::ShapeSet, 2> shapeBuffers;
    std::atomic<int> activeShapeBuffer { 0 };
    std::array<std::atomic<float>, mixxynth::kMaxShapes> shapePhases {};

    // The newest voice's modulated destination values, in parameter units. Same relaxed
    // single-writer (audio) / single-reader (UI) contract as shapePhases.
    std::array<std::atomic<float>, mixxynth::kNumModDests> modValues {};

    // False when nothing is sounding. Without it the editor would keep showing the last
    // values it saw, and a stale readout that looks live is worse than none.
    std::atomic<bool> modValuesLive { false };
    std::atomic<int> stateLoadCount { 0 };

    // Shared band-limited operator waveform tables (M4b). One copy for the whole
    // plugin; the voices hold a pointer to it.
    mixxynth::OpWaveTables waveTables;

    // M4b workstream 4: the FX rack. Lives at the end of the signal path.
    mixxynth::FxRack rack;

    // Per-block snapshot of the slot types, so processBlock reads each parameter
    // once rather than once per slot per block.
    std::array<int, mixxynth::kMaxFxSlots> fxTypes {};
    std::array<bool, mixxynth::kMaxFxSlots> fxBypass {};
    std::array<float, mixxynth::kMaxFxSlots * mixxynth::kMaxFxParams> fxParams {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthProcessor)
};
