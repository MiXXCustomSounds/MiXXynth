#include "ModShape.h"

#include <algorithm>

namespace mixxynth
{
namespace
{
    constexpr float kPi = 3.14159265358979323846f;
}

void ModShape::sortPoints()
{
    numPoints = juce::jlimit (2, kMaxShapePoints, numPoints);

    std::sort (points.begin(), points.begin() + numPoints,
               [] (const ShapePoint& a, const ShapePoint& b) { return a.x < b.x; });

    points[0].x = 0.0f;
    points[static_cast<size_t> (numPoints - 1)].x = 1.0f;

    for (int i = 0; i < numPoints; ++i)
    {
        auto& p = points[static_cast<size_t> (i)];
        p.x = juce::jlimit (0.0f, 1.0f, p.x);
        p.y = juce::jlimit (0.0f, 1.0f, p.y);
        p.tension = juce::jlimit (-1.0f, 1.0f, p.tension);
    }
}

float applyTension (float u, float tension)
{
    u = juce::jlimit (0.0f, 1.0f, u);

    if (std::abs (tension) < 1.0e-4f)
        return u;

    // Exponent 0.125..8: tension > 0 rises quickly (ease out), tension < 0
    // rises slowly (ease in). Monotonic, so the curve can never fold back.
    const float exponent = std::pow (2.0f, -juce::jlimit (-1.0f, 1.0f, tension) * 3.0f);
    return std::pow (u, exponent);
}

float ModShape::valueAt (float x01) const
{
    const int n = juce::jlimit (2, kMaxShapePoints, numPoints);

    if (x01 <= points[0].x)
        return points[0].y;

    if (x01 >= points[static_cast<size_t> (n - 1)].x)
        return points[static_cast<size_t> (n - 1)].y;

    for (int i = 0; i < n - 1; ++i)
    {
        const auto& a = points[static_cast<size_t> (i)];
        const auto& b = points[static_cast<size_t> (i + 1)];

        if (x01 >= a.x && x01 <= b.x)
        {
            const float span = b.x - a.x;

            if (span <= 1.0e-6f)
                return b.y;

            const float u = (x01 - a.x) / span;
            return a.y + (b.y - a.y) * applyTension (u, a.tension);
        }
    }

    return points[static_cast<size_t> (n - 1)].y;
}

ModShape ModShape::defaultLfo()
{
    ModShape s;
    s.mode = shapeLfo;
    s.rateHz = 2.0f;
    s.bipolar = true;

    // Nine samples of a sine, so the default is immediately musical.
    s.numPoints = 9;

    for (int i = 0; i < s.numPoints; ++i)
    {
        const float x = static_cast<float> (i) / static_cast<float> (s.numPoints - 1);
        s.points[static_cast<size_t> (i)] = { x, 0.5f - 0.5f * std::cos (2.0f * kPi * x), 0.0f };
    }

    return s;
}

ModShape ModShape::defaultEnv()
{
    ModShape s;
    s.mode = shapeEnv;
    s.rateHz = 1.0f;
    s.bipolar = false;
    s.releaseSeconds = 0.3f;
    s.numPoints = 5;
    s.points[0] = { 0.00f, 0.0f, -0.35f }; // attack, eased
    s.points[1] = { 0.06f, 1.0f, 0.30f };  // peak, quick fall
    s.points[2] = { 0.35f, 0.6f, 0.0f };   // decay to sustain
    s.points[3] = { 0.75f, 0.6f, 0.0f };   // sustain
    s.points[4] = { 1.00f, 0.0f, 0.0f };   // fall to zero
    return s;
}

void buildShapePath (const ModShape& shape, juce::Path& path,
                     juce::Rectangle<float> area, bool bipolar)
{
    path.clear();

    const int steps = 160;

    for (int i = 0; i <= steps; ++i)
    {
        const float x01 = static_cast<float> (i) / static_cast<float> (steps);
        const float y01 = juce::jlimit (0.0f, 1.0f, shape.valueAt (x01));

        const float px = area.getX() + x01 * area.getWidth();
        const float py = bipolar ? area.getCentreY() - (y01 - 0.5f) * area.getHeight()
                                 : area.getBottom() - y01 * area.getHeight();

        if (i == 0)
            path.startNewSubPath (px, py);
        else
            path.lineTo (px, py);
    }
}

juce::ValueTree shapeToValueTree (const ModShape& shape, const juce::Identifier& type)
{
    juce::ValueTree tree (type);

    tree.setProperty ("mode", shape.mode, nullptr);
    tree.setProperty ("rate", shape.rateHz, nullptr);
    tree.setProperty ("sync", shape.sync, nullptr);
    tree.setProperty ("syncIndex", shape.syncIndex, nullptr);
    tree.setProperty ("phase", shape.phase, nullptr);
    tree.setProperty ("delay", shape.delaySeconds, nullptr);
    tree.setProperty ("smooth", shape.smooth, nullptr);
    tree.setProperty ("bipolar", shape.bipolar, nullptr);
    tree.setProperty ("release", shape.releaseSeconds, nullptr);
    tree.setProperty ("retrig", shape.retrig, nullptr);

    juce::String encoded;

    for (int i = 0; i < shape.numPoints; ++i)
    {
        const auto& p = shape.points[static_cast<size_t> (i)];
        encoded << p.x << ',' << p.y << ',' << p.tension << ';';
    }

    tree.setProperty ("points", encoded, nullptr);
    return tree;
}

void shapeFromValueTree (const ModShape& fallback, const juce::ValueTree& tree, ModShape& out)
{
    out = fallback;

    if (! tree.isValid())
        return;

    out.mode = juce::jlimit (0, kNumShapeModes - 1,
                             static_cast<int> (tree.getProperty ("mode", fallback.mode)));
    out.rateHz = static_cast<float> (tree.getProperty ("rate", fallback.rateHz));
    out.sync = static_cast<bool> (tree.getProperty ("sync", fallback.sync));
    out.syncIndex = static_cast<int> (tree.getProperty ("syncIndex", fallback.syncIndex));
    out.phase = static_cast<float> (tree.getProperty ("phase", fallback.phase));
    out.delaySeconds = static_cast<float> (tree.getProperty ("delay", fallback.delaySeconds));
    out.smooth = static_cast<float> (tree.getProperty ("smooth", fallback.smooth));
    out.bipolar = static_cast<bool> (tree.getProperty ("bipolar", fallback.bipolar));
    out.releaseSeconds = static_cast<float> (tree.getProperty ("release", fallback.releaseSeconds));
    out.retrig = static_cast<bool> (tree.getProperty ("retrig", fallback.retrig));

    const auto encoded = tree.getProperty ("points").toString();

    if (encoded.isEmpty())
        return;

    juce::StringArray triples;
    triples.addTokens (encoded, ";", "");

    int count = 0;

    for (const auto& triple : triples)
    {
        juce::StringArray values;
        values.addTokens (triple, ",", "");

        if (values.size() < 3 || count >= kMaxShapePoints)
            continue;

        out.points[static_cast<size_t> (count)] = { values[0].getFloatValue(),
                                                    values[1].getFloatValue(),
                                                    values[2].getFloatValue() };
        ++count;
    }

    if (count >= 2)
    {
        out.numPoints = count;
        out.sortPoints();
    }
}
}
