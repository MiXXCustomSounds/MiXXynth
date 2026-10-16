#include "SquareCell.h"

#include <cmath>

#include "ModMatrix.h"
#include "OpWave.h"

namespace mixxynth::ui
{
namespace
{
    // Near-black, so a row of cells reads as a grid; the border is dark enough
    // not to compete with the fill but present enough to delineate the cell.
    const juce::Colour cellBackground { 0xff101418 };
    const juce::Colour cellBorder     { 0xff2a323d };
    const juce::Colour valueText      { 0xffe2e8ee };

    // Dimmer than the value, because the name is a caption and the number is the point.
    // Not called `labelText`: that is the constructor parameter, and the shadow would be
    // both a warning and a trap.
    const juce::Colour captionText    { 0xff8b98a5 };

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

void SquareCell::setWaveformDisplay (const mixxynth::OpWaveTables* tables)
{
    waveTables = tables;
    repaint();
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
    // Blue means "this is being modulated", orange means "this is a value you set".
    //
    // This used to depend ONLY on a live published value, which meant it could never say
    // blue: nothing publishes modulated values yet (it is PLAN-M6 step 5). So the colour
    // was correct in principle and invisible in practice.
    //
    // Whether a cell is modulated is knowable without that path - it has an assignment with
    // a non-zero depth - so the colour is decided from that. The live value still drives the
    // fill once it exists, and the two are not the same question: one says "is this moving",
    // the other says "where is it now".
    const bool modulated = isShowingLiveValue()
                             || std::abs (totalDepth (getAssignments())) > 1.0e-6f;

    return modulated ? modulatedBlue : accentOrange;
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
        // The name, above the value, when the cell has to say what it is. The strip is only
        // taken when a label is actually drawn, so a grid cell - which is already named by
        // its column header - keeps its full height for the value.
        const bool drawLabel = showLabel && text.isNotEmpty();

        auto textArea = getLocalBounds().removeFromTop (static_cast<int> (textHeight) + 2
                                                          + (drawLabel ? static_cast<int> (labelHeight) : 0));

        if (drawLabel)
        {
            g.setColour (captionText);
            g.setFont (9.5f);
            g.drawFittedText (text.toUpperCase(),
                              textArea.removeFromTop (static_cast<int> (labelHeight)).reduced (2, 0),
                              juce::Justification::centred, 1);
        }

        if (waveTables != nullptr)
        {
            // A shape cell draws the shape instead of naming it. The picture keeps telling
            // you which shape you are on the whole way through the morph, which a name can
            // only manage at the seven stops it happens to have words for.
            auto waveArea = b.reduced (5.0f, 6.0f);

            if (drawLabel)
                waveArea = waveArea.withTrimmedTop (labelHeight);

            paintWaveform (g, waveArea);
        }
        else
        {
            // The parameter's own text unless something more legible was supplied: a
            // shape cell wants to say "sine", not "0.000".
            g.setColour (valueText);
            g.setFont (11.0f);
            g.drawFittedText (valueTextForTest(), textArea.reduced (3, 1),
                              juce::Justification::centred, 1);
        }
    }

    // One 'T' per assignment: a stem from the value to where the modulation can push it,
    // with a crossbar at the far end.
    //
    //   depth > 0                    depth < 0
    //      ___                         ^  value
    //       |                          |
    //       |   <- stem                |   <- stem
    //    ^  value                   __|__  <- crossbar
    //
    // The orientation falls out of the arithmetic rather than being special-cased: the
    // crossbar is always at value + depth, so a positive depth puts it above the value and
    // a negative one puts it below. That is what makes a bipolar assignment read as a
    // mirrored 'T' without anything having to decide that it is bipolar.
    //
    // The stem's LENGTH is the depth, so the shape is the amount. The horizontal line this
    // replaced could not show that, because a line has no length to read - it only had a
    // position, and position was already saying where the reach was.
    //
    // The crossbar is also the grab handle, which is why depthEdgeAt tests its y.
    {
        const int n = juce::jmax (1, getNumAssignments());
        const float slot = (b.getWidth() - 6.0f) / static_cast<float> (n);
        const float valueY = yForValue01 (value01());

        for (int i = 0; i < getNumAssignments(); ++i)
        {
            const bool isDragged = draggingDepth && depthDragIndex == i;
            const float depth = effectiveDepth (i, isDragged, depthDragDepth);
            const float reachY = yForValue01 (value01() + depth);
            const bool hot = isDragged || overDepthIndex == i;

            // Assignments sit side by side in their own columns, so several on one cell
            // stay readable instead of drawing over each other.
            const float cx = b.getX() + 3.0f + slot * (static_cast<float> (i) + 0.5f);

            const float top = juce::jmin (valueY, reachY);
            const float bottom = juce::jmax (valueY, reachY);

            const float stemW = hot ? 3.0f : 2.0f;
            const float barW = juce::jmin (slot * 0.72f, 20.0f);
            const float barH = hot ? 3.0f : 2.0f;

            g.setColour (hot ? accentOrange : juce::Colours::white.withAlpha (0.9f));

            // The stem. At least a crossbar tall, so a depth dragged to zero still shows a
            // tick at the value rather than vanishing entirely.
            g.fillRect (cx - stemW * 0.5f, top, stemW, juce::jmax (barH, bottom - top));

            g.fillRect (cx - barW * 0.5f, reachY - barH * 0.5f, barW, barH);
        }
    }
}

void SquareCell::paintWaveform (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (waveTables == nullptr || ! waveTables->isPrepared() || area.getWidth() < 8.0f)
        return;

    // One point per ~2.5 px, bounded at both ends: a wide cell should not spend hundreds of
    // table reads on a thumbnail, and a narrow one should not degrade into a triangle.
    const int n = juce::jlimit (24, 96, static_cast<int> (area.getWidth() / 2.5f));

    juce::Path path;

    for (int i = 0; i <= n; ++i)
    {
        const float phase = static_cast<float> (i) / static_cast<float> (n);

        // Sampled at a low pitch so the drawing shows the shape ITSELF rather than the shape
        // as band-limited at some arbitrary frequency. The mip bands exist so the audio does
        // not alias; the picture wants the ideal waveform.
        const float v = waveTables->sample (phase, value01(), 110.0);

        const float x = area.getX() + area.getWidth() * phase;
        const float y = area.getCentreY() - v * area.getHeight() * 0.5f;

        if (i == 0)
            path.startNewSubPath (x, y);
        else
            path.lineTo (x, y);
    }

    // Drawn over the fill, so it has to read against orange, blue and near-black alike. A
    // dark underlay with a bright line over it does that; no single colour can.
    g.setColour (juce::Colour (0xff0b0d10).withAlpha (0.65f));
    g.strokePath (path, juce::PathStrokeType (3.0f));

    g.setColour (valueText);
    g.strokePath (path, juce::PathStrokeType (1.4f));
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

    float v = valueFromDrag (dragStartValue01, dragStartY, e.position.y);

    if (juce::approximatelyEqual (v, dragValue01))
        return;

    dragValue01 = v;
    setParamFrom01 (v);

    if (onStatus != nullptr && source.p != nullptr)
    {
        // The cell's own wording when it has one. A shape cell wants to say "Square" while
        // you drag through the morph, not "0.333" - and now that the cell DRAWS the shape
        // rather than naming it, this is the only place the name appears at all.
        const juce::String shown = valueTextFunction != nullptr
                                     ? valueTextFunction (v)
                                     : source.p->getText (v, 8);

        onStatus (text + ": " + shown);
    }

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
