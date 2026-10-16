#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ModTarget.h"

namespace mixxynth::ui
{
// M6: a square grid cell that is both a value readout and a vertical slider.
//
//   drag the body        set the value (up = more)
//   drag a white edge    set that assignment's modulation depth
//   drop a source on it  assign that source to this cell
//   right-click          menu: assigned sources, amount presets, clear
//
// The fill from the bottom is the value, so a column of cells reads as a bar
// chart rather than as a wall of dials. The colour says whether the value is
// currently being modulated, and the white lines say how far the modulation can
// push it - which is the pair of things a bare number cannot show.
class SquareCell : public juce::Component,
                   public juce::DragAndDropTarget,
                   public ModTarget,
                   public sst::jucegui::data::Continuous::DataListener
{
public:
    SquareCell (const juce::String& labelText,
                juce::RangedAudioParameter* param,
                int destIndex,
                bool showValueText = true);
    ~SquareCell() override;

    void paint (juce::Graphics&) override;

    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDragEnter (const SourceDetails&) override { modDragEnter(); }
    void itemDragExit (const SourceDetails&) override { modDragExit(); }
    void itemDropped (const SourceDetails&) override;

    // ---- ModTarget ---------------------------------------------------------
    juce::RangedAudioParameter* getParam() const override { return source.p; }
    juce::String getLabelText() const override { return text; }
    void repaintForMod() override { repaint(); }
    void refreshFromParam() override { repaint(); }
    bool isAdjusting() const noexcept override { return draggingValue || draggingDepth; }

    // ---- live modulation ---------------------------------------------------
    // The modulated value, pushed by the editor's timer. Negative means the cell
    // is not modulated, which is when it falls back to the parameter's own value
    // and paints orange.
    void setLiveValue01 (float v01);

    // Overrides the text drawn in the cell. The operator shape cell uses this to
    // say "sine" rather than "0.000", which is the difference between a readout and
    // a mystery number.
    void setValueTextFunction (std::function<juce::String (float)> f);

    // The text the cell would draw right now. Lets the audit check that a shape
    // cell names its shape rather than showing a bare number.
    juce::String valueTextForTest() const;

    // ---- test seams --------------------------------------------------------
    // What the cell is showing: the live value if modulated, else the parameter.
    float value01() const;
    bool isShowingLiveValue() const noexcept { return liveValue01 >= 0.0f; }
    juce::Colour fillColour() const;
    juce::Rectangle<float> fillRect() const;
    float yForValue01 (float v01) const;
    static float valueFromDrag (float startValue01, float startY, float currentY);

    // ---- Continuous::DataListener ------------------------------------------
    // The cell draws itself from the parameter, but it is not the widget that
    // owns it, so it has to repaint when the parameter moves for any reason.
    void dataChanged() override { repaint(); }
    void sourceVanished (sst::jucegui::data::Continuous*) override {}

private:
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // Index of the assignment whose outer depth edge is under this point, or -1.
    int depthEdgeAt (juce::Point<int> pos) const;

    void setParamFrom01 (float v01);

    ContinuousSource source;
    juce::String text;
    bool showValue = true;
    float liveValue01 = -1.0f;
    std::function<juce::String (float)> valueTextFunction;

    bool draggingValue = false;
    float dragStartValue01 = 0.0f;
    float dragStartY = 0.0f;
    float dragValue01 = 0.0f;

    bool draggingDepth = false;
    int depthDragIndex = -1;
    int depthDragSrc = 0;
    float depthDragStartDepth = 0.0f;
    float depthDragStartY = 0.0f;
    float depthDragDepth = 0.0f;

    int overDepthIndex = -1;

    // How close to a depth line counts as grabbing it. The edge has to win over
    // the value drag, or a cell that is also a slider could never have its depth
    // set - which is every cell.
    static constexpr float edgeGrab = 4.0f;
    static constexpr float textHeight = 12.0f;

    // 140 px of travel covers the whole range: slower than the 110 px a
    // modulation depth uses, because a value is edited far more often and wants
    // the finer control.
    static constexpr float valueDragPixels = 140.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquareCell)
};
}
