#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <sst/jucegui/components/Knob.h>

#include "ParamSources.h"

namespace mixxynth::ui
{
    // M3a: a knob that accepts a dragged modulation source, shows the resulting
    // depth as a ring around the dial, and lets you set the amount by dragging
    // that ring - the Serum / Vital gesture.
    //
    //   - drop a source chip on it -> creates/fills a modulation slot
    //   - drag the ring            -> sets that slot's depth (up = more)
    //   - right-click              -> menu: assigned sources, amount presets, clear
    //
    // A knob whose dest is dstOff cannot be a modulation target, so a drop on it
    // is rejected with a visible flash instead of silently doing nothing.
    class ModKnob : public juce::Component,
                    public juce::DragAndDropTarget,
                    private juce::Timer
    {
    public:
        // One modulation source aimed at this knob, with its signed depth.
        struct Assignment
        {
            int source = 0;
            float depth = 0.0f;
        };

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

        // The full assignment list, pushed by the editor whenever slots change.
        void setModAssignments (std::vector<Assignment> a);
        int getNumAssignments() const { return static_cast<int> (assignments.size()); }

        // True while the ring is being dragged, so the editor can avoid
        // overwriting the in-progress value.
        bool isAdjustingRing() const noexcept { return draggingRing; }

        std::function<void (int srcIndex, int destIndex)> onSourceDropped;
        std::function<void (int destIndex)> onClearRequested;
        std::function<void (int srcIndex, int destIndex, float depth)> onModDepthChanged;
        std::function<void (int srcIndex, int destIndex)> onModRemoveRequested;
        std::function<void (const juce::String&)> onStatus;

        void refreshFromParam();

    private:
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void timerCallback() override;

        void flashRejected();
        void showMenu();
        void handleMenuResult (int result);
        juce::Rectangle<float> dialArea() const;
        bool isOverRing (juce::Point<int> pos) const;

        sst::jucegui::components::Knob knob;
        ContinuousSource source;
        juce::String text;
        int dest = 0;

        float modDepth = 0.0f;
        std::vector<Assignment> assignments;

        bool dragOver = false;
        bool rejectFlash = false;
        bool labelVisible = true;
        bool overRing = false;

        bool draggingRing = false;
        int ringDragSrc = 0;
        float ringDragStartDepth = 0.0f;
        float ringDragStartY = 0.0f;
        float ringDragDepth = 0.0f;

        static constexpr int labelHeight = 14;
        static constexpr int ringWidth = 13;
        static constexpr int numAmountPresets = 8;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModKnob)
    };
}
