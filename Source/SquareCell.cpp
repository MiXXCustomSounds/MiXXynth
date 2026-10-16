#include "SquareCell.h"

#include <cmath>

#include "ModMatrix.h"

namespace mixxynth::ui
{
namespace
{
    // Near-black, so a row of cells reads as a grid; the border is dark enough
    // not to compete with the fill but present enough to delineate the cell.
    const juce::Colour cellBackground { 0xff101418 };
    const juce::Colour cellBorder     { 0xff2a323d };
    const juce::Colour valueText      { 0xffe2e8ee };

    // The plugin's accent. A cell at 0% is black, so orange means "there is a
    // value here" and blue means "and it is moving".
    const juce::Colour accentOrange   { 0xffffa53d };
    const juce::Colour modulatedBlue  { 0xff3aa0ff };

    const juce::Colour acceptGreen    { 0xff4ade80 };
    const juce::Colour rejectRed      { 0xffef4444 };
}

SquareCell::SquareCell (const juce::String& labelText,
                        juce::RangedAudioParameter* param,
                        int destIndex,
                        bool showValueText)
    : ModTarget (destIndex), source (param), text (labelText), showValue (showValueText)
{
    // The cell draws itself from the parameter, so it has to hear about changes
    // that did not come from a drag: a preset load, the dice, host automation.
    source.addGUIDataListener (this);
}

SquareCell::~SquareCell()
{
    // Must happen before `source` is destroyed: its destructor walks the listener
    // list calling sourceVanished, and by then this object is gone.
    source.removeGUIDataListener (this);
}

float SquareCell::value01() const
{
    if (isShowingLiveValue())
        return juce::jlimit (0.0f, 1.0f, liveValue01);

    // Already normalised 0..1 by ContinuousSource, which is exactly the fraction
    // the fill height wants.
    return source.getValue();
}

void SquareCell::setLiveValue01 (float v01)
{
    const float clamped = v01 < 0.0f ? -1.0f : juce::jlimit (0.0f, 1.0f, v01);

    if (! juce::approximatelyEqual (clamped, liveValue01))
    {
        liveValue01 = clamped;
        repaint();
    }
}

void SquareCell::setValueTextFunction (std::function<juce::String (float)> f)
{
    valueTextFunction = std::move (f);
    repaint();
}

juce::String SquareCell::valueTextForTest() const
{
    // At the ends of the range the fill says everything the number would, so an
    // opt-in cell drops the text entirely. Only the routing depths opt in: see the
    // note on setHideTextAtExtremes for why a level must keep reporting its zero.
    if (hideAtExtremes)
    {
        const float v = value01();

        if (v <= 0.0005f || v >= 0.9995f)
            return {};
    }

    if (valueTextFunction != nullptr)
        return valueTextFunction (value01());

    return source.p != nullptr ? source.p->getText (value01(), 8) : juce::String {};
}

juce::Colour SquareCell::fillColour() const
{
    return isShowingLiveValue() ? modulatedBlue : accentOrange;
}

juce::Rectangle<float> SquareCell::fillRect() const
{
    auto b = getLocalBounds().toFloat().reduced (1.5f);
    return b.removeFromBottom (b.getHeight() * juce::jlimit (0.0f, 1.0f, value01()));
}

float SquareCell::yForValue01 (float v01) const
{
    auto b = getLocalBounds().toFloat().reduced (1.5f);
    return b.getBottom() - b.getHeight() * juce::jlimit (0.0f, 1.0f, v01);
}

float SquareCell::valueFromDrag (float startValue01, float startY, float currentY)
{
    const float delta = (startY - currentY) / valueDragPixels;
    return juce::jlimit (0.0f, 1.0f, startValue01 + delta);
}

void SquareCell::setParamFrom01 (float v01)
{
    source.setValueFromGUI (juce::jlimit (0.0f, 1.0f, v01));
}

void SquareCell::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();

    g.setColour (cellBackground);
    g.fillRoundedRectangle (b, 3.0f);

    // The value, filled from the bottom up.
    const auto fill = fillRect();

    if (fill.getHeight() > 0.5f)
    {
        g.setColour (fillColour());
        g.fillRect (fill);
    }

    if (isRejectFlashing())
    {
        g.setColour (rejectRed);
        g.drawRoundedRectangle (b.reduced (1.0f), 3.0f, 2.5f);
    }
    else if (isDragOver())
    {
        // Green when the cell can be modulated, red when the drop would be
        // refused, so the gesture is legible before the mouse is released.
        g.setColour (isModTarget() ? acceptGreen : rejectRed);
        g.drawRoundedRectangle (b.reduced (1.0f), 3.0f, 2.0f);
    }
    else
    {
        g.setColour (cellBorder);
        g.drawRoundedRectangle (b.reduced (0.5f), 3.0f, 1.0f);
    }

    if (showValue && source.p != nullptr)
    {
        // The parameter's own text unless something more legible was supplied: a
        // shape cell wants to say "sine", not "0.000".
        g.setColour (valueText);
        g.setFont (11.0f);
        g.drawFittedText (valueTextForTest(),
                          getLocalBounds().removeFromTop (static_cast<int> (textHeight) + 2)
                                          .reduced (3, 1),
                          juce::Justification::centred, 1);
    }

    // One white line per assignment, drawn at value + depth, so you can see where
    // the modulation is aiming and how far it can reach. The line's position is
    // the grab handle for that assignment's amount.
    for (int i = 0; i < getNumAssignments(); ++i)
    {
        const bool isDragged = draggingDepth && depthDragIndex == i;
        const float depth = effectiveDepth (i, isDragged, depthDragDepth);
        const float y = yForValue01 (value01() + depth);
        const bool hot = isDragged || overDepthIndex == i;

        g.setColour (hot ? accentOrange : juce::Colours::white.withAlpha (0.85f));
        g.fillRect (b.getX() + 2.0f, y - (hot ? 1.5f : 1.0f),
                    b.getWidth() - 4.0f, hot ? 3.0f : 2.0f);
    }
}

int SquareCell::depthEdgeAt (juce::Point<int> pos) const
{
    int best = -1;
    float bestDistance = edgeGrab;

    for (int i = 0; i < getNumAssignments(); ++i)
    {
        const float y = yForValue01 (value01() + getAssignments()[static_cast<size_t> (i)].depth);
        const float distance = std::abs (static_cast<float> (pos.y) - y);

        if (distance <= bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

void SquareCell::itemDropped (const SourceDetails& details)
{
    modDragDropped (static_cast<int> (details.description.toString().getIntValue()));
}

void SquareCell::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showModMenu (this);
        return;
    }

    // The depth edge is tested FIRST, and everything else drags the value. One
    // cell therefore has two drag targets, and this is the rule that keeps them
    // from fighting: a cell with no assignments has no edge, so there is no
    // ambiguity at all.
    const int edge = depthEdgeAt (e.getPosition());

    if (edge >= 0 && edge < static_cast<int> (getAssignments().size()))
    {
        const auto& a = getAssignments()[static_cast<size_t> (edge)];

        draggingDepth = true;
        depthDragIndex = edge;
        depthDragSrc = a.source;
        depthDragStartDepth = a.depth;
        depthDragDepth = a.depth;
        depthDragStartY = e.position.y;
        repaint();
        return;
    }

    draggingValue = true;
    dragStartValue01 = value01();
    dragValue01 = dragStartValue01;
    dragStartY = e.position.y;
    repaint();
}

void SquareCell::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingDepth)
    {
        const float depth = depthFromDrag (depthDragStartDepth, depthDragStartY, e.position.y);

        if (juce::approximatelyEqual (depth, depthDragDepth))
            return;

        depthDragDepth = depth;

        if (onModDepthChanged != nullptr)
            onModDepthChanged (depthDragSrc, getDestIndex(), depth);

        if (onStatus != nullptr)
        {
            const int src = juce::jlimit (0, mixxynth::kNumModSources - 1, depthDragSrc);
            onStatus (text + ": " + mixxynth::modSourceNames()[src]
                        + " amount " + formatModDepth (depth));
        }

        repaint();
        return;
    }

    if (! draggingValue)
        return;

    const float v = valueFromDrag (dragStartValue01, dragStartY, e.position.y);

    if (juce::approximatelyEqual (v, dragValue01))
        return;

    dragValue01 = v;
    setParamFrom01 (v);

    if (onStatus != nullptr && source.p != nullptr)
        onStatus (text + ": " + source.p->getText (v, 8));

    repaint();
}

void SquareCell::mouseUp (const juce::MouseEvent&)
{
    if (draggingValue || draggingDepth)
    {
        draggingValue = false;
        draggingDepth = false;
        repaint();
    }
}

void SquareCell::mouseMove (const juce::MouseEvent& e)
{
    const int index = depthEdgeAt (e.getPosition());

    if (index != overDepthIndex)
    {
        overDepthIndex = index;
        setMouseCursor (index >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                   : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void SquareCell::mouseExit (const juce::MouseEvent&)
{
    if (overDepthIndex >= 0)
    {
        overDepthIndex = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}
}
