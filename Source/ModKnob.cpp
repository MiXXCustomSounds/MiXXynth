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
        b.removeFromTop (14);

    knob.setBounds (b);
}

void ModKnob::paint (juce::Graphics& g)
{
    if (labelVisible)
    {
        g.setColour (juce::Colour (0xffb9c2cc));
        g.setFont (11.0f);
        g.drawFittedText (text, getLocalBounds().removeFromTop (14), juce::Justification::centred, 1);
    }

    if (dragOver)
    {
        g.setColour (juce::Colour (0xff35e0ff));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 1.5f);
    }

    // Modulation depth ring.
    if (std::abs (modDepth) > 0.001f)
    {
        auto area = getLocalBounds();

        if (labelVisible)
            area.removeFromTop (14);

        const auto centre = area.toFloat().getCentre();
        const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 2.0f;
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
    repaint();

    const int srcIndex = static_cast<int> (details.description.toString().getIntValue());

    if (onSourceDropped != nullptr)
        onSourceDropped (srcIndex, dest);
}

void ModKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onClearRequested != nullptr)
        onClearRequested (dest);
    else
        Component::mouseDown (e);
}
}
