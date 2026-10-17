#pragma once

#include "PresetStore.h"

class MiXXynthProcessor;

namespace mixxynth
{
    // What the bar shows, in one call: the editor polls this, and one call means one
    // fingerprint of the live patch rather than one per field.
    struct PresetStatus
    {
        juce::String name;      // the preset's name, "Init", or "(unsaved)"
        juce::String category;
        bool factory = false;   // the loaded preset cannot be overwritten
        bool dirty = false;     // the patch has been edited since it was loaded
        int index = -1;         // the loaded preset, or -1
    };

    // The preset session (M7). The store knows about files; this knows about the patch you
    // are working on: which preset it came from, whether it has been edited since, and the
    // moves between them. See docs/PLAN-M7.md.
    //
    // Message thread for the editing calls. The host's program calls land here too, which is
    // why the read-only accessors are safe with nothing loaded and why the store locks.
    class PresetManager
    {
    public:
        explicit PresetManager (MiXXynthProcessor&);

        PresetStore& store() { return bank; }
        const PresetStore& store() const { return bank; }

        // The loaded preset, or -1 when the patch is not one of the bank's - Init, or a patch
        // the host restored from a session.
        int currentIndex() const { return current; }
        juce::String currentCategory() const;

        // Not const, because reading the patch is not a const operation: the state copy
        // flushes every parameter into the tree first. See buildStateTree.
        juce::String currentName();

        // Everything the bar draws, in one pass over the live patch.
        PresetStatus status();

        // True when the patch differs from the one it was loaded from. A comparison rather
        // than a change flag, so a patch edited and then put back by hand is not dirty, and
        // a load is never mistaken for an edit.
        bool isDirty();

        // True when the patch is the init patch, byte for byte.
        bool isInitPatch();

        // Loads a bank preset. False when the index is not in the bank or the file cannot be
        // read, in which case nothing changes.
        bool load (int index);

        // Init: every parameter at its default, and the selection cleared, because Init is a
        // starting point rather than a preset in the bank.
        void loadInit();

        // Moves `delta` presets through the bank, wrapping. False when the bank is empty.
        bool step (int delta);

        // Writes the patch as a user preset, and selects it. Refuses an empty name, and a
        // name that belongs to a factory preset. Returns the new index, or -1.
        int saveAs (const juce::String& category, const juce::String& name);

        // Overwrites the loaded preset when it is one of the user's. False when nothing is
        // loaded or the loaded preset is a factory one, since neither can be overwritten.
        bool save();

        // Deletes a user preset. The patch is left alone, so the delete cannot take the
        // sound with it.
        bool remove (int index);

        // Remembers the patch so undo() can put it back. Called before a roll and before a
        // load, so "undo" means "put back what was there before that happened".
        //
        // A HISTORY, not a single slot. One slot was enough while the dice lived in the patch
        // bar, because the bar was the only thing that remembered; once the roll moved to the
        // toolbar, the slot could still hold a preset load from several moves ago - which is
        // exactly why undo appeared to land somewhere random rather than one step back.
        void rememberForUndo();

        bool canUndo() const { return ! undoStack.empty(); }
        bool canRedo() const { return ! redoStack.empty(); }

        bool undo();
        bool redo();

        // Re-reads the bank and keeps the selection when the preset still exists.
        void refresh();

        // Establishes the baseline the dirty marker compares against: the patch as it is right
        // now reads as clean. The processor calls this at the end of its constructor, because
        // a baseline taken before the default modulation modules exist would make every fresh
        // instance dirty.
        void resetBaseline();

        // The host's program list, which is this same bank rather than a second one.
        int numPrograms() const;
        juce::String programName (int index) const;
        int currentProgram() const;

    private:
        // Records the patch as the one at `index`, and the fingerprint it had at that moment.
        void markLoaded (int index);

        MiXXynthProcessor& proc;
        PresetStore bank;

        int current = -1;
        juce::int64 loadedPrint = 0;

        // One entry per move. The selection and the fingerprint travel with the tree, so an
        // undo puts back the whole session position: restoring the tree alone would leave a
        // hand-edited patch looking clean, because the fingerprint would be re-taken from the
        // restored patch.
        struct HistoryEntry
        {
            juce::ValueTree state;
            int index = -1;
            juce::int64 print = 0;
        };

        // Bounded, because each entry is a whole patch and an editor is not the place to grow
        // without limit. Deep enough that nobody reaches the end of it by accident.
        static constexpr int kMaxHistory = 64;

        std::vector<HistoryEntry> undoStack;
        std::vector<HistoryEntry> redoStack;
    };
}
