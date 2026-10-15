#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ModKnob.h"

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

private:
    class SourceChip;
    class FmPage;
    class MatrixPage;
    class ModPage;
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
    std::unique_ptr<FmPage> fmPage;
    std::unique_ptr<MatrixPage> matrixPage;
    std::unique_ptr<ModPage> modPage;
    std::unique_ptr<FxPage> fxPage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiXXynthEditor)
};
