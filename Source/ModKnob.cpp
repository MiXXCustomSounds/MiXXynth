#include "ModKnob.h"

#include "EditorTheme.h"
#include "ModMatrix.h"

namespace mixxynth::ui
{
namespace
{
    // Amount presets offered by the right-click menu, for people who want an
    // exact value rather than a drag.
    const float amountPresets[] = { 1.0f, 0.75f, 0.5f, 0.25f, -0.25f, -0.5f, -0.75f, -1.0f };

    juce::String formatDepth (float d)
    {
        return (d >= 0.0f ? "+" : "") + juce::String (d, 2);
    }

    // Knob convention: value 0 sits at -135 degrees and 1 at +135, measured
    // clockwise from twelve o'clock, which is how addCentredArc reads angles and
    // what the sst dial uses for its own indicator.
    float angleForValue (float x)
    {
        return (juce::jlimit (0.0f, 1.0f, x) - 0.5f) * 1.5f * juce::MathConstants<float>::pi;
    }
}

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
    // instead of forwarding them, so the menu has to be hooked here rather than
    // in our own mouseDown override.
    knob.onPopupMenu = [this] (const juce::ModifierKeys&) { showMenu(); };

    // Listen to the parameter as well as the knob widget. The widget repaints
    // itself, which covers the dial, but the modulation arcs live outside it and
    // are drawn here - so without this the arcs sit at whatever value the knob had
    // when they were last painted.
    source.addGUIDataListener (this);
}

ModKnob::~ModKnob()
{
    // Must happen before `source` is destroyed: its destructor walks the listener
    // list calling sourceVanished, and by then this object is gone.
    source.removeGUIDataListener (this);
}

void ModKnob::refreshFromParam()
{
    // The dial is a child; the arcs are ours. Repaint both.
    knob.repaint();
    repaint();
}

void ModKnob::setModDepth (float d)
{
    if (! juce::approximatelyEqual (d, modDepth))
    {
        modDepth = d;
        repaint();
    }
}

void ModKnob::setModAssignments (std::vector<Assignment> a)
{
    float total = 0.0f;

    for (const auto& x : a)
        total += x.depth;

    const bool countChanged = a.size() != assignments.size();
    const bool depthChanged = ! juce::approximatelyEqual (total, modDepth);

    assignments = std::move (a);
    modDepth = juce::jlimit (-1.0f, 1.0f, total);

    if (countChanged || depthChanged)
        repaint();
}

// The dial is circular, so it needs a square area. Handing it a wide, short rect
// (as the page layouts naturally produce) makes the dial overflow and get
// clipped, so centre a square inside the cell instead. The square is inset by the
// ring width on every side to leave the modulation ring outside the dial, where
// it is visible and can be dragged. That inset scales with the cell, so a small
// knob keeps a usable dial instead of losing 26 px to a fixed ring.
juce::Rectangle<float> ModKnob::dialArea() const
{
    auto b = getLocalBounds();

    if (labelVisible)
        b.removeFromTop (labelHeight);

    const int shortSide = juce::jmin (b.getWidth(), b.getHeight());
    const int size = juce::jmax (12, shortSide - 2 * ringWidthFor (shortSide));
    return b.withSizeKeepingCentre (size, size).toFloat();
}

float ModKnob::value01() const
{
    return source.p != nullptr ? juce::jlimit (0.0f, 1.0f, source.p->getValue()) : 0.0f;
}

std::pair<float, float> ModKnob::arcAnglesForTest (int index) const
{
    if (index < 0 || index >= static_cast<int> (assignments.size()))
        return { 0.0f, 0.0f };

    const float v = value01();
    const float depth = assignments[static_cast<size_t> (index)].depth;

    return { angleForValue (v), angleForValue (v + depth) };
}

// Arcs stack outwards from the dial edge so several sources on one knob stay
// individually readable and individually grabbable.
float ModKnob::arcRadiusFor (int index) const
{
    const float baseRadius = dialArea().getWidth() * 0.5f;

    return baseRadius + 3.5f + static_cast<float> (juce::jmax (0, index)) * 3.5f;
}

int ModKnob::ringIndexAt (juce::Point<int> pos) const
{
    if (assignments.empty())
        return -1;

    const auto sq = dialArea();
    const auto centre = sq.getCentre();
    const float d = pos.toFloat().getDistanceFrom (centre);

    // The band has to be generous: a 2 px target is not a target. Each arc owns a
    // slot of the ring band and the nearest one wins, so overlapping bands resolve
    // to the arc the pointer is actually closest to.
    const int shown = juce::jmin (static_cast<int> (assignments.size()), 3);

    int best = -1;
    float bestDistance = std::numeric_limits<float>::max();

    for (int i = 0; i < shown; ++i)
    {
        const float r = arcRadiusFor (i);
        const float distance = std::abs (d - r);

        // Half the arc spacing, plus a little, so there are no dead gaps between
        // adjacent arcs and the innermost one is reachable from the dial edge.
        if (distance <= 3.5f && distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

void ModKnob::resized()
{
    knob.setBounds (dialArea().toNearestInt());
}

void ModKnob::paint (juce::Graphics& g)
{
    if (labelVisible)
    {
        // While dragging the ring the label strip doubles as the value readout,
        // so the amount is visible without a tooltip.
        const bool showValue = draggingRing;
        const auto colour = ringDragDepth >= 0.0f ? juce::Colour (0xff4ade80)
                                                  : juce::Colour (0xfff87171);

        g.setColour (showValue ? colour : juce::Colour (0xffb9c2cc));
        g.setFont (showValue ? 12.0f : 11.0f);
        g.drawFittedText (showValue ? formatDepth (ringDragDepth) : text,
                          getLocalBounds().removeFromTop (labelHeight),
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

    if (assignments.empty())
        return;

    // One arc per assigned source, each drawn FROM THE KNOB'S CURRENT VALUE outward
    // to value + depth. That is the Serum and Vital convention, and it is what makes
    // the ring readable at a glance: you can see where the knob sits and how far the
    // modulation will push it, rather than having to infer both.
    const auto sq = dialArea();
    const auto centre = sq.getCentre();
    const float v = value01();
    const int shown = juce::jmin (static_cast<int> (assignments.size()), 3);

    for (int i = 0; i < shown; ++i)
    {
        const auto& a = assignments[static_cast<size_t> (i)];
        const bool isDragged = draggingRing && ringDragIndex == i;
        const float depth = isDragged ? ringDragDepth : a.depth;

        const float from = angleForValue (v);
        const float to = angleForValue (v + depth);
        const float radius = arcRadiusFor (i);

        juce::Path p;
        p.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, from, to, true);

        if (isDragged || overRingIndex == i)
            g.setColour (depth >= 0.0f ? juce::Colour (0xff4ade80) : juce::Colour (0xfff87171));
        else
            g.setColour (i == 0 ? juce::Colour (0xff3f9e63) : juce::Colour (0xff7c6bc4));

        g.strokePath (p, juce::PathStrokeType (isDragged || overRingIndex == i ? 3.6f : 2.4f,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // A hint that there are more sources than fit in the ring band.
    if (static_cast<int> (assignments.size()) > shown)
    {
        g.setColour (juce::Colour (0xffa78bfa));
        g.setFont (9.0f);
        g.drawText ("+" + juce::String (static_cast<int> (assignments.size()) - shown),
                    getLocalBounds().removeFromBottom (11),
                    juce::Justification::centred);
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

void ModKnob::mouseMove (const juce::MouseEvent& e)
{
    const int index = ringIndexAt (e.getPosition());

    if (index != overRingIndex)
    {
        overRingIndex = index;
        setMouseCursor (index >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                   : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ModKnob::mouseExit (const juce::MouseEvent&)
{
    if (overRingIndex >= 0)
    {
        overRingIndex = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ModKnob::mouseDown (const juce::MouseEvent& e)
{
    // Only reached outside the dial square, since the sst knob child covers the
    // dial itself. That leaves the ring band plus the cell's corners.
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }

    const int index = ringIndexAt (e.getPosition());

    if (index >= 0 && index < static_cast<int> (assignments.size()))
    {
        // Drag whichever arc was grabbed, not always the first one. A knob with two
        // sources previously had one draggable arc and one that could only be
        // reached through the right-click menu.
        const auto& a = assignments[static_cast<size_t> (index)];

        draggingRing = true;
        ringDragIndex = index;
        ringDragSrc = a.source;
        ringDragStartDepth = a.depth;
        ringDragDepth = ringDragStartDepth;
        ringDragStartY = e.position.y;
        repaint();
        return;
    }

    Component::mouseDown (e);
}

void ModKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingRing)
        return;

    // Vertical drag: 110 px of travel covers the full -1..+1 range.
    const float delta = (ringDragStartY - e.position.y) / 110.0f;
    float depth = juce::jlimit (-1.0f, 1.0f, ringDragStartDepth + delta);

    // Snap to exactly zero, so "no modulation" is easy to land on.
    if (std::abs (depth) < 0.02f)
        depth = 0.0f;

    if (juce::approximatelyEqual (depth, ringDragDepth))
        return;

    ringDragDepth = depth;

    if (onModDepthChanged != nullptr)
        onModDepthChanged (ringDragSrc, dest, depth);

    if (onStatus != nullptr)
    {
        const int src = juce::jlimit (0, mixxynth::kNumModSources - 1, ringDragSrc);
        onStatus (text + ": " + mixxynth::modSourceNames()[src]
                    + " amount " + formatDepth (depth));
    }

    repaint();
}

void ModKnob::mouseUp (const juce::MouseEvent&)
{
    if (draggingRing)
    {
        draggingRing = false;
        repaint();
    }
}

void ModKnob::showMenu()
{
    if (! isModTarget())
    {
        // Only complain if this knob is part of the modulation UI at all; the
        // slot-depth knobs have no destination and no clear handler.
        if (onClearRequested != nullptr)
            flashRejected();

        return;
    }

    juce::PopupMenu menu;

    if (assignments.empty())
    {
        menu.addSectionHeader ("No modulation");
        menu.addItem (1, "Drag a source chip onto this knob", false, false);
    }
    else
    {
        menu.addSectionHeader ("Modulation (" + juce::String (assignments.size()) + ")");

        for (size_t i = 0; i < assignments.size(); ++i)
        {
            const int src = juce::jlimit (0, mixxynth::kNumModSources - 1, assignments[i].source);
            menu.addItem (100 + static_cast<int> (i),
                          juce::String (mixxynth::modSourceNames()[src])
                            + "   " + formatDepth (assignments[i].depth));
        }

        menu.addSeparator();

        juce::PopupMenu amounts;

        for (int i = 0; i < numAmountPresets; ++i)
            amounts.addItem (200 + i, formatDepth (amountPresets[i]));

        menu.addSubMenu ("Set amount (first source)", amounts);
        menu.addSeparator();
        menu.addItem (2, "Clear all modulation");
    }

    juce::Component::SafePointer<ModKnob> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe] (int result)
                        {
                            if (safe != nullptr)
                                safe->handleMenuResult (result);
                        });
}

void ModKnob::handleMenuResult (int result)
{
    if (result == 0)
        return;

    if (result == 2)
    {
        if (onClearRequested != nullptr)
            onClearRequested (dest);

        return;
    }

    if (result >= 200 && result < 200 + numAmountPresets)
    {
        if (! assignments.empty() && onModDepthChanged != nullptr)
            onModDepthChanged (assignments.front().source, dest, amountPresets[result - 200]);

        return;
    }

    if (result >= 100)
    {
        const int idx = result - 100;

        if (idx >= 0 && idx < static_cast<int> (assignments.size()) && onModRemoveRequested != nullptr)
            onModRemoveRequested (assignments[static_cast<size_t> (idx)].source, dest);
    }
}
}
