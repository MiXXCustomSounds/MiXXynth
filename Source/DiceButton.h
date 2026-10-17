#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace mixxynth::ui
{
    // A square button showing a die face. The word "Dice" told you what the button did; the die
    // says the same thing in the space a bar has to spare.
    //
    // It lives in the patch bar rather than on a page, next to the five section locks, because
    // the two are one gesture: the locks decide what a roll leaves alone, and a roll button
    // somewhere else would leave the locks with nothing to explain. The tooltip says "unlocked"
    // rather than "every control" for the same reason.
    class DiceButton : public juce::Button
    {
    public:
        DiceButton() : juce::Button ("Dice")
        {
            setTooltip ("Roll a new patch from the unlocked sections");
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced (1.0f);
            const bool hot = over || down;

            g.setColour (hot ? juce::Colour (0xff232b34) : juce::Colour (0xff161c22));
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (hot ? juce::Colour (0xffffa53d) : juce::Colour (0xff39434f));
            g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.5f);

            // Six pips in two columns of three.
            const auto c = b.getCentre();
            const float s = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
            const float r = juce::jmax (1.6f, s * 0.12f);
            const float dx = s * 0.23f, dy = s * 0.27f;

            g.setColour (hot ? juce::Colour (0xffffa53d) : juce::Colour (0xffc2ccd6));

            for (int col = 0; col < 2; ++col)
                for (int row = 0; row < 3; ++row)
                    g.fillEllipse (c.x + (col == 0 ? -dx : dx) - r,
                                   c.y + (static_cast<float> (row) - 1.0f) * dy - r,
                                   r * 2.0f, r * 2.0f);
        }
    };
}
