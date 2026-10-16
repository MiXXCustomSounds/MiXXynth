#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <sst/jucegui/components/Knob.h>

#include "ModTarget.h"

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
    //
    // M6: everything a drop, a menu or a depth drag DOES now lives in ModTarget,
    // because SquareCell has to behave identically. What is left here is what is
    // genuinely a knob: the dial, and the arcs drawn around it.
    class ModKnob : public juce::Component,
                    public juce::DragAndDropTarget,
                    public ModTarget,
                    public sst::jucegui::data::Continuous::DataListener
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

        // The modulation arcs are drawn by THIS component, outside the bounds of the
        // sst knob child, so repainting only the child leaves them stale. That is
        // why the arcs are anchored to the knob's value now: a stale arc used to be
        // indistinguishable from a fresh one, and it is not any more.
        void dataChanged() override { repaint(); }
        void sourceVanished (sst::jucegui::data::Continuous*) override {}

        // ---- ModTarget -------------------------------------------------------
        juce::RangedAudioParameter* getParam() const override { return source.p; }
        juce::String getLabelText() const override { return text; }
        void repaintForMod() override { repaint(); }

        // The parameter this knob drives (used by the editor audit).
        juce::String getParamID() const
        {
            return source.p != nullptr ? source.p->getParameterID() : juce::String {};
        }

        void setLabelText (const juce::String& t)
        {
            if (text != t)
            {
                text = t;
                repaint();
            }
        }

        // True while the ring is being dragged, so the editor can avoid
        // overwriting the in-progress value.
        bool isAdjustingRing() const noexcept { return draggingRing; }

        // For the editor audit. "A second source is draggable" is a geometry claim
        // that a screenshot cannot verify, so expose both halves of it: a point
        // sitting on an arc, and which arc a point would grab.
        juce::Point<int> arcPointForTest (int index) const
        {
            const auto sq = dialArea();
            const float r = arcRadiusFor (index);

            return juce::Point<int> (juce::roundToInt (sq.getCentreX()),
                                     juce::roundToInt (sq.getCentreY() - r));
        }

        int ringIndexAtForTest (juce::Point<int> pos) const { return ringIndexAt (pos); }

        // The angular span arc `index` currently covers, in radians clockwise from
        // twelve o'clock. It depends on the knob's VALUE, which is precisely why the
        // parent has to repaint when the value moves - the bug this replaced was a
        // stale arc, and it was invisible while the arc was anchored to a fixed
        // angle instead.
        std::pair<float, float> arcAnglesForTest (int index) const;

        // The assignment list, the total depth and the five mod callbacks are all
        // inherited from ModTarget. Only the drawing and the ring dragging are ours.

        void refreshFromParam() override;

    private:
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        juce::Rectangle<float> dialArea() const;

        // Index into `assignments` of the arc under this point, or -1.
        //
        // Arcs step outwards from the dial edge, so several can be live at once and
        // the nearest one wins. The old code assumed there was only ever one
        // draggable arc, which is why a knob with two sources could only have its
        // first one adjusted by dragging.
        int ringIndexAt (juce::Point<int> pos) const;

        // Where an arc sits, in dial-local coordinates.
        float arcRadiusFor (int index) const;

        // The knob's own normalised value, which the arcs are anchored to.
        float value01() const;


        // The ring has to stay grabbable without eating the dial on small knobs,
        // so it scales with the cell rather than being a fixed width.
        //
        // Wider than it looks like it needs to be, because up to three arcs stack
        // outwards from the dial edge at 3.5 px apart. At the old 5-13 px the
        // outermost arc had nowhere to live and the band was too thin to hit
        // reliably.
        static int ringWidthFor (int shortSide)
        {
            return juce::jlimit (10, 22, shortSide / 5);
        }

        sst::jucegui::components::Knob knob;
        ContinuousSource source;
        juce::String text;
        bool labelVisible = true;

        // Which arc the pointer is over, or -1. Replaces a bool, because more than
        // one arc can be live and the hover highlight has to follow the right one.
        int overRingIndex = -1;

        bool draggingRing = false;
        int ringDragIndex = 0;   // index into `assignments`, not a source index
        int ringDragSrc = 0;
        float ringDragStartDepth = 0.0f;
        float ringDragStartY = 0.0f;
        float ringDragDepth = 0.0f;

        static constexpr int labelHeight = 14;
        static constexpr int ringWidth = 13;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModKnob)
    };
}
