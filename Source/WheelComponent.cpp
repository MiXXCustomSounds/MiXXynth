#include "WheelComponent.h"

namespace mixxynth::ui
{
WheelComponent::WheelComponent (const juce::String& labelText, bool isBipolar)
    : text (labelText), bipolar (isBipolar)
{
}

juce::Rectangle<float> WheelComponent::wheelBounds() const
{
    auto b = getLocalBounds().toFloat();
    b.removeFromBottom (14.0f); // label strip

    const float w = juce::jmin (b.getWidth(), 26.0f);
    return b.withSizeKeepingCentre (w, b.getHeight()).reduced (1.0f);
}

float WheelComponent::valueForY (float y) const
{
    const auto r = wheelBounds();
    const float t = juce::jlimit (0.0f, 1.0f, 1.0f - (y - r.getY()) / juce::jmax (1.0f, r.getHeight()));

    return bipolar ? (t * 2.0f - 1.0f) : t;
}

void WheelComponent::paint (juce::Graphics& g)
{
    const auto r = wheelBounds();

    g.setColour (juce::Colour (0xff151a20));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (juce::Colour (0xff3c4650));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    // Centre detent line for the bipolar wheel.
    if (bipolar)
    {
        g.setColour (juce::Colour (0xff2c343c));
        g.drawLine (r.getX() + 3.0f, r.getCentreY(), r.getRight() - 3.0f, r.getCentreY(), 1.0f);
    }

    // Grip line.
    const float t = bipolar ? (value * 0.5f + 0.5f) : value;
    const float y = r.getBottom() - t * r.getHeight();

    g.setColour (juce::Colour (0xffffa53d));
    g.fillRoundedRectangle (r.getX() + 2.0f, y - 2.0f, r.getWidth() - 4.0f, 4.0f, 2.0f);

    g.setColour (juce::Colour (0xff8b98a5));
    g.setFont (10.0f);
    g.drawFittedText (text, getLocalBounds().removeFromBottom (13), juce::Justification::centred, 1);
}

void WheelComponent::setFromMouse (const juce::MouseEvent& e)
{
    const float v = valueForY (static_cast<float> (e.position.y));

    if (! juce::approximatelyEqual (v, value))
    {
        value = v;

        if (onValueChanged != nullptr)
            onValueChanged (value);

        repaint();
    }
}

void WheelComponent::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;
    stopTimer();
    setFromMouse (e);
}

void WheelComponent::mouseDrag (const juce::MouseEvent& e)
{
    setFromMouse (e);
}

void WheelComponent::mouseUp (const juce::MouseEvent&)
{
    dragging = false;

    // A pitch wheel is spring-loaded; the mod wheel stays put.
    if (bipolar)
        startTimerHz (60);
}

void WheelComponent::timerCallback()
{
    value *= 0.75f;

    if (std::abs (value) < 0.01f)
    {
        value = 0.0f;
        stopTimer();
    }

    if (onValueChanged != nullptr)
        onValueChanged (value);

    repaint();
}

void WheelComponent::setValueFromMidi (float v)
{
    if (dragging)
        return; // the user's hand wins while they are moving it

    const float clamped = juce::jlimit (bipolar ? -1.0f : 0.0f, 1.0f, v);

    if (! juce::approximatelyEqual (clamped, value))
    {
        value = clamped;
        repaint();
    }
}
}
