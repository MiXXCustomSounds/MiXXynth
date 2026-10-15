#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ModKnob.h"
#include "WheelComponent.h"

class MiXXynthProcessor;

// M3a editor: split layout.
//   left rail  - modulation source chips, always visible, drag them onto knobs
//   bottom bar - 4 macros + master, always visible
//   centre     - tabbed pages (FM / Matrix / Mod / FX)
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

    MiXXynthProcessor& processor() { return proc; }

    // Every modulatable knob, so depth rings can be refreshed from the slots.
    std::vector<mixxynth::ui::ModKnob*> modKnobs;

    // Read-only tab access for the editor audit.
    int getNumTabs() const { return tabs.getNumTabs(); }
    const juce::StringArray& getTabNames() const { return tabs.getTabNames(); }
    juce::Component* getTabContentComponent (int index) const
    {
        return tabs.getTabContentComponent (index);
    }

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
    class FmPage;
    class MatrixPage;
    class ModPage;
    class VaPage;
    class VoicePage;
    class FxPage;

    MiXXynthProcessor& proc;

    juce::Label title;
    juce::Component rail;
    juce::Label railTitle;
    juce::Component bar;
    juce::Label barTitle;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    juce::OwnedArray<SourceChip> chips;
    juce::OwnedArray<mixxynth::ui::ModKnob> macroKnobs;

    // Performance bar: on-screen keyboard plus pitch and mod wheels.
    juce::Component perfBar;
    std::unique_ptr<juce::MidiKeyboardComponent> keyboard;
    std::unique_ptr<mixxynth::ui::WheelComponent> pitchWheel, modWheel;
    std::unique_ptr<FmPage> fmPage;
    std::unique_ptr<MatrixPage> matrixPage;
    std::unique_ptr<ModPage> modPage;
    std::unique_ptr<VaPage> vaPage;
    std::unique_ptr<VoicePage> voicePage;
    std::unique_ptr<FxPage> fxPage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthEditor)
};
