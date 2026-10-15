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
    // A knob whose dest is dstOff cannot be a modulation target, so a drop on it
    // is rejected with a visible flash instead of silently doing nothing.
    class ModKnob : public juce::Component,
                    public juce::DragAndDropTarget,
                    private juce::Timer
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

        // True when this knob is a valid modulation destination.
        bool isModTarget() const { return dest != 0; }

        // The parameter this knob drives (used by the editor audit).
        juce::RangedAudioParameter* getParam() const { return source.p; }
        juce::String getParamID() const
        {
            return source.p != nullptr ? source.p->getParameterID() : juce::String {};
        }
        const juce::String& getLabelText() const { return text; }

        // Total signed modulation depth currently aimed at this knob.
        void setModDepth (float d);
        float getModDepth() const { return modDepth; }

        std::function<void (int srcIndex, int destIndex)> onSourceDropped;
        std::function<void (int destIndex)> onClearRequested;

        void refreshFromParam();

    private:
        void mouseDown (const juce::MouseEvent&) override;
        void timerCallback() override;
        void flashRejected();

        sst::jucegui::components::Knob knob;
        ContinuousSource source;
        juce::String text;
        int dest = 0;
        float modDepth = 0.0f;
        bool dragOver = false;
        bool rejectFlash = false;
        bool labelVisible = true;
        static constexpr int labelHeight = 14;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModKnob)
    };
}
