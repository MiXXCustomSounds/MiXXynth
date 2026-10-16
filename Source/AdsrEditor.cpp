#include "AdsrEditor.h"

#include <cmath>

namespace mixxynth::ui
{
namespace
{
    // The modulation section's ENV colours, so an operator envelope and a
    // modulation envelope are recognisably the same thing.
    const juce::Colour plotBackground { 0xff131318 };
    const juce::Colour gridColour    { 0xff2a2a33 };
    const juce::Colour envAccent     { 0xffffa63d };

    constexpr int gridDivisions = 16;
}

AdsrEditor::AdsrEditor (juce::RangedAudioParameter* attackParam,
                        juce::RangedAudioParameter* decayParam,
                        juce::RangedAudioParameter* sustainParam,
                        juce::RangedAudioParameter* releaseParam)
{
    params[0] = attackParam;
    params[1] = decayParam;
    params[2] = sustainParam;
    params[3] = releaseParam;

    setTooltip ("Drag the three points to shape this operator's envelope: "
                "attack, then decay and sustain, then release.");
}

AdsrEditor::~AdsrEditor() = default;

float AdsrEditor::timeToX (float seconds)
{
    const float t = juce::jlimit (0.0f, windowSeconds, seconds) / windowSeconds;
    return std::pow (t, axisPower);
}

float AdsrEditor::xToTime (float x01)
{
    return std::pow (juce::jlimit (0.0f, 1.0f, x01), 1.0f / axisPower) * windowSeconds;
}

float AdsrEditor::readParam (int index) const
{
    auto* p = params[index];

    return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
}

float AdsrEditor::attackSeconds() const  { return readParam (0); }
float AdsrEditor::decaySeconds() const   { return readParam (1); }
float AdsrEditor::sustainLevel() const   { return juce::jlimit (0.0f, 1.0f, readParam (2)); }
float AdsrEditor::releaseSeconds() const { return readParam (3); }

mixxynth::ModShape AdsrEditor::currentShape() const
{
    const float a = attackSeconds();
    const float d = decaySeconds();
    const float s = sustainLevel();
    const float r = releaseSeconds();

    mixxynth::ModShape shape;
    shape.mode = mixxynth::shapeEnv;
    shape.bipolar = false;
    shape.numPoints = 6;

    // A real ADSR has a plateau at the sustain level between the decay and the note
    // off. Without it the curve decays straight into its release and the sustain
    // reads as a point rather than as a level.
    shape.points[0] = { 0.0f, 0.0f, 0.0f };
    shape.points[1] = { timeToX (a), 1.0f, 0.0f };
    shape.points[2] = { timeToX (a + d), s, 0.0f };
    shape.points[3] = { timeToX (a + d + holdSeconds), s, 0.0f };
    shape.points[4] = { timeToX (a + d + holdSeconds + r), 0.0f, 0.0f };
    // A tail at zero, so the curve reads across the whole width instead of stopping
    // wherever the release happens to end.
    shape.points[5] = { 1.0f, 0.0f, 0.0f };

    return shape;
}

void AdsrEditor::refreshFromParams()
{
    repaintIfChanged();
}

bool AdsrEditor::repaintIfChanged()
{
    const float now[4] { attackSeconds(), decaySeconds(), sustainLevel(), releaseSeconds() };
    bool changed = false;

    for (int i = 0; i < 4; ++i)
    {
        if (! juce::approximatelyEqual (now[i], lastPainted[i]))
        {
            lastPainted[i] = now[i];
            changed = true;
        }
    }

    if (changed)
        repaint();

    return changed;
}

juce::Rectangle<float> AdsrEditor::plotArea() const
{
    return getLocalBounds().toFloat().reduced (4.0f, 5.0f);
}

juce::Point<float> AdsrEditor::toPixel (juce::Point<float> plot) const
{
    const auto area = plotArea();

    // Unipolar: 0 sits on the bottom edge and 1 on the top, which is how an
    // envelope reads, and what the modulation section's envelopes do.
    return { area.getX() + juce::jlimit (0.0f, 1.0f, plot.x) * area.getWidth(),
             area.getBottom() - juce::jlimit (0.0f, 1.0f, plot.y) * area.getHeight() };
}

juce::Point<float> AdsrEditor::handleForTest (int handle) const
{
    const float a = attackSeconds();
    const float d = decaySeconds();
    const float r = releaseSeconds();

    switch (handle)
    {
        case attackPeak:  return { timeToX (a), 1.0f };
        case decayCorner: return { timeToX (a + d), sustainLevel() };
        case releaseEnd:  return { timeToX (a + d + holdSeconds + r), 0.0f };
        default:          break;
    }

    return { 0.0f, 0.0f };
}

int AdsrEditor::handleAt (juce::Point<float> pixel) const
{
    int best = -1;
    float bestDistance = grabRadius;

    for (int h = 0; h < numHandles; ++h)
    {
        const float distance = pixel.getDistanceFrom (toPixel (handleForTest (h)));

        // Nearest wins, and a tie goes to the earlier handle. That matters on the
        // init patch, where every stage is zero and all three handles sit on the
        // origin: the decay corner has to be reachable first, and dragging it out
        // is what separates it from the release end.
        if (distance <= bestDistance)
        {
            bestDistance = distance;
            best = h;
        }
    }

    return best;
}

void AdsrEditor::writeParam (int index, float seconds)
{
    auto* p = params[index];

    if (p == nullptr)
        return;

    const auto& range = p->getNormalisableRange();

    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (range.start, range.end, seconds)));
    p->endChangeGesture();
}

void AdsrEditor::setStage (int handle, float x01, float y01)
{
    const float a = attackSeconds();
    const float d = decaySeconds();

    // The axis is absolute time, so a handle's x is the END of its stage and the
    // stage's own length is the difference from the previous handle. Clamping that
    // difference at zero is what stops a drag past the previous handle from
    // inverting the envelope.
    switch (handle)
    {
        case attackPeak:
            writeParam (0, xToTime (x01));
            break;

        case decayCorner:
            writeParam (1, juce::jmax (0.0f, xToTime (x01) - a));
            writeParam (2, y01);
            break;

        case releaseEnd:
            writeParam (3, juce::jmax (0.0f, xToTime (x01) - a - d - holdSeconds));
            break;

        default:
            return;
    }

    if (onChange != nullptr)
        onChange();

    if (onStatus != nullptr)
        onStatus ("Envelope: attack " + juce::String (attackSeconds(), 2)
                    + "s, decay " + juce::String (decaySeconds(), 2)
                    + "s, sustain " + juce::String (sustainLevel(), 2)
                    + ", release " + juce::String (releaseSeconds(), 2) + "s");

    repaint();
}

void AdsrEditor::dragHandleForTest (int handle, juce::Point<float> to)
{
    setStage (handle, to.x, to.y);
}

void AdsrEditor::mouseDown (const juce::MouseEvent& e)
{
    dragHandle = handleAt (e.position);

    if (dragHandle >= 0)
    {
        repaint();
        return;
    }

    Component::mouseDown (e);
}

void AdsrEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragHandle < 0)
        return;

    const auto area = plotArea();

    if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
        return;

    // The pixel is converted to plot space here and the stage maths happens in
    // setStage, so a drag and a test exercise exactly the same code.
    setStage (dragHandle,
              (e.position.x - area.getX()) / area.getWidth(),
              (area.getBottom() - e.position.y) / area.getHeight());
}

void AdsrEditor::mouseUp (const juce::MouseEvent&)
{
    if (dragHandle >= 0)
    {
        dragHandle = -1;
        repaint();
    }
}

void AdsrEditor::mouseMove (const juce::MouseEvent& e)
{
    const int h = handleAt (e.position);

    if (h != hoveredHandle)
    {
        hoveredHandle = h;
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor
                               : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void AdsrEditor::mouseExit (const juce::MouseEvent&)
{
    if (hoveredHandle >= 0)
    {
        hoveredHandle = -1;
        repaint();
    }
}

void AdsrEditor::paint (juce::Graphics& g)
{
    const auto area = plotArea();
    const auto shape = currentShape();

    g.setColour (plotBackground);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

    g.setColour (gridColour);

    for (int i = 1; i < gridDivisions; ++i)
    {
        const float x = area.getX() + area.getWidth() * static_cast<float> (i)
                          / static_cast<float> (gridDivisions);
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
    }

    for (int i = 1; i < 4; ++i)
    {
        const float y = area.getY() + area.getHeight() * static_cast<float> (i) / 4.0f;
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
    }

    // Curve, from the same builder the modulation envelopes use, so the two cannot
    // disagree about what an envelope looks like.
    juce::Path path;
    buildShapePath (shape, path, area, false);

    juce::Path filled (path);
    filled.lineTo (area.getRight(), area.getBottom());
    filled.lineTo (area.getX(), area.getBottom());
    filled.closeSubPath();

    g.setColour (envAccent.withAlpha (0.16f));
    g.fillPath (filled);

    g.setColour (envAccent);
    g.strokePath (path, juce::PathStrokeType (1.8f));

    // The origin is drawn square because it is fixed, exactly as the modulation
    // editor marks its endpoints.
    g.setColour (envAccent);
    g.fillRect (juce::Rectangle<float> (7.0f, 7.0f).withCentre (toPixel ({ 0.0f, 0.0f })));

    for (int h = 0; h < numHandles; ++h)
    {
        const bool hot = (h == hoveredHandle || h == dragHandle);
        const float r = hot ? 5.0f : 3.5f;

        g.setColour (hot ? juce::Colours::white : envAccent);
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f)
                         .withCentre (toPixel (handleForTest (h))));
    }
}
}
