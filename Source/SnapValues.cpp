#include "SnapValues.h"

#include <cmath>

namespace mixxynth
{
    namespace
    {
        bool snapFlag = false;

        // Musical ratios. These are the ones a DX-style FM synth actually uses: the integer
        // series, the half and the two-and-a-half, and the octave multiples. A ratio knob
        // that lands on 1.873 is a ratio knob you have to fight.
        const float kRatios[] =
        {
            0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
            9.0f, 10.0f, 11.0f, 12.0f, 14.0f, 16.0f
        };

        // Detune, in whatever unit the parameter is in - cents for the VA, and the same shape
        // for the unison. Ten-cent steps: fine enough to tune, coarse enough that two clicks
        // is a deliberate interval rather than a coincidence.
        const float kDetune[] =
        {
            -50.0f, -40.0f, -30.0f, -20.0f, -10.0f, -5.0f, 0.0f, 5.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f
        };

        // Times in seconds. The decade marks plus the few in between that people actually
        // reach for, because a percussive attack is 1 ms and a pad's is 500.
        const float kTimes[] =
        {
            0.0f, 0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f,
            1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 30.0f
        };

        // Rates in Hz.
        const float kRates[] =
        {
            0.0f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 8.0f, 10.0f, 20.0f, 40.0f
        };

        // Frequencies in Hz, for the filters. Roughly third-octave where it matters, because
        // a cutoff is placed by ear against the note being played.
        const float kFrequencies[] =
        {
            20.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 800.0f, 1000.0f, 1500.0f, 2000.0f,
            3000.0f, 5000.0f, 8000.0f, 12000.0f, 16000.0f, 20000.0f
        };

        // Levels, depths, mixes and everything else that is a fraction of something. 5% steps:
        // fine enough that it does not feel notchy, coarse enough that the round numbers are
        // always reachable.
        const float kFractions[] =
        {
            0.0f, 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f,
            0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f
        };

        struct SnapSet
        {
            const float* points;
            int count;
        };

        // Which set a parameter uses, by name. Ordered: the first match wins, so the more
        // specific patterns come first.
        SnapSet setFor (const juce::String& id)
        {
            const auto endsWith = [&id] (const char* suffix)
            {
                return id.endsWith (suffix);
            };

            if (endsWith ("ratio") || id.contains ("Ratio"))
                return { kRatios, static_cast<int> (sizeof (kRatios) / sizeof (kRatios[0])) };

            if (id.containsIgnoreCase ("detune"))
                return { kDetune, static_cast<int> (sizeof (kDetune) / sizeof (kDetune[0])) };

            if (endsWith ("attack") || endsWith ("decay") || endsWith ("release")
                  || endsWith ("Attack") || endsWith ("Decay") || endsWith ("Release"))
                return { kTimes, static_cast<int> (sizeof (kTimes) / sizeof (kTimes[0])) };

            if (endsWith ("rate") || endsWith ("Rate"))
                return { kRates, static_cast<int> (sizeof (kRates) / sizeof (kRates[0])) };

            if (id.containsIgnoreCase ("cutoff") || id.containsIgnoreCase ("freq"))
                return { kFrequencies, static_cast<int> (sizeof (kFrequencies) / sizeof (kFrequencies[0])) };

            // The fallback, for levels and depths and mixes. A parameter with no set at all
            // would make the switch look broken on most of the panel.
            return { kFractions, static_cast<int> (sizeof (kFractions) / sizeof (kFractions[0])) };
        }
    }

    bool SnapValues::isEnabled() noexcept { return snapFlag; }
    void SnapValues::setEnabled (bool shouldSnap) noexcept { snapFlag = shouldSnap; }

    int SnapValues::numSnapPointsFor (const juce::String& paramId)
    {
        return setFor (paramId).count;
    }

    const float* SnapValues::snapPointsFor (const juce::String& paramId, int& count)
    {
        const auto set = setFor (paramId);
        count = set.count;
        return set.points;
    }

    float SnapValues::snap (const juce::RangedAudioParameter& param, float normalised)
    {
        if (! snapFlag)
            return normalised;

        const auto set = setFor (param.getParameterID());

        if (set.count <= 0)
            return normalised;

        const float real = param.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised));

        // The nearest point in real units, not in normalised ones. A ratio range is skewed, so
        // snapping the normalised value would put 2.0 nowhere near where it looks like it
        // should be.
        float best = set.points[0];
        float bestDistance = std::abs (real - best);

        for (int i = 1; i < set.count; ++i)
        {
            const float distance = std::abs (real - set.points[i]);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = set.points[i];
            }
        }

        return juce::jlimit (0.0f, 1.0f, param.convertTo0to1 (best));
    }
}
