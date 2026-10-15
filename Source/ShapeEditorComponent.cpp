#include "ShapeEditorComponent.h"

namespace mixxynth
{
ShapeEditorComponent::ShapeEditorComponent()
{
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    setShape (ModShape::defaultLfo());
}

void ShapeEditorComponent::setShape (const ModShape& s)
{
    shape = s;
    shape.sortPoints();
    repaint();
}

void ShapeEditorComponent::setAccentColour (juce::Colour c)
{
    accent = c;
    repaint();
}

void ShapeEditorComponent::setGridDivisions (int divisions)
{
    gridDivisions = juce::jlimit (2, 64, divisions);
    repaint();
}

void ShapeEditorComponent::setPlayhead (float phase01)
{
    if (std::abs (phase01 - playhead) < 0.004f)
        return;

    playhead = phase01;
    repaint();
}

juce::Rectangle<float> ShapeEditorComponent::plotArea() const
{
    return getLocalBounds().toFloat().reduced (4.0f, 5.0f);
}

juce::Point<float> ShapeEditorComponent::pointToPixel (const ShapePoint& p) const
{
    const auto area = plotArea();

    const float px = area.getX() + juce::jlimit (0.0f, 1.0f, p.x) * area.getWidth();
    const float py = shape.bipolar
                       ? area.getCentreY() - (juce::jlimit (0.0f, 1.0f, p.y) - 0.5f) * area.getHeight()
                       : area.getBottom() - juce::jlimit (0.0f, 1.0f, p.y) * area.getHeight();

    return { px, py };
}

float ShapeEditorComponent::pixelToX (float px) const
{
    const auto area = plotArea();
    return area.getWidth() > 1.0f ? juce::jlimit (0.0f, 1.0f, (px - area.getX()) / area.getWidth()) : 0.0f;
}

float ShapeEditorComponent::pixelToY (float py) const
{
    const auto area = plotArea();

    if (area.getHeight() <= 1.0f)
        return 0.0f;

    if (shape.bipolar)
        return juce::jlimit (0.0f, 1.0f, (area.getCentreY() - py) / area.getHeight() + 0.5f);

    return juce::jlimit (0.0f, 1.0f, (area.getBottom() - py) / area.getHeight());
}

void ShapeEditorComponent::snapX (float& x) const
{
    const float step = 1.0f / static_cast<float> (gridDivisions);
    x = juce::jlimit (0.0f, 1.0f, std::round (x / step) * step);
}

void ShapeEditorComponent::notify()
{
    if (onChange != nullptr)
        onChange();
}

void ShapeEditorComponent::paint (juce::Graphics& g)
{
    const auto area = plotArea();

    g.setColour (juce::Colour (0xff131318));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

    // Grid.
    g.setColour (juce::Colour (0xff2a2a33));

    for (int i = 1; i < gridDivisions; ++i)
    {
        const float x = area.getX() + area.getWidth() * static_cast<float> (i) / static_cast<float> (gridDivisions);
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
    }

    for (int i = 1; i < 4; ++i)
    {
        const float y = area.getY() + area.getHeight() * static_cast<float> (i) / 4.0f;
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
    }

    if (shape.bipolar)
    {
        g.setColour (juce::Colour (0xff3a3a46));
        g.drawHorizontalLine (juce::roundToInt (area.getCentreY()), area.getX(), area.getRight());
    }

    // Curve, drawn from the same code the DSP uses so they can never disagree.
    juce::Path path;
    buildShapePath (shape, path, area, shape.bipolar);

    juce::Path filled (path);

    if (shape.bipolar)
    {
        filled.lineTo (area.getRight(), area.getCentreY());
        filled.lineTo (area.getX(), area.getCentreY());
    }
    else
    {
        filled.lineTo (area.getRight(), area.getBottom());
        filled.lineTo (area.getX(), area.getBottom());
    }

    filled.closeSubPath();

    g.setColour (accent.withAlpha (0.16f));
    g.fillPath (filled);

    g.setColour (accent);
    g.strokePath (path, juce::PathStrokeType (1.8f));

    if (playhead >= 0.0f)
    {
        const float px = area.getX() + juce::jlimit (0.0f, 1.0f, playhead) * area.getWidth();
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (px), area.getY(), area.getBottom());
    }

    // Breakpoints. Endpoints are drawn square so they read as anchors.
    for (int i = 0; i < shape.numPoints; ++i)
    {
        const auto p = pointToPixel (shape.points[static_cast<size_t> (i)]);
        const bool endpoint = (i == 0 || i == shape.numPoints - 1);
        const bool hot = (i == hoveredPoint || i == dragPoint);
        const float r = hot ? 5.0f : 3.5f;
        const auto box = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p);

        g.setColour (hot ? juce::Colours::white : accent);

        if (endpoint)
            g.fillRect (box);
        else
            g.fillEllipse (box);
    }
}

int ShapeEditorComponent::findPointNear (juce::Point<float> pos) const
{
    int best = -1;
    float bestDist = 10.0f;

    for (int i = 0; i < shape.numPoints; ++i)
    {
        const float d = pointToPixel (shape.points[static_cast<size_t> (i)]).getDistanceFrom (pos);

        if (d <= bestDist)
        {
            bestDist = d;
            best = i;
        }
    }

    return best;
}

int ShapeEditorComponent::findSegmentNear (juce::Point<float> pos) const
{
    const auto area = plotArea();

    if (pos.x < area.getX() || pos.x > area.getRight())
        return -1;

    const float x01 = pixelToX (pos.x);

    for (int i = 0; i < shape.numPoints - 1; ++i)
        if (x01 >= shape.points[static_cast<size_t> (i)].x
              && x01 <= shape.points[static_cast<size_t> (i + 1)].x)
            return i;

    return -1;
}

void ShapeEditorComponent::mouseMove (const juce::MouseEvent& e)
{
    const int hover = findPointNear (e.position);

    if (hover != hoveredPoint)
    {
        hoveredPoint = hover;
        repaint();
    }
}

void ShapeEditorComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredPoint != -1)
    {
        hoveredPoint = -1;
        repaint();
    }
}

void ShapeEditorComponent::mouseDown (const juce::MouseEvent& e)
{
    dragPoint = -1;
    dragSegment = -1;
    dragTension = false;

    const int pointIndex = findPointNear (e.position);
    const int segmentIndex = findSegmentNear (e.position);

    if (e.mods.isPopupMenu())
    {
        if (pointIndex >= 0)
        {
            showContextMenu (pointIndex, -1);
        }
        else if (segmentIndex >= 0)
        {
            // Defer the menu to mouse-up so a right-drag can bend the segment
            // instead (the Vital gesture).
            dragTension = true;
            dragSegment = segmentIndex;
            dragAnchor = e.position;
            dragStartTension = shape.points[static_cast<size_t> (segmentIndex)].tension;
        }

        return;
    }

    if (pointIndex >= 0)
    {
        dragPoint = pointIndex;
        dragAnchor = e.position;
        repaint();
        return;
    }

    if (shape.numPoints < kMaxShapePoints && segmentIndex >= 0)
    {
        float x = pixelToX (e.position.x);

        if (! e.mods.isShiftDown())
            snapX (x);

        const ShapePoint added { x, pixelToY (e.position.y), 0.0f };

        int insertAt = shape.numPoints;

        for (int i = 0; i < shape.numPoints; ++i)
        {
            if (added.x < shape.points[static_cast<size_t> (i)].x)
            {
                insertAt = i;
                break;
            }
        }

        for (int i = shape.numPoints; i > insertAt; --i)
            shape.points[static_cast<size_t> (i)] = shape.points[static_cast<size_t> (i - 1)];

        shape.points[static_cast<size_t> (insertAt)] = added;
        ++shape.numPoints;
        shape.sortPoints();

        dragPoint = -1;

        for (int i = 0; i < shape.numPoints; ++i)
        {
            if (std::abs (shape.points[static_cast<size_t> (i)].x - added.x) < 1.0e-4f)
            {
                dragPoint = i;
                break;
            }
        }

        dragAnchor = e.position;

        if (onStatus != nullptr)
            onStatus ("Point added - drag to place, double-click to delete");

        repaint();
        notify();
    }
}

void ShapeEditorComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragTension && dragSegment >= 0)
    {
        auto& p = shape.points[static_cast<size_t> (dragSegment)];
        p.tension = juce::jlimit (-1.0f, 1.0f,
                                  dragStartTension - (e.position.y - dragAnchor.y) / 70.0f);

        if (onStatus != nullptr)
            onStatus ("Segment " + juce::String (dragSegment + 1)
                      + " curve " + juce::String (p.tension, 2));

        repaint();
        notify();
        return;
    }

    if (dragPoint < 0)
        return;

    auto& p = shape.points[static_cast<size_t> (dragPoint)];
    const bool endpoint = (dragPoint == 0 || dragPoint == shape.numPoints - 1);

    if (! endpoint)
    {
        float x = pixelToX (e.position.x);

        if (! e.mods.isShiftDown())
            snapX (x);

        const float lo = shape.points[static_cast<size_t> (dragPoint - 1)].x + 0.004f;
        const float hi = shape.points[static_cast<size_t> (dragPoint + 1)].x - 0.004f;
        p.x = juce::jlimit (juce::jmin (lo, hi), juce::jmax (lo, hi), x);
    }

    p.y = pixelToY (e.position.y);

    if (onStatus != nullptr)
        onStatus ("Point " + juce::String (dragPoint + 1)
                  + "  x " + juce::String (p.x, 3)
                  + "  y " + juce::String (p.y, 3)
                  + "  curve " + juce::String (p.tension, 2));

    repaint();
    notify();
}

void ShapeEditorComponent::mouseUp (const juce::MouseEvent& e)
{
    if (dragTension && dragSegment >= 0)
    {
        // A right-click without a drag opens the segment menu instead.
        if (e.position.getDistanceFrom (dragAnchor) < 4.0f)
            showContextMenu (-1, dragSegment);

        dragTension = false;
        dragSegment = -1;
    }

    if (dragPoint >= 0)
    {
        dragPoint = -1;
        repaint();
        notify();
    }
}

void ShapeEditorComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int index = findPointNear (e.position);

    if (index <= 0 || index >= shape.numPoints - 1)
        return; // endpoints stay

    for (int i = index; i < shape.numPoints - 1; ++i)
        shape.points[static_cast<size_t> (i)] = shape.points[static_cast<size_t> (i + 1)];

    --shape.numPoints;
    shape.sortPoints();

    if (onStatus != nullptr)
        onStatus ("Point deleted");

    repaint();
    notify();
}

void ShapeEditorComponent::showContextMenu (int pointIndex, int segmentIndex)
{
    juce::PopupMenu menu;

    if (pointIndex >= 0)
    {
        const bool endpoint = (pointIndex == 0 || pointIndex == shape.numPoints - 1);
        const float t = shape.points[static_cast<size_t> (pointIndex)].tension;

        menu.addSectionHeader ("Point " + juce::String (pointIndex + 1) + " curve");
        menu.addItem (1, "Linear", true, std::abs (t) < 0.05f);
        menu.addItem (2, "Ease out (fast start)", true, t > 0.5f);
        menu.addItem (3, "Ease in (slow start)", true, t < -0.5f);
        menu.addSeparator();
        menu.addItem (4, "Delete point", ! endpoint);
    }
    else if (segmentIndex >= 0)
    {
        menu.addSectionHeader ("Segment " + juce::String (segmentIndex + 1));
        menu.addItem (5, "Flatten curve", true);
    }

    menu.addSeparator();
    menu.addItem (6, "Reset shape to default");

    juce::Component::SafePointer<ShapeEditorComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safeThis, pointIndex, segmentIndex] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& s = safeThis->shape;
        bool changed = true;

        if (result == 1 && pointIndex >= 0)
            s.points[static_cast<size_t> (pointIndex)].tension = 0.0f;
        else if (result == 2 && pointIndex >= 0)
            s.points[static_cast<size_t> (pointIndex)].tension = 0.7f;
        else if (result == 3 && pointIndex >= 0)
            s.points[static_cast<size_t> (pointIndex)].tension = -0.7f;
        else if (result == 4 && pointIndex > 0 && pointIndex < s.numPoints - 1)
        {
            for (int i = pointIndex; i < s.numPoints - 1; ++i)
                s.points[static_cast<size_t> (i)] = s.points[static_cast<size_t> (i + 1)];

            --s.numPoints;
            s.sortPoints();
        }
        else if (result == 5 && segmentIndex >= 0)
            s.points[static_cast<size_t> (segmentIndex)].tension = 0.0f;
        else if (result == 6)
            s = s.mode == shapeLfo ? ModShape::defaultLfo() : ModShape::defaultEnv();
        else
            changed = false;

        if (changed)
        {
            safeThis->repaint();
            safeThis->notify();
        }
    });
}
}
