#pragma once

#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

// The preset bank (M7). See docs/PLAN-M7.md.
//
// Two sources, one list:
//   - factory presets, embedded in the binary from `presets/<Category> - <Name>.xml` in the
//     repository through juce_add_binary_data. Embedded rather than copied to disk on first
//     run, because a VST3 installed into a DAW has no repository to copy from and a disk
//     mirror that is never read is a second source of truth that can drift.
//   - user presets, on disk under %APPDATA%/MiXXynth/Presets/.
//
// A preset FILE is the plugin state XML with no wrapper. The name and the category come from
// the filename, in the form "<Category> - <Name>.xml", so the category travels with the file
// when it is copied or moved and there is nothing inside the file to fall out of step with
// its name. A state the user exported from a DAW session therefore loads unchanged.
//
// Nothing here knows about the processor, so the bank can be listed, read and written without
// one - which is what lets the tests cover it directly.
namespace mixxynth
{
    // The separator between a preset's category and its name, in the filename.
    inline const char* const presetNameSeparator = " - ";

    struct PresetInfo
    {
        juce::String name;      // "Glass Bell"
        juce::String category;  // "Bell"
        bool factory = false;   // embedded in the binary, so read-only
        juce::File file;        // empty for a factory preset
        juce::String resource;  // the BinaryData resource name, for a factory preset
    };

    // The categories, in the order the browser shows them. A curated order rather than
    // alphabetical: the first entry is what someone lands on when they open the list, and
    // "Bass" being first alphabetically is a coincidence rather than a decision.
    const juce::StringArray& presetCategoryOrder();

    class PresetStore
    {
    public:
        PresetStore();

        // %APPDATA%/MiXXynth/Presets, or the platform equivalent. Created on demand by save().
        static juce::File userPresetDir();

        // Lists the factory bank and re-reads the user folder. Message thread.
        void refresh();

        int size() const;

        // By value, because the list can be re-read on the message thread while a host asks
        // for a program name on another one. A reference into the vector would be a reference
        // into something that can be replaced underneath the caller.
        PresetInfo at (int index) const;

        bool isValidIndex (int index) const;
        bool isFactory (int index) const;
        juce::String nameOf (int index) const;
        juce::String categoryOf (int index) const;

        // Index of the preset with this category and name, or -1. Factory wins a tie, because
        // a user preset cannot be saved over a factory one.
        int indexOf (const juce::String& category, const juce::String& name) const;

        // The preset's state tree, or an invalid tree when it cannot be read.
        juce::ValueTree read (int index) const;

        // Writes a user preset. Returns its index, or -1 when the file cannot be written.
        int save (const juce::String& category, const juce::String& name, const juce::ValueTree& state);

        // Deletes a user preset. Refuses a factory preset, and refuses an index it does not have.
        bool remove (int index);

        // Category names present in the bank, in presetCategoryOrder() then alphabetical.
        juce::StringArray categories() const;

        int numFactory() const;
        int numUser() const;

        // Where a user preset of this category and name lives.
        static juce::File fileFor (const juce::String& category, const juce::String& name);

        // Splits "Bell - Glass Bell" into its two halves. A filename with no separator is all
        // name, and lands in the "Other" category rather than being rejected - a preset
        // someone dropped in by hand should still show up.
        static void splitName (const juce::String& fileNameWithoutExtension,
                               juce::String& category, juce::String& name);

    private:
        void listFactory();
        void listUser();

        // No lock: for callers that already hold it.
        const PresetInfo* rawAt (int index) const;

        std::vector<PresetInfo> presets;

        // Guards `presets`. Recursive, because save() and remove() re-list through refresh().
        mutable juce::CriticalSection lock;
    };

    // The shape of a parameter inside a state tree, which is JUCE's rather than ours: one PARAM
    // child per parameter, carrying its id and its value.
    //
    // These three names are AudioProcessorValueTreeState's own valueType / idPropertyID /
    // valuePropertyID, which are private there. They are spelled out here because a preset has
    // to be the same shape as the state a DAW stores - and getting it wrong is silent: a value
    // written as a property of the ROOT is simply ignored on load, which is how the first
    // version of this bank loaded thirty patches as one init patch. The patch audit now checks
    // the round trip, so a mistake here fails a test rather than sounding wrong.
    namespace presetState
    {
        inline const juce::Identifier parameterNode { "PARAM" };
        inline const juce::Identifier idProperty { "id" };
        inline const juce::Identifier valueProperty { "value" };
    }

    // A preset is a PARTIAL state: it carries only what it changes, and loading it means
    // laying it over the init patch. This is that layering, in one place.
    //
    // Parameter nodes are merged BY ID, because every one of them shares the type "PARAM" - so
    // a merge that keyed on the node type would collapse four hundred and fifty parameters into
    // one. Every other child (the shapes, the FX files) replaces wholesale by type.
    juce::ValueTree overlayState (const juce::ValueTree& base, const juce::ValueTree& overlay);

    // The other direction: what `full` changes about `init`. Saving goes through this so a
    // preset written by the user is as compact as one from the bank, and so there is only
    // ever one preset format rather than a factory one and a user one.
    juce::ValueTree diffState (const juce::ValueTree& full, const juce::ValueTree& init);

    // A stable hash of everything in a state tree, so "has this patch been edited since it
    // was loaded" is a comparison rather than a change flag. A change flag would call a load
    // followed by a hand-undo dirty; this does not.
    juce::int64 fingerprint (const juce::ValueTree& tree);
}
