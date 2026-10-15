#include "ModKnob.h"

#include "EditorTheme.h"

namespace mixxynth::ui
{
ModKnob::ModKnob (const juce::String& labelText,
                  juce::RangedAudioParameter* param,
                  int destIndex,
                  bool showLabel)
    : source (param), text (labelText), dest (destIndex), labelVisible (showLabel)
{
    initialiseWidgetStyles();

    knob.setSource (&source);
    style (knob);
    addAndMakeVisible (knob);

    // sst-jucegui's mouseDown consumes right-clicks and calls onPopupMenu
    // instead of forwarding them, so the clear gesture has to be hooked here
    // rather than in our own mouseDown override.
    knob.onPopupMenu = [this] (const juce::ModifierKeys&)
    {
        if (onClearRequested != nullptr && isModTarget())
            onClearRequested (dest);
    };
}

ModKnob::~ModKnob() = default;

void ModKnob::refreshFromParam()
{
    knob.repaint();
}

void ModKnob::setModDepth (float d)
{
    if (! juce::approximatelyEqual (d, modDepth))
    {
        modDepth = d;
        repaint();
    }
}

void ModKnob::resized()
{
    auto b = getLocalBounds();

    if (labelVisible)
        b.removeFromTop (labelHeight);

    // The dial is circular, so it needs a square area. Handing it a wide, short
    // rect (as the page layouts naturally produce) makes the dial overflow the
    // bottom and get clipped, so centre a square inside the cell instead.
    const int size = juce::jmax (16, juce::jmin (b.getWidth(), b.getHeight()));
    knob.setBounds (b.withSizeKeepingCentre (size, size));
}

void ModKnob::paint (juce::Graphics& g)
{
    if (labelVisible)
    {
        g.setColour (juce::Colour (0xffb9c2cc));
        g.setFont (11.0f);
        g.drawFittedText (text, getLocalBounds().removeFromTop (labelHeight),
                          juce::Justification::centred, 1);
    }

    if (rejectFlash)
    {
        g.setColour (juce::Colour (0xffef4444));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.5f);
    }
    else if (dragOver)
    {
        // Green when the knob can be modulated, red when the drop would be
        // rejected, so the gesture is legible before releasing the mouse.
        g.setColour (isModTarget() ? juce::Colour (0xff4ade80) : juce::Colour (0xffef4444));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 1.5f);
    }

    // Modulation depth ring, drawn around the same square the dial uses.
    if (std::abs (modDepth) > 0.001f)
    {
        auto area = getLocalBounds();

        if (labelVisible)
            area.removeFromTop (labelHeight);

        const int size = juce::jmax (16, juce::jmin (area.getWidth(), area.getHeight()));
        const auto square = area.withSizeKeepingCentre (size, size).toFloat();
        const auto centre = square.getCentre();
        const float radius = size * 0.5f - 1.0f;
        const float sweep = juce::jlimit (-1.0f, 1.0f, modDepth) * juce::MathConstants<float>::pi;

        juce::Path p;
        p.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                         -juce::MathConstants<float>::halfPi,
                         -juce::MathConstants<float>::halfPi + sweep,
                         true);

        g.setColour (modDepth >= 0.0f ? juce::Colour (0xff4ade80) : juce::Colour (0xfff87171));
        g.strokePath (p, juce::PathStrokeType (2.5f));
    }
}

void ModKnob::itemDragEnter (const SourceDetails&)
{
    dragOver = true;
    repaint();
}

void ModKnob::itemDragExit (const SourceDetails&)
{
    dragOver = false;
    repaint();
}

void ModKnob::itemDropped (const SourceDetails& details)
{
    dragOver = false;

    const int srcIndex = static_cast<int> (details.description.toString().getIntValue());

    if (! isModTarget())
    {
        // Not a modulation destination: make the rejection visible rather than
        // silently swallowing the drop.
        flashRejected();
        return;
    }

    repaint();

    if (onSourceDropped != nullptr)
        onSourceDropped (srcIndex, dest);
}

void ModKnob::flashRejected()
{
    rejectFlash = true;
    repaint();
    startTimer (450);
}

void ModKnob::timerCallback()
{
    stopTimer();
    rejectFlash = false;
    repaint();
}

void ModKnob::mouseDown (const juce::MouseEvent& e)
{
    // Only reached for clicks outside the dial square, since the sst knob child
    // covers the dial itself.
    if (e.mods.isPopupMenu() && onClearRequested != nullptr && isModTarget())
        onClearRequested (dest);
    else
        Component::mouseDown (e);
}
}
