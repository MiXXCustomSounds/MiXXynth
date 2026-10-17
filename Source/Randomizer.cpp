#include "Randomizer.h"

#include <cmath>

#include "FmVoice.h"
#include "Fx/FxRack.h"
#include "ModMatrix.h"
#include "ModShape.h"
#include "PluginProcessor.h"

namespace mixxynth
{
    namespace
    {
        // Musical ratios. A uniform draw over the whole 0..16 range is inharmonic almost
        // everywhere, which reads as "broken" rather than as "bright" - and 0 stops the
        // operator's phase advancing, so it is silence rather than a ratio.
        const float kRatios[] = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
        constexpr int kNumRatios = static_cast<int> (sizeof (kRatios) / sizeof (kRatios[0]));

        // Where a rolled matrix slot may point, and the parameter that destination drives.
        //
        // A curated list rather than the whole destination enum, because the enum's tail is
        // thirty matrix depths and a roll that spent most of its slots on those would be a
        // roll that mostly did nothing audible. The parameter id is here so the roll can also
        // place that parameter somewhere the modulation can be HEARD - a +0.4 sweep onto a
        // filter sitting wide open is not a modulation, it is a no-op.
        struct ModDestChoice
        {
            int dest;
            const char* paramId;
            int section;   // the roll section that owns the parameter, so a lock still holds
        };

        const ModDestChoice kModDestinations[] =
        {
            { dstFmIndex,         "fmIndex",      rollFm },
            { dstOp1Level,        "op1level",     rollFm },
            { dstOp2Level,        "op2level",     rollFm },
            { dstOp3Level,        "op3level",     rollFm },
            { dstOp4Level,        "op4level",     rollFm },
            { dstOp5Level,        "op5level",     rollFm },
            { dstOp6Level,        "op6level",     rollFm },
            { dstOp2Ratio,        "op2ratio",     rollFm },
            { dstOp3Ratio,        "op3ratio",     rollFm },
            { dstOp4Ratio,        "op4ratio",     rollFm },
            { dstOp5Ratio,        "op5ratio",     rollFm },
            { dstOp6Ratio,        "op6ratio",     rollFm },
            { dstVaLevel,         "vaLevel",      rollVa },
            { dstVaPulseWidth,    "vaShape",      rollVa },
            { dstFilterCutoff,    "filterCutoff", rollFilter },
            { dstFilterReso,      "filterReso",   rollFilter },
            { dstOp1FilterCutoff, "op1fltCutoff", rollFilter },
            { dstOp2FilterCutoff, "op2fltCutoff", rollFilter },
            { dstOp3FilterCutoff, "op3fltCutoff", rollFilter },
            { dstOp4FilterCutoff, "op4fltCutoff", rollFilter },
            { dstOp5FilterCutoff, "op5fltCutoff", rollFilter },
            { dstOp6FilterCutoff, "op6fltCutoff", rollFilter }
        };

        constexpr int kNumModDestinations = static_cast<int> (sizeof (kModDestinations)
                                                               / sizeof (kModDestinations[0]));

        // A shape from an archetype, because a random breakpoint list is almost always a shape
        // nobody wants. Six that a person would actually draw.
        ModShape randomShape (juce::Random& rng, bool lfo)
        {
            auto shape = lfo ? ModShape::defaultLfo() : ModShape::defaultEnv();

            if (! lfo)
            {
                // An envelope keeps the default's structure - attack, peak, decay, sustain,
                // fall - and moves the times and the sustain level. A random point list here
                // would break the one shape an envelope has to be.
                const float sustain = 0.25f + rng.nextFloat() * 0.7f;
                const float decayX = 0.15f + rng.nextFloat() * 0.5f;
                const float sustainEnd = juce::jmin (0.95f, decayX + 0.2f + rng.nextFloat() * 0.4f);

                shape.points[0] = { 0.0f, 0.0f, -0.5f + rng.nextFloat() };
                shape.points[1] = { 0.02f + rng.nextFloat() * 0.12f, 1.0f,
                                    0.1f + rng.nextFloat() * 0.6f };
                shape.points[2] = { decayX, sustain, 0.0f };
                shape.points[3] = { sustainEnd, sustain, 0.0f };
                shape.points[4] = { 1.0f, 0.0f, 0.0f };
                shape.numPoints = 5;
                shape.releaseSeconds = 0.05f + rng.nextFloat() * 1.2f;

                return shape;
            }

            shape.rateHz = 0.08f * std::pow (9.0f / 0.08f, rng.nextFloat());
            shape.smooth = rng.nextFloat() < 0.35f ? rng.nextFloat() * 0.5f : 0.0f;

            switch (rng.nextInt (6))
            {
                case 0:   // ramp up
                    shape.numPoints = 2;
                    shape.points[0] = { 0.0f, 0.0f, 0.0f };
                    shape.points[1] = { 1.0f, 1.0f, 0.0f };
                    break;

                case 1:   // ramp down
                    shape.numPoints = 2;
                    shape.points[0] = { 0.0f, 1.0f, 0.0f };
                    shape.points[1] = { 1.0f, 0.0f, 0.0f };
                    break;

                case 2:   // triangle
                    shape.numPoints = 3;
                    shape.points[0] = { 0.0f, 0.0f, 0.0f };
                    shape.points[1] = { 0.5f, 1.0f, 0.0f };
                    shape.points[2] = { 1.0f, 0.0f, 0.0f };
                    break;

                case 3:   // pulse, with a duty the roll chooses
                {
                    const float duty = 0.2f + rng.nextFloat() * 0.6f;
                    shape.numPoints = 4;
                    shape.points[0] = { 0.0f, 1.0f, 0.0f };
                    shape.points[1] = { duty, 1.0f, 0.0f };
                    shape.points[2] = { duty, 0.0f, 0.0f };
                    shape.points[3] = { 1.0f, 0.0f, 0.0f };
                    break;
                }

                case 4:   // steps, which is as close to sample-and-hold as breakpoints get
                {
                    const int steps = 4 + rng.nextInt (5);
                    shape.numPoints = steps;

                    for (int i = 0; i < steps; ++i)
                        shape.points[static_cast<size_t> (i)] =
                            { static_cast<float> (i) / static_cast<float> (steps - 1),
                              rng.nextFloat(), 0.0f };

                    break;
                }

                default:  // swell
                    shape.numPoints = 3;
                    shape.points[0] = { 0.0f, 0.0f, 0.5f };
                    shape.points[1] = { 0.3f + rng.nextFloat() * 0.3f, 1.0f, -0.5f };
                    shape.points[2] = { 1.0f, 0.0f, 0.0f };
                    break;
            }

            return shape;
        }
    }

    const char* rollSectionName (int section) noexcept
    {
        switch (section)
        {
            case rollFm:     return "FM";
            case rollVa:     return "VA";
            case rollFilter: return "FILT";
            case rollMod:    return "MOD";
            case rollFx:     return "FX";
            default:         return "?";
        }
    }

    Randomizer::Randomizer (MiXXynthProcessor& p)
        : proc (p)
    {
    }

    void Randomizer::setLocked (int section, bool isLocked)
    {
        if (section >= 0 && section < kNumRollSections)
            locked[static_cast<size_t> (section)] = isLocked;
    }

    bool Randomizer::isLocked (int section) const
    {
        return section >= 0 && section < kNumRollSections && locked[static_cast<size_t> (section)];
    }

    int Randomizer::numLocked() const
    {
        int count = 0;

        for (const bool l : locked)
            count += l ? 1 : 0;

        return count;
    }

    int Randomizer::numUnlocked() const
    {
        return kNumRollSections - numLocked();
    }

    juce::int64 Randomizer::clockSeed()
    {
        return juce::Time::currentTimeMillis();
    }

    void Randomizer::set (const juce::String& id, float realValue)
    {
        if (auto* param = proc.apvts.getParameter (id))
        {
            // Through the parameter rather than the state tree, so the host is told and the
            // editor's poll sees it - the same path a hand move takes.
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (realValue));
            param->endChangeGesture();
        }
    }

    float Randomizer::get (const juce::String& id) const
    {
        if (auto* param = proc.apvts.getParameter (id))
            return param->convertFrom0to1 (param->getValue());

        return 0.0f;
    }

    float Randomizer::between (juce::Random& rng, float low, float high)
    {
        if (low > high)
            std::swap (low, high);

        return low + rng.nextFloat() * (high - low);
    }

    float Randomizer::betweenLog (juce::Random& rng, float low, float high)
    {
        if (low > high)
            std::swap (low, high);

        return low * std::pow (high / low, rng.nextFloat());
    }

    float Randomizer::fxChoiceValue (int index, int count)
    {
        return count > 1 ? static_cast<float> (index) / static_cast<float> (count - 1)
                         : 0.0f;
    }

    void Randomizer::rollFmSection (juce::Random& rng)
    {
        set ("fmIndex", between (rng, 0.05f, 0.85f));

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n (i + 1);

            set ("op" + n + "ratio", kRatios[rng.nextInt (kNumRatios)]);
            set ("op" + n + "level", between (rng, 0.15f, 0.9f));
            set ("op" + n + "feedback", between (rng, 0.0f, 0.55f));
            set ("op" + n + "wave", rng.nextFloat());
            set ("op" + n + "attack", between (rng, 0.0f, 0.35f));
            set ("op" + n + "decay", between (rng, 0.05f, 1.6f));
            set ("op" + n + "sustain", between (rng, 0.25f, 1.0f));
            set ("op" + n + "release", between (rng, 0.05f, 2.0f));
        }

        // The routing. A free matrix with every cell at zero is six carriers with no FM at
        // all, so a roll that left it alone would be a roll that produced a sine - which is
        // why the routing belongs to the FM section rather than being assumed.
        //
        // The shape is a two-level stack rather than random edges: pick the carriers, then
        // give each of them modulators from the operators that are not carriers. Random edges
        // produce cycles as often as they produce a stack, and a cycle here is not a timbre,
        // it is a feedback loop with nothing to contain it.
        std::array<bool, MiXXFmVoice::numOps> isCarrier {};

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n (i + 1);

            set ("op" + n + "on", 1.0f);

            for (int d = 0; d < MiXXFmVoice::numOps; ++d)
                if (i != d)
                    set ("mtx" + n + juce::String (d + 1), 0.0f);
        }

        for (int c = 0, count = 1 + rng.nextInt (2); c < count; ++c)
            isCarrier[static_cast<size_t> (rng.nextInt (MiXXFmVoice::numOps))] = true;

        // The audibility guarantee, and it is structural rather than a patch-up afterwards:
        // the first carrier gets a real ratio, a real level and a sustain that holds, so a
        // roll can never land on silence.
        int firstCarrier = 0;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
            if (isCarrier[static_cast<size_t> (i)])
            {
                firstCarrier = i;
                break;
            }

        isCarrier[static_cast<size_t> (firstCarrier)] = true;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n (i + 1);

            set ("carrier" + n, isCarrier[static_cast<size_t> (i)] ? 1.0f : 0.0f);

            if (i == firstCarrier)
            {
                set ("op" + n + "ratio", kRatios[rng.nextInt (kNumRatios)]);
                set ("op" + n + "level", between (rng, 0.5f, 0.85f));
                set ("op" + n + "decay", between (rng, 0.2f, 1.2f));
                set ("op" + n + "sustain", between (rng, 0.45f, 1.0f));
                set ("op" + n + "release", between (rng, 0.1f, 1.5f));
            }
        }

        // Modulators, one or two per carrier, from the operators that are not carriers.
        for (int c = 0; c < MiXXFmVoice::numOps; ++c)
        {
            if (! isCarrier[static_cast<size_t> (c)])
                continue;

            for (int m = 0, count = rng.nextInt (3); m < count; ++m)
            {
                const int src = rng.nextInt (MiXXFmVoice::numOps);

                if (isCarrier[static_cast<size_t> (src)] || src == c)
                    continue;

                set ("mtx" + juce::String (src + 1) + juce::String (c + 1),
                     between (rng, 0.25f, 0.95f));
            }
        }
    }

    void Randomizer::rollVaSection (juce::Random& rng)
    {
        // The VA is a blend under the FM rather than a second synth fighting it, so its level
        // stays in the lower half: a roll that put the VA at full would bury the FM engine the
        // instrument is about.
        set ("vaLevel", between (rng, 0.0f, 0.55f));
        set ("vaShape", rng.nextFloat());
        set ("vaOctave", static_cast<float> (-1 + rng.nextInt (3)));
        set ("vaDetune", between (rng, -12.0f, 12.0f));
        set ("vaSubLevel", between (rng, 0.0f, 0.45f));
        set ("vaNoiseLevel", between (rng, 0.0f, 0.2f));

        set ("vaAttack", between (rng, 0.005f, 0.4f));
        set ("vaDecay", between (rng, 0.0f, 1.5f));
        set ("vaSustain", between (rng, 0.4f, 1.0f));
        set ("vaRelease", between (rng, 0.05f, 1.5f));
    }

    void Randomizer::rollFilterSection (juce::Random& rng)
    {
        // On, and inside the band. A filter rolled wide open is a filter that does nothing,
        // and one rolled to 40 Hz is a patch that has gone silent - both of which read as a
        // broken roll rather than as a decision.
        set ("filterOn", 1.0f);
        set ("filterType", static_cast<float> (rng.nextInt (3)));
        set ("filterCutoff", betweenLog (rng, 350.0f, 12000.0f));
        set ("filterReso", between (rng, 0.0f, 0.6f));
        set ("filterDrive", between (rng, 0.0f, 0.45f));

        // The per-operator filters: mostly off, because six filters at once is mud rather than
        // a roll. One or two is where they earn their place.
        std::array<bool, MiXXFmVoice::numOps> enabled {};

        for (int i = 0, count = rng.nextInt (3); i < count; ++i)
            enabled[static_cast<size_t> (rng.nextInt (MiXXFmVoice::numOps))] = true;

        for (int i = 0; i < MiXXFmVoice::numOps; ++i)
        {
            const juce::String n (i + 1);

            set ("op" + n + "fltOn", enabled[static_cast<size_t> (i)] ? 1.0f : 0.0f);
            set ("op" + n + "fltType", static_cast<float> (rng.nextInt (3)));
            set ("op" + n + "fltCutoff", betweenLog (rng, 400.0f, 9000.0f));
            set ("op" + n + "fltReso", between (rng, 0.0f, 0.5f));
            set ("op" + n + "fltDrive", between (rng, 0.0f, 0.4f));
            set ("op" + n + "fltAttack", between (rng, 0.0f, 0.25f));
            set ("op" + n + "fltDecay", between (rng, 0.05f, 1.8f));
            set ("op" + n + "fltSustain", between (rng, 0.0f, 0.8f));
            set ("op" + n + "fltRelease", between (rng, 0.05f, 1.2f));
            set ("op" + n + "fltRoute", static_cast<float> (rng.nextInt (3)));
        }
    }

    void Randomizer::rollModSection (juce::Random& rng)
    {
        // The shapes first, because the slots point at them: a slot driving an LFO that is
        // still a plain sine is a slot that could have been a fixed offset instead.
        for (int i = 0; i < proc.getNumShapeModules(); ++i)
            proc.setShape (i, randomShape (rng, proc.shapeIsLfo (i)));

        // Every slot back to Off, so a roll is a routing rather than an accumulation on top of
        // the last one.
        for (int s = 0; s < kNumModSlots; ++s)
        {
            const juce::String n (s + 1);

            set ("mod" + n + "src", 0.0f);
            set ("mod" + n + "dst", 0.0f);
            set ("mod" + n + "depth", 0.0f);
        }

        // Which destinations this roll may reach. A destination whose own section is locked is
        // off limits: a lock means "do not touch this", and it holds across sections or it is
        // not a lock.
        juce::Array<int> allowed;

        for (int d = 0; d < kNumModDestinations; ++d)
            if (! isLocked (kModDestinations[d].section))
                allowed.add (d);

        if (allowed.isEmpty())
            return;

        const int numSlots = juce::jmin (2 + rng.nextInt (4), kNumModSlots);   // two to five
        const int numModuleSources = proc.getNumShapeModules();

        for (int s = 0; s < numSlots; ++s)
        {
            const auto& choice = kModDestinations[allowed[rng.nextInt (allowed.size())]];

            // The destination's base value moves first, so the slot has somewhere to move it:
            // a sweep onto a parameter that is already at its limit is a slot that does
            // nothing, and a roll that lands on those half the time feels broken.
            placeDestination (choice.paramId, rng);

            // A source that exists. The modules are contiguous from srcLfo1, and the macros
            // are always there.
            const int src = (numModuleSources > 0 && rng.nextFloat() < 0.7f)
                                ? srcLfo1 + rng.nextInt (numModuleSources)
                                : srcMacro1 + rng.nextInt (kNumMacros);

            // A depth with a magnitude worth hearing. A uniform draw around zero would spend
            // most of its range on amounts that are indistinguishable from Off.
            const float magnitude = between (rng, 0.2f, 0.55f);

            set ("mod" + juce::String (s + 1) + "src", static_cast<float> (src));
            set ("mod" + juce::String (s + 1) + "dst", static_cast<float> (choice.dest));
            set ("mod" + juce::String (s + 1) + "depth",
                 rng.nextBool() ? magnitude : -magnitude);
        }
    }

    void Randomizer::placeDestination (const juce::String& id, juce::Random& rng)
    {
        // Every value here is chosen so the modulation has room to move rather than so the
        // parameter looks interesting on its own.
        if (id == "fmIndex")                set (id, between (rng, 0.12f, 0.45f));
        else if (id == "vaLevel")           set (id, between (rng, 0.2f, 0.5f));
        else if (id == "vaShape")           set (id, between (rng, 0.15f, 0.8f));
        else if (id == "filterCutoff")      set (id, betweenLog (rng, 900.0f, 5000.0f));
        else if (id == "filterReso")        set (id, between (rng, 0.15f, 0.5f));
        else if (id.endsWith ("fltCutoff")) set (id, betweenLog (rng, 1200.0f, 6000.0f));
        else if (id.endsWith ("level"))     set (id, between (rng, 0.35f, 0.7f));
        else if (id.endsWith ("ratio"))     set (id, kRatios[rng.nextInt (kNumRatios)]);
    }

    void Randomizer::rollFxSection (juce::Random& rng)
    {
        // An empty rack, so a roll is a chain rather than an accumulation.
        for (int s = 0; s < kMaxFxSlots; ++s)
        {
            const juce::String n (s + 1);

            set ("fx" + n + "type", 0.0f);
            set ("fx" + n + "bypass", 0.0f);

            for (int p = 0; p < kMaxFxParams; ++p)
                set ("fx" + n + "p" + juce::String (p + 1), 0.0f);
        }

        // Distinct types, drawn without replacement: two of the same module in a row is not a
        // chain, it is a mistake.
        juce::Array<int> pool;

        for (int t = fxWeirdVerb; t < kNumFxTypes; ++t)
            pool.add (t);

        const int numModules = juce::jmin (2 + rng.nextInt (3), kMaxFxSlots);   // two to four

        for (int s = 0; s < numModules; ++s)
        {
            const int pick = rng.nextInt (pool.size());
            const int type = pool[pick];
            pool.remove (pick);

            const juce::String n (s + 1);
            set ("fx" + n + "type", static_cast<float> (type));

            // Every control the module declares, moved off its default by a bounded amount
            // rather than drawn from the whole range. The defaults are the module author's idea
            // of usable, so a jittered default stays usable; a uniform draw would set a delay's
            // mix to nothing as often as to everything, which is two ways to do nothing.
            const auto desc = FxRack::descriptorFor (type);

            for (int p = 0; p < desc.numParams && p < kMaxFxParams; ++p)
            {
                const auto& info = desc.params[static_cast<size_t> (p)];
                const juce::String id = "fx" + n + "p" + juce::String (p + 1);

                if (info.isChoice && info.choices.size() > 0)
                {
                    set (id, fxChoiceValue (rng.nextInt (info.choices.size()),
                                            info.choices.size()));
                    continue;
                }

                // A mix or a level moves less than everything else. Without that distinction a
                // roll kept landing on modules that were either fully wet or fully dry, which
                // is the one place a jittered default is not good enough.
                const juce::String name (info.name);
                const bool isMix = name.containsIgnoreCase ("mix")
                                     || name.containsIgnoreCase ("level");
                const float spread = isMix ? 0.22f : 0.38f;
                const float jitter = (rng.nextFloat() * 2.0f - 1.0f) * spread;

                set (id, juce::jlimit (0.0f, 1.0f, info.defaultValue + jitter));
            }
        }
    }

    void Randomizer::roll (juce::int64 seed)
    {
        // One generator for the whole roll, seeded once, so the seed alone decides the patch
        // and a section's lock changes only what it is applied to.
        juce::Random rng (seed);

        if (! isLocked (rollFm))     rollFmSection (rng);
        if (! isLocked (rollVa))     rollVaSection (rng);
        if (! isLocked (rollFilter)) rollFilterSection (rng);
        if (! isLocked (rollMod))    rollModSection (rng);
        if (! isLocked (rollFx))     rollFxSection (rng);
    }
}
