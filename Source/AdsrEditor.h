#pragma once

#include <functional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ModShape.h"

namespace mixxynth::ui
{
// M6: the operator envelope, drawn and edited the way the modulation envelopes are.
//
// The four stages are four parameters - attack, decay, sustain and release in
// seconds - rather than a breakpoint shape, so this is not ShapeEditorComponent.
// It does share the drawing: it builds the equivalent ModShape and renders it with
// the same buildShapePath the LFO section uses, so the two cannot drift apart
// visually. The colours and grid are the ENV accent from that section.
//
// The time axis is a fixed window spanning the three parameter maxima, mapped
// through a power curve. A linear axis is unusable here: the init patch has every
// stage at zero and an 8 second release is four thousand times a 2 ms attack, so a
// linear window hides either the short end or the long end. The power curve keeps
// both on screen and is exactly invertible, so a drag round-trips to the value it
// started from.
class AdsrEditor : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    // The three draggable handles, in time order. The origin is fixed at (0, 0).
    enum Handle { attackPeak = 0, decayCorner, releaseEnd, numHandles };

    AdsrEditor (juce::RangedAudioParameter* attackParam,
                juce::RangedAudioParameter* decayParam,
                juce::RangedAudioParameter* sustainParam,
                juce::RangedAudioParameter* releaseParam);
    ~AdsrEditor() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // The envelope those four parameters describe, as a shape, with a trailing
    // point at zero so the curve reads to the right edge. This is also what the
    // drawing is built from, which is why it goes through the shared path builder.
    mixxynth::ModShape currentShape() const;

    void refreshFromParams();

    // True when the four parameters still describe the same envelope the last
    // repaint drew, so the page's poll can skip work instead of repainting six
    // envelopes thirty times a second.
    bool repaintIfChanged();

    // A nominal hold for the sustain plateau. The sustain has no duration of its own
    // - it lasts as long as the note does - but an envelope that decays straight into
    // its release does not read as an ADSR at all: the sustain level would be a
    // single point rather than a level. So the display gives the plateau a fixed
    // half second. It is a drawing convention, not a parameter.
    static constexpr float holdSeconds = 0.5f;

    // The whole axis, in seconds: the three parameter maxima plus that hold.
    static constexpr float windowSeconds = 14.5f;
    // A power curve rather than a straight line. See the note above.
    static constexpr float axisPower = 0.4f;

    static float timeToX (float seconds);
    static float xToTime (float x01);

    // Where a handle sits, in 0..1 plot space. Exposed so the audit can check the
    // geometry instead of trusting a screenshot.
    juce::Point<float> handleForTest (int handle) const;

    // Applies a handle drag. The mouse handlers call this and so does the audit.
    void dragHandleForTest (int handle, juce::Point<float> to);

    float attackSeconds() const;
    float decaySeconds() const;
    float sustainLevel() const;
    float releaseSeconds() const;

    std::function<void()> onChange;
    std::function<void (const juce::String&)> onStatus;

private:
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> toPixel (juce::Point<float> plot) const;
    int handleAt (juce::Point<float> pixel) const;
    void setStage (int handle, float x01, float y01);
    void writeParam (int index, float seconds);
    float readParam (int index) const;

    // attack, decay, sustain, release. Sustain's range is already 0..1 so it needs
    // no conversion, unlike the three times.
    juce::RangedAudioParameter* params[4] { nullptr, nullptr, nullptr, nullptr };

    int dragHandle = -1;
    int hoveredHandle = -1;

    // What the last repaint drew, so a poll that changes nothing costs nothing.
    float lastPainted[4] { -1.0f, -1.0f, -1.0f, -1.0f };

    static constexpr float grabRadius = 9.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdsrEditor)
};
}
