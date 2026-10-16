#include "OpWave.h"

#include <cmath>

namespace mixxynth
{
namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr double kTwoPi = 6.28318530717958647692;

    // The lowest fundamental the mip map has to cover.
    constexpr double kBaseFreq = 27.5; // A0

    // Duty of each shape, indexed by shape. The first four entries are unused: the
    // one shape that is not a pulse is the saw, and the switch never reads them.
    constexpr double kShapeDuty[] = { 0.5, 0.5, 0.5, 0.5, 0.25, 0.125, 0.0625 };

    // Fourier series with unit peak for each shape. Saw and the pulses overshoot by the
    // usual Gibbs amount once truncated, which is expected and standard.
    float harmonicAmp (int shape, int h)
    {
        switch (shape)
        {
            case 0: // sine: fundamental only
                return h == 1 ? 1.0f : 0.0f;

            case 1: // triangle: odd h, 1/h^2, alternating sign
                if ((h % 2) == 0)
                    return 0.0f;

                return (8.0f / (kPi * kPi)) * ((((h - 1) / 2) % 2) == 0 ? 1.0f : -1.0f)
                         / static_cast<float> (h * h);

            case 2: // saw: all h, 1/h
                return (2.0f / kPi) / static_cast<float> (h);

            default: // pulses, including the square at duty 0.5
            {
                // b_n = 2(1 - cos(2*pi*n*d)) / (pi*n) for a bipolar pulse of duty d. At
                // d = 0.5 it collapses to 4/(pi*n) for odd n and 0 for even n, which is
                // the square this used to hard-code - so the square is now simply the
                // first pulse rather than a special case, and the narrowing stops fall
                // out of the same formula instead of needing their own series.
                const int clampedShape = juce::jlimit (0, OpWaveTables::numShapes - 1, shape);
                const double d = kShapeDuty[static_cast<size_t> (clampedShape)];
                const double n = static_cast<double> (h);

                return static_cast<float> (2.0 * (1.0 - std::cos (kTwoPi * d * n))
                                             / (kTwoPi * 0.5 * n));
            }
        }
    }

    // Cosine coefficient for a shape. Zero for every shape that is odd about zero, which
    // is all of sine, triangle and saw, so only the pulses use it.
    //
    // The pulses need it because a SINE series can only represent an odd-symmetric
    // function. A pulse at duty 0.5 is odd-symmetric, which is why the square always
    // worked, but a pulse at any other duty is not - so a sine-only series converges to
    // the pulse's odd part, which is a square at double the intended rate. The first
    // attempt at the narrowing stops did exactly that, and every one of them measured a
    // duty of 0.5.
    float harmonicAmpCos (int shape, int h)
    {
        if (shape < 3)
            return 0.0f;

        const int clampedShape = juce::jlimit (0, OpWaveTables::numShapes - 1, shape);
        const double d = kShapeDuty[static_cast<size_t> (clampedShape)];
        const double n = static_cast<double> (h);

        // a_n = 2 sin(2*pi*n*d) / (pi*n)
        return static_cast<float> (2.0 * std::sin (kTwoPi * d * n) / (kTwoPi * 0.5 * n));
    }

    // DC offset. A pulse is not centred on zero unless its duty is 0.5, and without this
    // the narrow pulses would sit at the wrong mean level.
    float harmonicDC (int shape)
    {
        if (shape < 3)
            return 0.0f;

        const int clampedShape = juce::jlimit (0, OpWaveTables::numShapes - 1, shape);
        return static_cast<float> (2.0 * kShapeDuty[static_cast<size_t> (clampedShape)] - 1.0);
    }
}

void OpWaveTables::prepare (double sr)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;

    // Shared sine lookup so building the tables does not call std::sin millions
    // of times.
    sineLut.resize (static_cast<size_t> (sineLutSize) + 1);

    for (int i = 0; i <= sineLutSize; ++i)
        sineLut[static_cast<size_t> (i)] =
            std::sin (kTwoPi * static_cast<double> (i) / static_cast<double> (sineLutSize));

    const double nyquist = sampleRate * 0.5;

    for (int mip = 0; mip < numMips; ++mip)
    {
        const double f = kBaseFreq * std::pow (2.0, static_cast<double> (mip));
        const int h = juce::jlimit (1, 256, static_cast<int> (std::floor (nyquist / f)));
        mipHarmonics[static_cast<size_t> (mip)] = h;

        for (int shape = 0; shape < numShapes; ++shape)
            buildShape (shape, mip, h);
    }

    prepared = true;
}

void OpWaveTables::buildShape (int shape, int mip, int harmonics)
{
    auto& table = tables[static_cast<size_t> (mip)][static_cast<size_t> (shape)];
    table.assign (static_cast<size_t> (tableSize), 0.0f);

    // DC first. It is zero for every odd-symmetric shape and non-zero for a pulse away
    // from duty 0.5, which is what keeps the narrow pulses centred on zero.
    const float dc = harmonicDC (shape);

    if (dc != 0.0f)
        for (auto& s : table)
            s = dc;

    // cos(2*pi*x) is sin(2*pi*(x + 1/4)), so the cosine terms come from the same sine
    // table at a quarter-period offset rather than needing a second lookup.
    const int quarter = sineLutSize / 4;

    for (int h = 1; h <= harmonics; ++h)
    {
        const float sinAmp = harmonicAmp (shape, h);
        const float cosAmp = harmonicAmpCos (shape, h);

        if (sinAmp == 0.0f && cosAmp == 0.0f)
            continue;

        // table[i] += sinAmp * sin(2*pi*h*i/N) + cosAmp * cos(2*pi*h*i/N)
        const int step = (h * sineLutSize) / tableSize;
        const int wrap = sineLutSize;

        for (int i = 0; i < tableSize; ++i)
        {
            const int idx = (i * step) % wrap;

            if (sinAmp != 0.0f)
                table[static_cast<size_t> (i)] += sinAmp * sineLut[static_cast<size_t> (idx)];

            if (cosAmp != 0.0f)
                table[static_cast<size_t> (i)] += cosAmp * sineLut[static_cast<size_t> ((idx + quarter) % wrap)];
        }
    }
}

OpWaveTables::Cursor OpWaveTables::makeCursor (float wave01, double frequency) const noexcept
{
    Cursor c;

    if (! prepared || tables[0][0].empty())
        return c;

    int mip = 0;

    if (frequency > kBaseFreq)
        mip = static_cast<int> (std::floor (std::log2 (frequency / kBaseFreq)));

    mip = juce::jlimit (0, numMips - 1, mip);

    const float w = juce::jlimit (0.0f, 1.0f, wave01) * static_cast<float> (numShapes - 1);
    const int s0 = juce::jlimit (0, numShapes - 1, static_cast<int> (w));
    const int s1 = juce::jmin (numShapes - 1, s0 + 1);

    c.lo = tables[static_cast<size_t> (mip)][static_cast<size_t> (s0)].data();
    c.hi = tables[static_cast<size_t> (mip)][static_cast<size_t> (s1)].data();
    c.frac = w - static_cast<float> (s0);

    return c;
}

float OpWaveTables::read (const Cursor& c, float phase01) noexcept
{
    // Tables not built yet: fall back to a plain sine rather than silence.
    if (c.lo == nullptr)
        return std::sin (2.0f * kPi * phase01);

    const float x = phase01 * static_cast<float> (tableSize);
    const int i0 = static_cast<int> (x) & (tableSize - 1);
    const int i1 = (i0 + 1) & (tableSize - 1);
    const float fx = x - std::floor (x);

    const float va = c.lo[i0] + (c.lo[i1] - c.lo[i0]) * fx;

    if (c.frac <= 0.0f)
        return va;

    const float vb = c.hi[i0] + (c.hi[i1] - c.hi[i0]) * fx;
    return va + (vb - va) * c.frac;
}

float OpWaveTables::sample (float phase01, float wave01, double frequency) const noexcept
{
    return read (makeCursor (wave01, frequency), phase01);
}

const char* OpWaveTables::shapeName (float wave01)
{
    static const char* const names[] = { "Sine", "Triangle", "Saw", "Square",
                                         "Pulse 25%", "Pulse 12%", "Pulse 6%" };

    const int i = juce::jlimit (0, numShapes - 1,
                                static_cast<int> (std::lround (
                                    juce::jlimit (0.0f, 1.0f, wave01)
                                      * static_cast<float> (numShapes - 1))));

    return names[i];
}
}
