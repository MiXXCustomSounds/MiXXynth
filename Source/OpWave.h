#pragma once

#include <array>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

// Band-limited operator waveforms with continuous morphing.
//
// op{n}wave is a single 0..1 control sweeping sine -> triangle -> saw -> square ->
// pulse 25% -> pulse 12% -> pulse 6%. The tail of the sweep narrows the duty, so past
// the square the control acts as a pulse width control rather than a shape selector,
// and the shape knob ends up doing the job a separate PWM knob would have done.
//
// The morph happens in the HARMONIC domain: each shape is synthesised from its
// Fourier series, and the control interpolates between adjacent shapes' tables.
// That keeps the shapes phase-coherent, so morphing never produces the
// cancellation notches a time-domain crossfade gives between, e.g., a saw and a
// square. It is also what makes the pulse stops interpolate smoothly: they are the
// same series at different duties, so sweeping between them sweeps the duty.
//
// One table per shape per octave band, so a high-ratio operator does not alias.
//
// The tables are SHARED: one instance is owned by the processor and handed to
// every voice. numMips x numShapes x tableSize is ~256 KB, and giving each of 64
// voices its own copy would be ~16 MB.
namespace mixxynth
{
    class OpWaveTables
    {
    public:
        // sine, triangle, saw, square, then a pulse that narrows: 25%, 12%, 6%.
        // The narrowing stops are the same Fourier series at different duties, which
        // is exactly why the top of the sweep reads as a pulse width control.
        static constexpr int numShapes = 7;
        static constexpr int numMips   = 8;    // one per octave from A0 up
        static constexpr int tableSize = 2048; // power of two: index masks, not modulo

        // Built once from prepareToPlay. Not real-time safe, and never called
        // from the audio thread.
        void prepare (double sampleRate);

        // Per-block setup: resolves the mip band and the morph pair once, so the
        // per-sample path is two table reads and a lerp. A null `lo` means the
        // tables are not built yet and the caller should fall back to a sine.
        struct Cursor
        {
            const float* lo = nullptr;
            const float* hi = nullptr;
            float frac = 0.0f;
        };

        Cursor makeCursor (float wave01, double frequency) const noexcept;

        // phase01 must already be wrapped into 0..1 by the caller.
        static float read (const Cursor& c, float phase01) noexcept;

        // Convenience for one-off reads (tests, meters).
        float sample (float phase01, float wave01, double frequency) const noexcept;

        // Name of the shape nearest the control position, for knob labels.
        static const char* shapeName (float wave01);

        bool isPrepared() const noexcept { return prepared; }

    private:
        static constexpr int sineLutSize = 4096;

        void buildShape (int shape, int mip, int harmonics);

        // [mip][shape][sample]
        std::array<std::array<std::vector<float>, numShapes>, numMips> tables;
        std::array<int, numMips> mipHarmonics {};
        std::vector<float> sineLut;
        double sampleRate = 44100.0;
        bool prepared = false;
    };
}
