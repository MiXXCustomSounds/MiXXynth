#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ModShape.h"

namespace mixxynth
{
// Serum/Vital-style breakpoint shape editor.
//
//   left-drag a point     move it (x snaps to the grid unless Shift is held)
//   click empty space     add a point there and start dragging it
//   double-click a point  delete it (endpoints cannot be deleted)
//   right-click           menu: delete point / curve shape / reset
//   right-drag a segment  set that segment's curve tension
//
// Endpoints keep their x positions (0 and 1) and only move vertically, which is
// what both Serum and Vital do. Every edit is reported through onChange.
class ShapeEditorComponent : public juce::Component
{
public:
    ShapeEditorComponent();

    void setShape (const ModShape& s); // does not fire onChange
    const ModShape& getShape() const noexcept { return shape; }

    void setAccentColour (juce::Colour c);
    void setGridDivisions (int divisions);
    void setPlayhead (float phase01); // negative hides it

    // True while a point or segment is being dragged, so callers can avoid
    // overwriting an in-progress edit from an external refresh.
    bool isEditing() const noexcept { return dragPoint >= 0 || dragTension; }

    std::function<void()> onChange;
    std::function<void (const juce::String&)> onStatus;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> pointToPixel (const ShapePoint& p) const;
    float pixelToX (float px) const;
    float pixelToY (float py) const;
    int findPointNear (juce::Point<float> pos) const;
    int findSegmentNear (juce::Point<float> pos) const;
    void snapX (float& x) const;
    void notify();
    void showContextMenu (int pointIndex, int segmentIndex);

    ModShape shape;
    juce::Colour accent { 0xff6ad1ff };
    int gridDivisions = 16;
    float playhead = -1.0f;

    int hoveredPoint = -1;
    int dragPoint = -1;
    int dragSegment = -1;
    bool dragTension = false;
    juce::Point<float> dragAnchor;
    float dragStartTension = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShapeEditorComponent)
};
}
