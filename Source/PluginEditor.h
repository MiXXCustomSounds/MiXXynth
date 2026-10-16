#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "AdsrEditor.h"
#include "ModKnob.h"
#include "ModShape.h"
#include "ShapeEditorComponent.h"
#include "WheelComponent.h"

class MiXXynthProcessor;

// M3a editor: split layout.
//   left sidebar - the four LFO and three ENV shapes, drawn and edited inline,
//                  plus a drag chip per source and the performance chips.
//                  Always visible, whatever tab you are on, and the chips drag
//                  onto any knob anywhere.
//   bottom bar   - 4 macros + master, always visible
//   centre       - tabbed pages (FM / Matrix / VA / Voice / Mod / FX)
// Look is intentionally plain for now; the black-metal / dark-oak / glow pass
// comes later.
class MiXXynthEditor : public juce::AudioProcessorEditor,
                       public juce::DragAndDropContainer,
                       public juce::Timer
{
public:
    explicit MiXXynthEditor (MiXXynthProcessor&);
    ~MiXXynthEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

    // Drag-and-drop entry points used by ModKnob.
    void assignModSlot (int srcIndex, int destIndex);
    void clearModDest (int destIndex);
    void refreshModRings();

    // Called by the sidebar strips when a shape is edited, and for status text.
    void shapeEdited (int index, const mixxynth::ModShape& shape);
    void setModStatus (const juce::String& text);

    // LFO/ENV modules. Adding appends to the module's own family; removing
    // compacts the list and remaps any matrix slot that referenced a later
    // module, so links keep pointing at the same module.
    void addShapeModule (bool lfo);
    void removeShapeModule (int index);
    void rebuildShapeStrips();

    // Exposed so the audit can check add/remove without driving buttons.
    int getNumShapeModules() const;
    int getNumLfoModules() const;

    // Wires the shared drag-drop and depth callbacks onto any modulation control.
    // Knobs and cells both inherit ModTarget, so this is one function rather than
    // one per control kind.
    void wireModTarget (mixxynth::ui::ModTarget& k);

    // Sets the amount of the modulation linking (source, destination), creating
    // the slot if the pair is not assigned yet. Backs the draggable depth ring.
    void setModDepthFor (int srcIndex, int destIndex, float depth);

    // Removes just the one slot linking (source, destination).
    void removeModSlot (int srcIndex, int destIndex);

    // Index of the slot linking (source, destination), or -1.
    int findModSlot (int srcIndex, int destIndex) const;

    // Runs the per-tab polling that the editor timer normally does. Exposed so
    // the editor audit can drive the Matrix page's mode-switch logic directly.
    void runTabPolls();

    // Pushes the newest voice's modulated values into the controls, so a modulated cell
    // moves and turns blue instead of only being able to say that it could.
    void pushModulatedValues();

    // Rolls the FM tab's dice. Exposed so the audit can check the roll's
    // invariants without driving a button.
    void rollFmDice();

    // The FM page's per-operator envelope editors, in operator order. They are not
    // ModTargets - an operator envelope only controls its own operator - so they are
    // not in modTargets and need their own way out for the audit.
    std::vector<mixxynth::ui::AdsrEditor*> getAdsrEditors() const;

    // Signature of the last slot configuration, so refreshModRings() can skip
    // rebuilding every knob's assignment list when nothing has changed.
    juce::uint64 modSignature = 0;
    bool modSignatureValid = false;

    // Published per-voice shape phase for the playhead, or negative when idle.
    float shapePhaseFor (int index) const;

    MiXXynthProcessor& processor() { return proc; }

    // Every modulatable control, so assignments can be refreshed from the slots.
    std::vector<mixxynth::ui::ModTarget*> modTargets;

    // Read-only tab access for the editor audit.
    // getTabNames() returns by value because TabbedComponent builds a fresh
    // StringArray; returning a reference here would dangle.
    int getNumTabs() const { return tabs.getNumTabs(); }
    juce::StringArray getTabNames() const { return tabs.getTabNames(); }
    juce::Component* getTabContentComponent (int index) const
    {
        return tabs.getTabContentComponent (index);
    }

    // Switches the FM tab's view. The audit needs this to inspect all three views, and
    // in particular to check that switching views does not leave the previous view's
    // controls behind - which is a bug the page shipped once already.
    void setFmView (int v);

    // Switches the envelope section's page. Same reason as setFmView: page two's controls are
    // only laid out while it is showing, so the audit cannot check them without driving it.
    void setFmAdsrPage (int page);

    // Geometry dump for the editor audit (modulation sidebar).
    juce::String sidebarDebug() const;

    // ---- FX rack, driven by the editor audit -------------------------------
    // These call the same methods the page's own buttons call, so the checks
    // exercise the real code path rather than a parallel implementation.
    void fxAddModule (int type);
    void fxRemoveModule (int slot);
    void fxMoveModule (int slot, int delta);

    // Drives a card's choice combo, so the audit exercises the real binding.
    void fxSetCardChoice (int cardIndex, int boxIndex, int choiceIndex);

    // Rebuilds the cards synchronously. The page defers this when a button
    // triggers it (the card would be deleted under its own callback), and the
    // audit does not run a message loop, so it asks for it directly.
    void fxRebuildCards();

    // Card and control counts plus a zero-bounds tally, for the audit.
    juce::String fxDebug() const;

    // What a Mod tab row is currently displaying, for the audit. A drag-and-drop
    // desync is invisible in the parameters, so it has to be checked here.
    juce::String modDebug (int slot) const;

    // Geometry dump for the editor audit (performance bar layout).
    juce::String perfBarDebug() const
    {
        juce::String s;
        s << "perfBar=" << perfBar.getBounds().toString()
          << " keyboard=" << (keyboard != nullptr ? keyboard->getBounds().toString() : juce::String ("null"))
          << " pitch=" << (pitchWheel != nullptr ? pitchWheel->getBounds().toString() : juce::String ("null"))
          << " mod=" << (modWheel != nullptr ? modWheel->getBounds().toString() : juce::String ("null"));
        return s;
    }

private:
    class SourceChip;
    class ShapeStrip;
    class FmPage;
    class FxPage;

    MiXXynthProcessor& proc;

    juce::Label title;
    juce::Component rail;
    juce::Label railTitle;
    juce::Viewport modViewport;

    // The voice section, which lives in the sidebar above the modulation section rather
    // than on a tab of its own. Fewer tabs, and the voice controls are things you set once
    // and then want to keep an eye on while working on the oscillator - which a tab you
    // have to switch to does not give you.
    juce::Label voiceCaption;
    juce::OwnedArray<mixxynth::ui::ModKnob> voiceKnobs;
    juce::ToggleButton* mpeToggle = nullptr;
    juce::Slider* mpeChannels = nullptr;
    juce::Label channelsLabel;
    juce::Component modContent;
    juce::Label perfChipTitle;
    juce::Label modStatus;
    juce::Component bar;
    juce::Label barTitle;

    // The output limiter's controls, which live in the bar beside Master because that is
    // where they sit in the signal path: last.
    class LimiterMeter;
    std::unique_ptr<LimiterMeter> limiterMeter;
    juce::ToggleButton limiterButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> limiterAttach;
    mixxynth::ui::ModKnob* limiterKnob = nullptr;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    juce::OwnedArray<ShapeStrip> shapeStrips;
    juce::TextButton addLfoButton, addEnvButton;
    juce::OwnedArray<SourceChip> chips;
    juce::OwnedArray<mixxynth::ui::ModKnob> macroKnobs;

    // Performance bar: on-screen keyboard plus pitch and mod wheels.
    juce::Component perfBar;
    std::unique_ptr<juce::MidiKeyboardComponent> keyboard;
    std::unique_ptr<mixxynth::ui::WheelComponent> pitchWheel, modWheel;
    std::unique_ptr<FmPage> fmPage;
    std::unique_ptr<FxPage> fxPage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthEditor)
};
