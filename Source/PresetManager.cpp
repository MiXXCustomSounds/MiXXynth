#include "PresetManager.h"

#include "PluginProcessor.h"

namespace mixxynth
{
    PresetManager::PresetManager (MiXXynthProcessor& p)
        : proc (p)
    {
        // The bank is listed here, but the dirty baseline is NOT taken here: this runs before
        // the processor's constructor body has set up the default modulation modules, and a
        // fingerprint taken now would be a fingerprint of a patch that never existed. The
        // processor calls resetBaseline() at the end of its own constructor.
        bank.refresh();
    }

    void PresetManager::resetBaseline()
    {
        // The fingerprint only. It deliberately does NOT clear the selection: the baseline is
        // "the patch as it is right now reads clean", which is a statement about the patch, not
        // about which preset it came from. Clearing the selection here would make a loaded
        // preset report itself as "(unsaved)".
        loadedPrint = fingerprint (proc.buildStateTree());
    }

    juce::String PresetManager::currentName()
    {
        return status().name;
    }

    juce::String PresetManager::currentCategory() const
    {
        return current >= 0 ? bank.categoryOf (current) : juce::String();
    }

    bool PresetManager::isInitPatch()
    {
        return fingerprint (proc.buildStateTree()) == fingerprint (proc.buildInitState());
    }

    PresetStatus PresetManager::status()
    {
        PresetStatus result;

        result.index = current;
        result.dirty = isDirty();

        if (current >= 0)
        {
            result.name = bank.nameOf (current);
            result.category = bank.categoryOf (current);
            result.factory = bank.isFactory (current);

            return result;
        }

        // Not a bank preset: either the init patch, or a patch that has no name any more -
        // one the host restored, or one whose preset was deleted. Saying "Init" for the
        // second of those would be a lie the bar tells on every frame.
        result.name = isInitPatch() ? juce::String ("Init") : juce::String ("(unsaved)");

        return result;
    }

    bool PresetManager::isDirty()
    {
        return fingerprint (proc.buildStateTree()) != loadedPrint;
    }

    void PresetManager::markLoaded (int index)
    {
        current = index;

        // Taken from the LIVE state rather than from the file, so the two are the same
        // question asked of the same tree. applyStateTree forces the algorithm to Free on
        // the way in, and a fingerprint of the file would disagree with the patch the moment
        // a preset named one of the DX7 algorithms.
        loadedPrint = fingerprint (proc.buildStateTree());
    }

    bool PresetManager::load (int index)
    {
        auto preset = bank.read (index);

        if (! preset.isValid())
            return false;

        rememberForUndo();

        // A preset is partial, so it is laid over the init patch: every parameter it does not
        // mention keeps its default rather than whatever the last patch happened to leave
        // behind. That is what makes a load deterministic.
        if (! proc.applyStateTree (overlayState (proc.buildInitState(), preset)))
            return false;

        markLoaded (index);

        return true;
    }

    void PresetManager::loadInit()
    {
        rememberForUndo();

        proc.applyInitState();

        // -1 rather than an index: Init is not in the bank, and pretending it is would put a
        // name in the bar that Save could then try to overwrite.
        markLoaded (-1);
    }

    bool PresetManager::step (int delta)
    {
        const int count = bank.size();

        if (count <= 0)
            return false;

        // From the selection when there is one, and from the start of the bank when there is
        // not - so the first click on an arrow moves somewhere predictable instead of to the
        // end of the list.
        const int from = current >= 0 ? current : (delta > 0 ? -1 : count);

        const int next = ((from + delta) % count + count) % count;

        return load (next);
    }

    int PresetManager::saveAs (const juce::String& category, const juce::String& name)
    {
        if (name.trim().isEmpty())
            return -1;

        // Saved as a diff against the init patch, so a preset written here is the same shape
        // as one from the bank: what it changes, and nothing else.
        const int index = bank.save (category, name, diffState (proc.buildStateTree(), proc.buildInitState()));

        if (index >= 0)
            markLoaded (index);

        return index;
    }

    bool PresetManager::save()
    {
        if (current < 0 || bank.isFactory (current))
            return false;

        return saveAs (bank.categoryOf (current), bank.nameOf (current)) >= 0;
    }

    bool PresetManager::remove (int index)
    {
        if (! bank.remove (index))
            return false;

        // The patch is deliberately left where it is. Deleting the preset you are listening
        // to and having the sound change under you is the worst possible behaviour for a
        // delete button; the bar just stops claiming a name for it.
        if (current == index)
            markLoaded (-1);

        return true;
    }

    void PresetManager::rememberForUndo()
    {
        undoStack.push_back ({ proc.buildStateTree(), current, loadedPrint });

        if (static_cast<int> (undoStack.size()) > kMaxHistory)
            undoStack.erase (undoStack.begin());

        // A new move invalidates the future: you cannot redo past an edit. Without this, a
        // redo after a roll would reinstate a patch the roll had already replaced.
        redoStack.clear();
    }

    bool PresetManager::undo()
    {
        if (undoStack.empty())
            return false;

        const auto entry = undoStack.back();

        // Captured BEFORE the restore, so redo is the exact inverse rather than a
        // re-derivation of where we think we were.
        const HistoryEntry leaving { proc.buildStateTree(), current, loadedPrint };

        if (! proc.applyStateTree (entry.state))
            return false;

        // Popped only once the restore has succeeded, so a failed undo does not eat the entry.
        undoStack.pop_back();
        redoStack.push_back (leaving);

        // The selection and the fingerprint come back with the patch rather than being
        // re-derived: a patch that was already edited when the move happened is still edited
        // after the undo, and the bar has to keep saying so.
        current = entry.index;
        loadedPrint = entry.print;

        return true;
    }

    bool PresetManager::redo()
    {
        if (redoStack.empty())
            return false;

        const auto entry = redoStack.back();
        const HistoryEntry leaving { proc.buildStateTree(), current, loadedPrint };

        if (! proc.applyStateTree (entry.state))
            return false;

        redoStack.pop_back();
        undoStack.push_back (leaving);

        current = entry.index;
        loadedPrint = entry.print;

        return true;
    }

    void PresetManager::refresh()
    {
        bank.refresh();

        if (current >= 0 && ! bank.isValidIndex (current))
            current = -1;
    }

    int PresetManager::numPrograms() const
    {
        // At least one, because a host that is told there are no programs and then asked for
        // program 0 is being told two contradictory things.
        return juce::jmax (1, bank.size());
    }

    juce::String PresetManager::programName (int index) const
    {
        if (bank.isValidIndex (index))
            return bank.nameOf (index);

        return "Init";
    }

    int PresetManager::currentProgram() const
    {
        return current >= 0 ? current : 0;
    }
}
