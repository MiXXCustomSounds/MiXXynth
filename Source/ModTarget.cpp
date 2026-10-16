#include "ModTarget.h"

#include <cmath>

#include "ModMatrix.h"

namespace mixxynth::ui
{
namespace
{
    // Amount presets offered by the right-click menu, for people who want an
    // exact value rather than a drag.
    constexpr int kNumAmountPresets = 8;
    const float amountPresets[kNumAmountPresets] = { 1.0f, 0.75f, 0.5f, 0.25f, -0.25f, -0.5f, -0.75f, -1.0f };
}

const float* modAmountPresets()
{
    return amountPresets;
}

int numModAmountPresets()
{
    return kNumAmountPresets;
}

juce::String formatModDepth (float d)
{
    return (d >= 0.0f ? "+" : "") + juce::String (d, 2);
}

float ModTarget::totalDepth (const std::vector<Assignment>& a)
{
    float total = 0.0f;

    for (const auto& x : a)
        total += x.depth;

    return juce::jlimit (-1.0f, 1.0f, total);
}

void ModTarget::setModDepth (float d)
{
    if (! juce::approximatelyEqual (d, modDepth))
    {
        modDepth = d;
        repaintForMod();
    }
}

void ModTarget::setModAssignments (std::vector<Assignment> a)
{
    const bool countChanged = a.size() != assignments.size();
    const bool depthChanged = ! juce::approximatelyEqual (totalDepth (a), modDepth);

    assignments = std::move (a);
    modDepth = totalDepth (assignments);

    if (countChanged || depthChanged)
        repaintForMod();
}

float ModTarget::effectiveDepth (int index, bool dragging, float dragDepth) const
{
    if (index < 0 || index >= static_cast<int> (assignments.size()))
        return 0.0f;

    return dragging ? dragDepth : assignments[static_cast<size_t> (index)].depth;
}

float ModTarget::depthFromDrag (float startDepth, float startY, float currentY)
{
    // Vertical drag: 110 px of travel covers the full -1..+1 range.
    const float delta = (startY - currentY) / 110.0f;
    float depth = juce::jlimit (-1.0f, 1.0f, startDepth + delta);

    // Snap to exactly zero, so "no modulation" is easy to land on.
    if (std::abs (depth) < 0.02f)
        depth = 0.0f;

    return depth;
}

void ModTarget::modDragEnter()
{
    dragOver = true;
    repaintForMod();
}

void ModTarget::modDragExit()
{
    dragOver = false;
    repaintForMod();
}

void ModTarget::modDragDropped (int sourceIndex)
{
    dragOver = false;

    if (! isModTarget())
    {
        // Not a modulation destination: make the rejection visible rather than
        // silently swallowing the drop.
        flashRejected();
        return;
    }

    repaintForMod();

    if (onSourceDropped != nullptr)
        onSourceDropped (sourceIndex, dest);
}

void ModTarget::flashRejected()
{
    rejectFlash = true;
    repaintForMod();
    startTimer (450);
}

void ModTarget::timerCallback()
{
    stopTimer();
    rejectFlash = false;
    repaintForMod();
}

void ModTarget::showModMenu (juce::Component* target)
{
    if (target == nullptr)
        return;

    if (! isModTarget())
    {
        // Only complain if this control is part of the modulation UI at all; the
        // slot-depth knobs have no destination and no clear handler.
        if (onClearRequested != nullptr)
            flashRejected();

        return;
    }

    juce::PopupMenu menu;

    if (assignments.empty())
    {
        menu.addSectionHeader ("No modulation");
        menu.addItem (1, "Drag a source chip onto this control", false, false);
    }
    else
    {
        menu.addSectionHeader ("Modulation (" + juce::String (assignments.size()) + ")");

        for (size_t i = 0; i < assignments.size(); ++i)
        {
            const int src = juce::jlimit (0, mixxynth::kNumModSources - 1, assignments[i].source);
            menu.addItem (100 + static_cast<int> (i),
                          juce::String (mixxynth::modSourceNames()[src])
                            + "   " + formatModDepth (assignments[i].depth));
        }

        menu.addSeparator();

        juce::PopupMenu amounts;

        for (int i = 0; i < kNumAmountPresets; ++i)
            amounts.addItem (200 + i, formatModDepth (amountPresets[i]));

        menu.addSubMenu ("Set amount (first source)", amounts);
        menu.addSeparator();
        menu.addItem (2, "Clear all modulation");
    }

    // `target` is the subclass itself - the same object as this base - so its
    // liveness is this object's liveness, which is what makes capturing `this`
    // below safe.
    juce::Component::SafePointer<juce::Component> safe (target);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                        [this, safe] (int result)
                        {
                            if (safe != nullptr)
                                handleModMenuResult (result);
                        });
}

void ModTarget::handleModMenuResult (int result)
{
    if (result == 0)
        return;

    if (result == 2)
    {
        if (onClearRequested != nullptr)
            onClearRequested (dest);

        return;
    }

    if (result >= 200 && result < 200 + kNumAmountPresets)
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
