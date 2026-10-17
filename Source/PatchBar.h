#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DiceButton.h"
#include "PresetManager.h"
#include "Randomizer.h"

class MiXXynthProcessor;

namespace mixxynth::ui
{
    // The patch bar (M7): the preset browser and the roll, side by side.
    //
    // They share a bar because they are the same kind of control - both act on the WHOLE patch
    // rather than on one parameter. A preset is a patch you keep; a roll is a patch you
    // generate. And the locks have to be visible next to the dice, or "why did that not
    // change?" is a question the interface refuses to answer.
    //
    // It polls rather than listening: the dirty marker is a fingerprint of the whole patch, and
    // the parameters it watches are written by the host, by automation and by the editor's own
    // controls. A poll at a few hertz is both cheaper and more honest than trying to catch every
    // route a change can arrive by.
    class PatchBar : public juce::Component,
                     private juce::Timer
    {
    public:
        explicit PatchBar (MiXXynthProcessor&);
        ~PatchBar() override;

        void paint (juce::Graphics&) override;
        void resized() override;

        // Re-reads the bank and the session. Called by the timer, and by the audit.
        void refresh();

        // The moves, exposed so the audit can drive them without a mouse. Each is what its
        // button does, so a check on one is a check on the other.
        bool loadSelected();
        bool step (int delta);
        bool loadInit();
        int saveAs (const juce::String& category, const juce::String& name);
        bool removeSelected();
        void rollNow();
        bool undoRoll();
        bool redoRoll();

        // What the bar is showing, for the audit.
        juce::String debug() const;

        // The number of items in the browser, including its section headings.
        int numBrowserItems() const { return browser.getNumItems(); }

        // The item id the browser has selected, so the audit can check it follows a load.
        int selectedItemId() const { return browser.getSelectedId(); }

        // For the audit: the preset readout's box and the height of one line of its text.
        // "The text is not clipped" is a claim about the layout, and the only way to check it
        // without a screenshot is to compare the two numbers.
        int statusHeightForTest() const { return status.getHeight(); }
        int statusLineHeightForTest() const
        {
            return juce::roundToInt (status.getFont().getHeight());
        }

    private:
        void timerCallback() override;
        void rebuildList();
        void showSaveDialog();
        void setMessage (const juce::String&);

        MiXXynthProcessor& proc;

        juce::ComboBox browser;
        juce::TextButton prevButton, nextButton, initButton, saveButton, deleteButton;
        juce::TextButton undoButton, redoButton;

        // The roll, and the five section locks beside it. This is the arrangement the class
        // comment above describes, and the one the dice briefly left for the FM page toolbar -
        // where it rolled FM parameters directly and ignored the locks entirely, so the locks
        // controlled nothing you could reach.
        DiceButton diceButton;
        juce::OwnedArray<juce::TextButton> lockButtons;
        juce::Label status;
        juce::Label message;

        // The item list the browser was built from, so a refresh only rebuilds when the bank
        // actually changed. Rebuilding a combo every poll would close a popup the user has open.
        int listedSize = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchBar)
    };
}
