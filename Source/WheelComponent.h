#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace mixxynth::ui
{
    // Hand-rolled pitch / mod wheel, since neither JUCE nor sst-jucegui ships one.
    // A vertical wheel with a grip line:
    //   - bipolar (pitch): centred, springs back to centre on release
    //   - unipolar (mod):  bottom-anchored, stays where it is left
    // Drawing is deliberately plain for now; the black-metal pass comes later.
    class WheelComponent : public juce::Component,
                           private juce::Timer
    {
    public:
        WheelComponent (const juce::String& labelText, bool isBipolar);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        float getValue() const { return value; }

        // Updates the wheel from incoming MIDI. Never calls onValueChanged.
        void setValueFromMidi (float v);

        std::function<void (float)> onValueChanged;

    private:
        void timerCallback() override;
        void setFromMouse (const juce::MouseEvent&);
        juce::Rectangle<float> wheelBounds() const;
        float valueForY (float y) const;

        juce::String text;
        bool bipolar = true;
        float value = 0.0f;
        bool dragging = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WheelComponent)
    };
}
