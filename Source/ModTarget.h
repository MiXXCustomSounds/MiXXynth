#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ParamSources.h"

namespace mixxynth::ui
{
// M6: the shared half of every modulation destination control.
//
// ModKnob and SquareCell both let you drop a modulation source on them, both
// offer the same right-click menu, and both obey the same reject rule and the
// same depth-drag feel. That logic used to live entirely inside ModKnob, which
// was fine while ModKnob was the only control that could be modulated. It is not
// any more, and a second copy of it is how a knob and a cell end up disagreeing
// about what a drop means.
//
// So the behaviour lives here, once, and the subclasses supply only what is
// genuinely different: how they are drawn, and what their parameter is.
//
// Message thread only, like every other widget in this file.
class ModTarget : private juce::Timer
{
public:
    // One modulation source aimed at this control, with its signed depth.
    struct Assignment
    {
        int source = 0;
        float depth = 0.0f;
    };

    explicit ModTarget (int destIndex) : dest (destIndex) {}
    ~ModTarget() override = default;

    // ---- supplied by the subclass ------------------------------------------
    // The parameter this control drives, or nullptr if it only displays.
    virtual juce::RangedAudioParameter* getParam() const = 0;

    // Used in menus and status messages.
    virtual juce::String getLabelText() const = 0;

    // Repaint hook, so this class can change what is drawn without knowing how.
    virtual void repaintForMod() = 0;

    // Re-read the parameter and repaint. The editor calls this after a preset load
    // or a dice roll, so a control shows the value that is actually there rather
    // than the one it last painted.
    virtual void refreshFromParam() = 0;

    // ---- shared state ------------------------------------------------------
    int getDestIndex() const noexcept { return dest; }

    // A control whose dest is dstOff cannot be a modulation target, so a drop on
    // it is refused. The refusal is visible rather than silent.
    bool isModTarget() const noexcept { return dest != 0; }

    float getModDepth() const noexcept { return modDepth; }
    int getNumAssignments() const noexcept { return static_cast<int> (assignments.size()); }
    const std::vector<Assignment>& getAssignments() const noexcept { return assignments; }

    void setModDepth (float d);
    void setModAssignments (std::vector<Assignment> a);

    // ---- drag and drop -----------------------------------------------------
    // Called from the subclass's DragAndDropTarget overrides, so the accept
    // rule, the highlight and the reject flash are shared.
    void modDragEnter();
    void modDragExit();
    void modDragDropped (int sourceIndex);

    bool isDragOver() const noexcept { return dragOver; }
    bool isRejectFlashing() const noexcept { return rejectFlash; }

    // ---- right-click menu --------------------------------------------------
    void showModMenu (juce::Component* target);
    void handleModMenuResult (int result);

    // ---- depth dragging ----------------------------------------------------
    // Vertical drag: 110 px of travel covers the full -1..+1 range, and a result
    // within 0.02 of zero snaps to exactly zero so "no modulation" is reachable.
    static float depthFromDrag (float startDepth, float startY, float currentY);

    std::function<void (int srcIndex, int destIndex)> onSourceDropped;
    std::function<void (int destIndex)> onClearRequested;
    std::function<void (int srcIndex, int destIndex, float depth)> onModDepthChanged;
    std::function<void (int srcIndex, int destIndex)> onModRemoveRequested;
    std::function<void (const juce::String&)> onStatus;

protected:
    // Signed depth of one assignment, with an in-progress drag substituted.
    float effectiveDepth (int index, bool dragging, float dragDepth) const;

    void flashRejected();

    // Total of every assignment, clamped, which is what a ring or a band shows.
    static float totalDepth (const std::vector<Assignment>& a);

private:
    void timerCallback() override;

    int dest = 0;
    float modDepth = 0.0f;
    std::vector<Assignment> assignments;

    bool dragOver = false;
    bool rejectFlash = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModTarget)
};

// The amount presets the menu offers, for people who want an exact value rather
// than a drag. Shared so the menu reads identically everywhere.
const float* modAmountPresets();
int numModAmountPresets();

// "+0.50" / "-0.25", the same string in a menu, a status line and a cell.
juce::String formatModDepth (float d);
}
