#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>
#include <sst/jucegui/components/Knob.h>

#include "ParamSources.h"

namespace mixxynth::ui
{
    // M3a: a knob that accepts a dragged modulation source and shows the
    // resulting depth as a ring around the dial.
    //   - drop a source chip on it  -> creates/fills a modulation slot
    //   - right-click               -> clears every slot targeting this knob
    class ModKnob : public juce::Component,
                    public juce::DragAndDropTarget
    {
    public:
        ModKnob (const juce::String& labelText,
                 juce::RangedAudioParameter* param,
                 int destIndex,
                 bool showLabel = true);
        ~ModKnob() override;

        void resized() override;
        void paint (juce::Graphics&) override;

        bool isInterestedInDragSource (const SourceDetails&) override { return true; }
        void itemDragEnter (const SourceDetails&) override;
        void itemDragExit (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

        int getDestIndex() const { return dest; }

        // Total signed modulation depth currently aimed at this knob.
        void setModDepth (float d);
        float getModDepth() const { return modDepth; }

        std::function<void (int srcIndex, int destIndex)> onSourceDropped;
        std::function<void (int destIndex)> onClearRequested;

        void refreshFromParam();

    private:
        void mouseDown (const juce::MouseEvent&) override;

        sst::jucegui::components::Knob knob;
        ContinuousSource source;
        juce::String text;
        int dest = 0;
        float modDepth = 0.0f;
        bool dragOver = false;
        bool labelVisible = true;
        static constexpr int labelHeight = 14;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModKnob)
    };
}
