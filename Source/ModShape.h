#pragma once

#include <array>
#include <cmath>

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

// Shape-editable modulation sources (Serum / Vital style).
//
// A shape is a list of breakpoints carrying x (position 0..1), y (level 0..1)
// and a per-segment curve tension. The tension applies to the segment that
// STARTS at that point, which is how both Serum and Vital model it.
//
// The reference implementations (Vital, Surge XT) are GPLv3, so nothing is
// copied from them: this is an independent MIT implementation of the same
// well-known model.
//
// Shapes live in the plugin state ValueTree rather than as individual
// automatable parameters, which is what Serum and Vital do and avoids adding
// hundreds of parameters.
namespace mixxynth
{
    constexpr int kMaxShapePoints = 16;
    constexpr int kNumLfos        = 4;
    constexpr int kNumModEnvs     = 3;
    constexpr int kNumShapes      = kNumLfos + kNumModEnvs; // 4 LFOs + 3 ENVs

    enum ShapeMode
    {
        shapeLfo = 0,  // loops
        shapeEnv,      // one pass on note-on, holds, releases on note-off
        shapeOneShot   // one pass on note-on, then holds (ignores note-off)
    };

    constexpr int kNumShapeModes = 3;

    struct ShapePoint
    {
        float x = 0.0f;       // 0..1 position
        float y = 0.0f;       // 0..1 level
        float tension = 0.0f; // -1..1 curve of the segment starting here
    };

    struct ModShape
    {
        std::array<ShapePoint, kMaxShapePoints> points {};
        int numPoints = 2;
        int mode = shapeLfo;
        float rateHz = 2.0f;
        bool sync = false;
        int syncIndex = 3;
        float phase = 0.0f;
        float delaySeconds = 0.0f;
        float smooth = 0.0f;      // 0..1 one-pole smoothing of the output
        bool bipolar = true;
        float releaseSeconds = 0.3f;
        bool retrig = true;

        // Sorted by x; the editor guarantees this.
        void sortPoints();
        float valueAt (float x01) const;

        static ModShape defaultLfo();
        static ModShape defaultEnv();
    };

    // Shared curve shaping, used by both the DSP and the editor so the drawing
    // and the sound can never disagree.
    float applyTension (float u, float tension);

    // Builds the curve into `path` for a component area. bipolar draws around
    // the vertical centre.
    void buildShapePath (const ModShape& shape, juce::Path& path,
                         juce::Rectangle<float> area, bool bipolar);

    // Serialisation (into the plugin state ValueTree).
    juce::ValueTree shapeToValueTree (const ModShape& shape, const juce::Identifier& type);
    void shapeFromValueTree (const ModShape& fallback, const juce::ValueTree& tree, ModShape& out);
}
