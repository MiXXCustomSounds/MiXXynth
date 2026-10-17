// M8 FX measurement harness: the instrument, its own checks, and the plugin measurements.
//
// The self-test is the point of this file. Every measurement in Analysis.cpp is checked against
// a signal whose answer is known in closed form BEFORE the harness is pointed at anything whose
// behaviour is unknown, because the failure being guarded against is not a crash - it is a
// plausible number that is wrong, which would then be used to justify an implementation.
//
//   MiXXynthSmoke        - DSP, no host
//   MiXXynthEditorAudit  - every control wired to what it should be
//   MiXXynthPatchAudit   - the patch: bank, presets, programs, roll
//   MiXXynthFxMeasure    - the measuring instrument, and what it measures
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/FxMeasure/Analysis.h"
#include "../Source/FxMeasure/PluginHost.h"
#include "../Source/FxMeasure/TestSignals.h"

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& name, const juce::String& detail = {})
    {
        if (! condition)
            ++failures;

        std::printf ("%s %-52s %s\n", condition ? "PASS" : "FAIL", name.toRawUTF8(),
                     detail.toRawUTF8());

        // Unbuffered: a crash would otherwise discard everything printed so far, which makes a
        // failure impossible to locate.
        std::fflush (stdout);
    }

    juce::String db (double value) { return juce::String (value, 3) + " dB"; }
    juce::String ms (double value) { return juce::String (value, 3) + " ms"; }

    // A copy of `dry` delayed by a delay that moves sinusoidally, which is what a chorus is.
    // Built here rather than in TestSignals because it is only ever a self-test input.
    juce::AudioBuffer<float> makeModulated (const juce::AudioBuffer<float>& dry, double sampleRate,
                                            double centreMs, double depthMs, double rateHz)
    {
        using namespace mixxynth::fxmeasure;

        FractionalDelay delay;
        delay.prepare ((int) std::ceil ((centreMs + depthMs) * 0.001 * sampleRate) + 8);

        juce::AudioBuffer<float> wet (1, dry.getNumSamples());
        const auto* d = dry.getReadPointer (0);
        auto* w = wet.getWritePointer (0);

        for (int i = 0; i < dry.getNumSamples(); ++i)
        {
            delay.write (d[i]);

            const double t = (double) i / sampleRate;
            const double currentMs = centreMs
                                     + depthMs * std::sin (juce::MathConstants<double>::pi * 2.0
                                                           * rateHz * t);
            w[i] = delay.read ((float) (currentMs * 0.001 * sampleRate));
        }

        return wet;
    }
}

int runSelfTest()
{
    using namespace mixxynth::fxmeasure;

    constexpr double sr = 48000.0;
    constexpr int channels = 1;

    // Two seconds: long enough for real resolution at 20 Hz, short enough that the whole
    // self-test stays a few seconds.
    const auto sweep = makeLogSweep (sr, 2.0, 20.0, 20000.0);
    const int n = sweep.numSamples();

    // Runs one processing step over the sweep and hands back what came out. The response is
    // measured from that buffer rather than from a reconstructed impulse response, because that
    // is where the accuracy is - see frequencyResponseFromSweep.
    const auto through = [&sweep, n] (const ProcessFn& fn)
    {
        juce::AudioBuffer<float> buffer (1, n);
        buffer.copyFrom (0, 0, sweep.signal, 0, 0, n);
        fn (buffer);
        return buffer;
    };

    const auto responseAt = [&sweep] (const juce::AudioBuffer<float>& recorded, double hz)
    {
        const auto points = frequencyResponseFromSweep (sweep, recorded, { hz });
        return points.empty() ? -300.0 : points.front().db;
    };

    std::printf ("\n--- the analysis, against answers known in closed form ---\n");

    // 1-2. Absolute level. A deconvolution that is merely proportional passes every shape check
    // and fails only these, and it would put a silent constant offset on every measurement the
    // harness ever takes.
    const auto gainUp = through ([] (juce::AudioBuffer<float>& b) { b.applyGain (2.0f); });
    const auto gainDown = through ([] (juce::AudioBuffer<float>& b) { b.applyGain (0.5f); });

    const double upDb = responseAt (gainUp, 1000.0);
    const double downDb = responseAt (gainDown, 1000.0);

    check (std::abs (upDb - 6.0206) < 0.02, "sweep recovers a +6.02 dB gain", db (upDb));
    check (std::abs (downDb + 6.0206) < 0.02, "sweep recovers a -6.02 dB gain", db (downDb));

    // 3-5. A known filter, against its closed form rather than against itself.
    OnePole lowpass;
    lowpass.sampleRate = sr;
    lowpass.cutoffHz = 1000.0;

    const auto lowpassResponse = through ([&lowpass] (juce::AudioBuffer<float>& b)
                                          {
                                              lowpass.reset();
                                              lowpass.process (b);
                                          });

    const double atCutoff = responseAt (lowpassResponse, 1000.0);
    const double analyticCutoff = lowpass.magnitudeDb (1000.0);

    check (std::abs (atCutoff - analyticCutoff) < 0.05,
           "one-pole: matches its closed form at cutoff",
           db (atCutoff) + " vs " + db (analyticCutoff));
    check (std::abs (atCutoff + 3.0103) < 0.1, "one-pole: -3 dB at cutoff", db (atCutoff));

    const double at20 = responseAt (lowpassResponse, 20.0);
    check (std::abs (at20) < 0.2, "one-pole: ~0 dB at 20 Hz", db (at20));

    // Compared against the closed form across the band rather than against a slope. A slope
    // check has to pick two frequencies, and the obvious pair straddles the top of the sweep
    // where the measurement is not trustworthy - so it would fail for a reason that has nothing
    // to do with the filter.
    double worstInBand = 0.0;
    double worstHz = 0.0;

    for (const double hz : { 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const double difference = std::abs (responseAt (lowpassResponse, hz)
                                            - lowpass.magnitudeDb (hz));

        if (difference > worstInBand)
        {
            worstInBand = difference;
            worstHz = hz;
        }
    }

    check (worstInBand < 0.05, "one-pole: matches its closed form across the band",
           juce::String (worstInBand, 4) + " dB worst, at " + juce::String (worstHz, 0) + " Hz");

    // 6. Two independent methods must agree. The tone method shares no code with the sweep
    // method, so agreement here is evidence rather than a tautology.
    const auto freqs = logFrequencies (50.0, 15000.0, 12);
    const auto bySweep = frequencyResponseFromSweep (sweep, lowpassResponse, freqs);
    const auto byTone = frequencyResponseByTone (freqs, sr, channels, 0.25, 60, 0.25f,
                                                 [&lowpass] (juce::AudioBuffer<float>& b)
                                                 {
                                                     lowpass.reset();
                                                     lowpass.process (b);
                                                 });

    double worstDifference = 0.0;

    for (size_t i = 0; i < freqs.size(); ++i)
        worstDifference = juce::jmax (worstDifference, std::abs (bySweep[i].db - byTone[i].db));

    check (worstDifference < 0.5, "sweep and tone methods agree within 0.5 dB",
           juce::String (worstDifference, 3) + " dB worst case");

    // 7. A known latency: the response peak sits where the delay is.
    constexpr int delaySamples = 128;

    juce::AudioBuffer<float> delayed (1, n + delaySamples);
    delayed.clear();
    delayed.copyFrom (0, delaySamples, sweep.signal, 0, 0, n);
    const auto delayedIr = deconvolve (sweep, delayed);

    check (std::abs (delayedIr.peakIndex - delaySamples) <= 1,
           "impulse response peak at a known 128-sample delay",
           "peak at " + juce::String (delayedIr.peakIndex));

    // 8-9. Known nulls.
    juce::AudioBuffer<float> dry (1, 8192), identical (1, 8192), half (1, 8192);
    fillSine (dry, 8192, sr, 1000.0, 0.5f);
    identical.copyFrom (0, 0, dry, 0, 0, 8192);
    half.copyFrom (0, 0, dry, 0, 0, 8192);
    half.applyGain (0.5f);

    const double identicalNull = nullDepthDb (dry, identical);
    const double halfNull = nullDepthDb (dry, half);

    check (identicalNull < -120.0, "null: identical signals are silent", db (identicalNull));
    check (std::abs (halfNull + 6.0206) < 0.05, "null: half amplitude reads -6.02 dB",
           db (halfNull));

    // 10-11. A known nonlinearity. tanh(3x)/3 rather than plain tanh, so the small-signal gain
    // is 1 and the low-level check means something: with a gain of 3 baked in, "linear at low
    // level" would be measuring the gain rather than the linearity.
    std::vector<double> levels;

    for (double level = -60.0; level <= 0.5; level += 6.0)
        levels.push_back (level);

    const auto linearCurve = transferCurve (levels, sr, channels, 1000.0, 0.05, 40,
                                            [] (juce::AudioBuffer<float>& b)
                                            {
                                                b.applyGain (1.0f);
                                            });

    const double curveSlope = (linearCurve.back().outDb - linearCurve.front().outDb)
                              / (linearCurve.back().inDb - linearCurve.front().inDb);

    check (std::abs (curveSlope - 1.0) < 0.01, "transfer curve: a linear gain has slope 1",
           juce::String (curveSlope, 4));

    const auto saturated = transferCurve (levels, sr, channels, 1000.0, 0.05, 40,
                                          [] (juce::AudioBuffer<float>& b)
                                          {
                                              auto* d = b.getWritePointer (0);

                                              for (int i = 0; i < b.getNumSamples(); ++i)
                                                  d[i] = std::tanh (3.0f * d[i]) / 3.0f;
                                          });

    bool monotonic = true;

    for (size_t i = 1; i < saturated.size(); ++i)
        if (saturated[i].outDb < saturated[i - 1].outDb - 0.01)
            monotonic = false;

    check (monotonic, "transfer curve: the saturator is monotonic");

    const double smallSignal = saturated.front().outDb - saturated.front().inDb;
    const double topSlope = (saturated.back().outDb - saturated[saturated.size() - 2].outDb) / 6.0;

    check (std::abs (smallSignal) < 0.5, "transfer curve: linear at low level", db (smallSignal));
    check (topSlope < 0.7, "transfer curve: compresses at high level",
           "top slope " + juce::String (topSlope, 3));

    // 12. A known gain envelope: -6 dB applied after half a second.
    const int grLength = (int) sr;
    juce::AudioBuffer<float> grDry (1, grLength), grWet (1, grLength);
    fillSine (grDry, grLength, sr, 220.0, 0.5f);
    grWet.copyFrom (0, 0, grDry, 0, 0, grLength);
    grWet.applyGain (0, (int) (sr * 0.5), 0.5f);

    const auto reduction = gainReduction (grDry, grWet, sr, 5.0);

    double earlyDb = 0.0, lateDb = 0.0;
    int earlyCount = 0, lateCount = 0;

    for (const auto& point : reduction)
    {
        if (point.seconds < 0.4) { earlyDb += point.db; ++earlyCount; }
        else if (point.seconds > 0.6) { lateDb += point.db; ++lateCount; }
    }

    earlyDb /= juce::jmax (1, earlyCount);
    lateDb /= juce::jmax (1, lateCount);

    check (std::abs (earlyDb + 6.0206) < 0.2, "gain reduction: a -6 dB step reads -6 dB",
           db (earlyDb));
    check (std::abs (lateDb) < 0.2, "gain reduction: unity reads 0 dB", db (lateDb));

    // 13-14. A known modulation. Noise rather than a tone: a periodic signal has an ambiguous
    // correlation peak, and the tracker would lock onto whichever period the lag window found
    // first rather than onto the delay.
    const int modLength = (int) (sr * 3.0);
    juce::AudioBuffer<float> modDry (1, modLength);
    juce::Random rng (20261016);
    fillNoise (modDry, modLength, rng, 0.5f);

    const auto modWet = makeModulated (modDry, sr, 3.5, 1.5, 2.5);

    // A 25 ms window and a 5 ms hop. The window has to be short enough that the delay does not
    // move much while it is being measured: 50 ms would average 1.2 ms of movement into every
    // point and read the depth low.
    const auto track = trackDelaySamples (modDry, modWet, (int) (sr * 0.025), (int) (sr * 0.005),
                                          (int) (sr * 0.007));
    const auto modulation = modulationFromTrack (track, sr, 0.005);

    double trackLo = 0.0, trackHi = 0.0;
    int measuredPoints = 0;

    for (auto value : track)
    {
        if (! std::isfinite (value))
            continue;

        if (measuredPoints == 0)
        {
            trackLo = (double) value;
            trackHi = (double) value;
        }

        trackLo = juce::jmin (trackLo, (double) value);
        trackHi = juce::jmax (trackHi, (double) value);
        ++measuredPoints;
    }

    check (modulation.valid, "modulation: a trajectory was tracked",
           juce::String (measuredPoints) + " of " + juce::String ((int) track.size())
               + " points, " + ms (trackLo / sr * 1000.0) + " to " + ms (trackHi / sr * 1000.0));

    // A still delay, to separate the tracker from the modulation: if this reads right and the
    // modulated case does not, the error is in how a MOVING delay is tracked rather than in
    // where the tracker puts a fixed one.
    const auto stillWet = makeModulated (modDry, sr, 3.5, 0.0, 2.5);
    const auto stillTrack = trackDelaySamples (modDry, stillWet, (int) (sr * 0.025),
                                               (int) (sr * 0.005), (int) (sr * 0.007));
    const auto still = modulationFromTrack (stillTrack, sr, 0.005);

    check (std::abs (still.centreMs - 3.5) < 0.05,
           "modulation: a still delay tracks where it is", ms (still.centreMs));
    check (std::abs (modulation.rateHz - 2.5) < 0.1, "modulation: rate within 0.1 Hz",
           juce::String (modulation.rateHz, 3) + " Hz");
    check (std::abs (modulation.depthMs - 1.5) < 0.15, "modulation: depth within 0.15 ms",
           ms (modulation.depthMs));
    check (std::abs (modulation.centreMs - 3.5) < 0.2, "modulation: centre within 0.2 ms",
           ms (modulation.centreMs));

    std::printf ("\n");
    return failures;
}

// Loads each of the plugin's VST3 programs in turn and reports how much of the dry signal
// survives.
//
// A program that does nothing reads as a perfect null; a program with an effect in it does not.
// That makes this a way to find a preset by BEHAVIOUR, which is what is needed when a plugin
// gives its programs placeholder names - Serum 2 FX has 128 of them, every one called "Prog N" -
// and when its preset file format is its own business rather than something to be parsed here.
int scanPrograms (mixxynth::fxmeasure::PluginHost& host, double sr, int channels)
{
    using namespace mixxynth::fxmeasure;

    auto* processor = host.processor();

    if (processor == nullptr)
        return 1;

    const int numPrograms = processor->getNumPrograms();

    std::printf ("\nprogram scan (%d programs), null depth against the dry signal:\n", numPrograms);
    std::printf ("  %4s  %-26s %10s\n", "prog", "name", "null dB");

    const int probeLength = (int) (sr * 0.5);

    juce::AudioBuffer<float> dryProbe (channels, probeLength);
    fillSine (dryProbe, probeLength, sr, 440.0, 0.25f);

    int interesting = 0;

    for (int i = 0; i < numPrograms; ++i)
    {
        processor->setCurrentProgram (i);

        juce::AudioBuffer<float> wetProbe (channels, probeLength);

        for (int ch = 0; ch < channels; ++ch)
            wetProbe.copyFrom (ch, 0, dryProbe, ch, 0, probeLength);

        host.process (wetProbe);

        const double depth = nullDepthDb (dryProbe, wetProbe);
        const bool doesSomething = depth > -100.0;

        if (doesSomething)
            ++interesting;

        std::printf ("  %4d  %-26s %10.2f%s\n", i, processor->getProgramName (i).toRawUTF8(),
                     depth, doesSomething ? "   <-- changes the signal" : "");
    }

    std::printf ("\n%d of %d programs change the signal.\n", interesting, numPrograms);
    return 0;
}

// What to print about a plugin's parameters, instead of measuring it.
//
// A plugin with thousands of parameters is not something to read through, and finding the ones
// a recipe needs is the step before driving them. Matching by name is the usual way; a range is
// for when the names are all the same shape and only the positions say anything - which is
// exactly how Serum 2's FX slots present themselves.
struct ParameterDump
{
    juce::String findPattern;
    int rangeStart = -1;
    int rangeCount = 0;
    bool listClassesOnly = false;
    bool scanPrograms = false;
};

// Where a plugin's state comes from and goes to.
//
// The state is an opaque blob as far as this tool is concerned: it is whatever the plugin writes
// and reads through the standard VST3 calls, handed straight back, exactly as a host does. That
// matters because some plugins keep their patch in their state rather than in their parameters -
// Serum 2 FX does, and none of its parameters says which effect is loaded - so a preset can only
// be brought in through the plugin's own interface. Doing it this way means the preset's file
// format never has to be understood here at all.
struct StateOptions
{
    juce::String loadPath;  // set on the plugin before measuring
    juce::String savePath;  // written after configuring
    bool configure = false; // open the plugin's own window first
};

// A window holding a plugin's own editor, so the plugin can be configured by hand.
class EditorWindow final : public juce::DocumentWindow
{
public:
    EditorWindow (juce::AudioProcessor& processor, std::function<void()> onClosed)
        : juce::DocumentWindow (processor.getName(), juce::Colours::black,
                                juce::DocumentWindow::closeButton),
          closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);

        // setContentOwned is right even though createEditorIfNeeded() hands back an editor the
        // processor also knows about: the editor's destructor calls editorBeingDeleted() and the
        // processor forgets it. Doing it the other way round deletes it twice.
        setContentOwned (processor.createEditorIfNeeded(), true);

        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        if (closed != nullptr)
            closed();
    }

private:
    std::function<void()> closed;
};

int measurePlugin (const juce::String& path, const juce::String& classSelector,
                   const ParameterDump& dump, const StateOptions& state)
{
    using namespace mixxynth::fxmeasure;

    const juce::File bundle (path);

    if (! bundle.exists())
    {
        std::printf ("No such plugin bundle: %s\n", path.toRawUTF8());
        return 1;
    }

    // Which classes the bundle declares, listed before anything is loaded. A VST3 bundle can
    // declare several and they are not interchangeable - Serum 2 declares both an instrument
    // and an effect from one binary, with different buses and different parameters - so this is
    // printed rather than assumed.
    juce::String listError;
    const auto classes = PluginHost::listClasses (bundle, listError);

    if (classes.empty())
    {
        std::printf ("%s\n  %s\n", path.toRawUTF8(), listError.toRawUTF8());
        return 1;
    }

    std::printf ("\n%s\n", path.toRawUTF8());
    std::printf ("  declares %d class(es):\n", (int) classes.size());

    for (const auto& entry : classes)
        std::printf ("    [%d] %-28s %d in / %d out%s\n", entry.index, entry.name.toRawUTF8(),
                     entry.numInputChannels, entry.numOutputChannels,
                     entry.isInstrument ? "   (instrument)" : "");

    if (dump.listClassesOnly)
        return 0;

    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    constexpr int channels = 2;

    PluginHost host;

    if (! host.load (bundle, sr, blockSize, channels, classSelector))
    {
        std::printf ("\nCould not load: %s\n", host.getInfo().error.toRawUTF8());
        return 1;
    }

    // State before anything is measured, so what gets measured is the patch that was loaded.
    if (state.loadPath.isNotEmpty())
    {
        juce::MemoryBlock captured;

        if (! juce::File (state.loadPath).loadFileAsData (captured))
        {
            std::printf ("Could not read the state file: %s\n", state.loadPath.toRawUTF8());
            return 1;
        }

        host.processor()->setStateInformation (captured.getData(), (int) captured.getSize());
        std::printf ("\nstate loaded : %s  (%d bytes)\n", state.loadPath.toRawUTF8(),
                     (int) captured.getSize());
    }

    // A save without --configure just captures what the plugin is in now, which is how the
    // round trip is checked: save the default state, load it back, and the measurement must not
    // move.
    if (state.savePath.isNotEmpty() && ! state.configure)
    {
        juce::MemoryBlock captured;
        host.processor()->getStateInformation (captured);

        if (! juce::File (state.savePath).replaceWithData (captured.getData(), captured.getSize()))
        {
            std::printf ("Could not write %s\n", state.savePath.toRawUTF8());
            return 1;
        }

        std::printf ("state saved  : %s  (%d bytes)\n", state.savePath.toRawUTF8(),
                     (int) captured.getSize());
    }

    const auto& info = host.getInfo();

    std::printf ("\n=== %s ===\n", info.name.toRawUTF8());
    std::printf ("class        : %s  (%d of %d)\n", info.className.toRawUTF8(), info.classIndex,
                 info.numClasses);
    std::printf ("manufacturer : %s\n", info.manufacturer.toRawUTF8());
    std::printf ("version      : %s\n", info.version.toRawUTF8());
    std::printf ("format       : %s\n", info.format.toRawUTF8());
    std::printf ("channels     : %d in / %d out\n", info.numInputChannels, info.numOutputChannels);
    std::printf ("parameters   : %d\n", info.numParameters);
    std::printf ("latency      : %d samples (%.3f ms at %.0f Hz)\n", info.reportedLatencySamples,
                 (double) info.reportedLatencySamples / sr * 1000.0, sr);

    // VST3 programs, when the plugin publishes them. A plugin that exposes its factory presets
    // this way can be pointed at one by name, which is a way into a preset-driven plugin that
    // needs no state chunk and no editor.
    if (auto* processor = host.processor())
    {
        const int numPrograms = processor->getNumPrograms();

        std::printf ("programs     : %d\n", numPrograms);

        for (int i = 0; i < juce::jmin (24, numPrograms); ++i)
            std::printf ("    [%d] %s\n", i, processor->getProgramName (i).toRawUTF8());

        if (numPrograms > 24)
            std::printf ("    ... and %d more\n", numPrograms - 24);
    }

    if (state.configure)
    {
        auto* processor = host.processor();

        if (processor == nullptr || ! processor->hasEditor())
        {
            std::printf ("\nThis plugin has no editor to configure it with.\n");
            return 1;
        }

        if (state.savePath.isEmpty())
        {
            std::printf ("\n--configure needs --save-state <file> to know where to put the state.\n");
            return 1;
        }

        std::printf ("\nLoad the preset in the plugin's own window and set it up, then close the\n");
        std::printf ("window. The state is then written to %s\n\n", state.savePath.toRawUTF8());

        {
            auto window = std::make_unique<EditorWindow> (
                *processor, [] { juce::MessageManager::getInstance()->stopDispatchLoop(); });

            juce::MessageManager::getInstance()->runDispatchLoop();
        }

        juce::MemoryBlock captured;
        processor->getStateInformation (captured);

        if (! juce::File (state.savePath).replaceWithData (captured.getData(), captured.getSize()))
        {
            std::printf ("Could not write %s\n", state.savePath.toRawUTF8());
            return 1;
        }

        std::printf ("state saved  : %s  (%d bytes)\n", state.savePath.toRawUTF8(),
                     (int) captured.getSize());
        return 0;
    }

    if (dump.scanPrograms)
        return scanPrograms (host, sr, channels);

    // Measured in whatever state it loads in. Driving the parameters is a recipe per effect and
    // does not exist yet, so this is the honest starting point rather than a measurement of one
    // named effect.
    const auto sweep = makeLogSweep (sr, 3.0, 20.0, 20000.0);
    const int n = sweep.numSamples();

    juce::AudioBuffer<float> buffer (channels, n);

    for (int ch = 0; ch < channels; ++ch)
        buffer.copyFrom (ch, 0, sweep.signal, 0, 0, n);

    host.process (buffer);

    const auto ir = deconvolve (sweep, buffer);

    if (! ir.isValid())
    {
        std::printf ("\nThe response could not be deconvolved.\n");
        return 1;
    }

    std::printf ("\nir peak      : index %d (%.1f ms)\n", ir.peakIndex,
                 (double) ir.peakIndex / sr * 1000.0);

    std::printf ("\n%-10s %9s\n", "Hz", "dB");

    for (const auto& point : frequencyResponseFromSweep (sweep, buffer,
                                                         logFrequencies (20.0, 20000.0, 24)))
        std::printf ("%-10.1f %9.2f\n", point.hz, point.db);

    // A null test: how much of the dry signal survives the plugin.
    const int nullLength = (int) sr;
    juce::AudioBuffer<float> nullDry (channels, nullLength), nullWet (channels, nullLength);
    fillSine (nullDry, nullLength, sr, 440.0, 0.25f);

    for (int ch = 0; ch < channels; ++ch)
        nullWet.copyFrom (ch, 0, nullDry, ch, 0, nullLength);

    host.process (nullWet);

    std::printf ("\nnull depth   : %.2f dB (440 Hz, dry against wet)\n",
                 nullDepthDb (nullDry, nullWet));

    // The parameter list, because driving these is the next step and this is what it needs.
    if (auto* processor = host.processor())
    {
        const auto& parameters = processor->getParameters();

        if (dump.rangeStart >= 0)
        {
            const int first = juce::jmax (0, dump.rangeStart);
            const int last = juce::jmin (parameters.size(),
                                         first + juce::jmax (1, dump.rangeCount));

            std::printf ("\nparameters %d to %d:\n", first, last - 1);

            for (int i = first; i < last; ++i)
                std::printf ("  %4d  %-44s %7.4f  %s\n", i,
                             parameters[i]->getName (44).toRawUTF8(),
                             parameters[i]->getValue(),
                             parameters[i]->getCurrentValueAsText().toRawUTF8());
        }
        else if (dump.findPattern.isNotEmpty())
        {
            // A plugin with thousands of parameters is not something to read through, so the
            // way in is to search for the ones a recipe will need by name.
            std::printf ("\nparameters matching \"%s\":\n", dump.findPattern.toRawUTF8());

            int shown = 0;

            for (int i = 0; i < parameters.size(); ++i)
            {
                if (! parameters[i]->getName (64).containsIgnoreCase (dump.findPattern))
                    continue;

                std::printf ("  %4d  %-44s %7.4f  %s\n", i,
                             parameters[i]->getName (44).toRawUTF8(),
                             parameters[i]->getValue(),
                             parameters[i]->getCurrentValueAsText().toRawUTF8());
                ++shown;
            }

            std::printf ("  %d match(es)\n", shown);
        }
        else
        {
            std::printf ("\nparameters (first 40 of %d):\n", parameters.size());

            for (int i = 0; i < juce::jmin (40, parameters.size()); ++i)
                std::printf ("  %3d  %-42s %7.4f  %s\n", i,
                             parameters[i]->getName (42).toRawUTF8(),
                             parameters[i]->getValue(),
                             parameters[i]->getCurrentValueAsText().toRawUTF8());
        }
    }

    std::printf ("\n");
    return 0;
}

void printUsage (const char* executable)
{
    std::printf ("MiXXynth FX measurement harness\n\n");
    std::printf ("  %s --selftest\n", executable);
    std::printf ("      Checks the analysis against signals whose answers are known in closed\n");
    std::printf ("      form. Run this before trusting any measurement taken with it.\n\n");
    std::printf ("  %s --plugin <path to .vst3>\n", executable);
    std::printf ("      Loads a VST3 by absolute path and measures it. Black box: audio in,\n");
    std::printf ("      audio out, nothing else. See docs/FX-MEASUREMENT.md.\n\n");
    std::printf ("  %s --plugin <path to .vst3> --list\n", executable);
    std::printf ("      Lists the classes the bundle declares and stops. A VST3 bundle can\n");
    std::printf ("      declare several - Serum 2 declares an instrument and an effect from one\n");
    std::printf ("      binary - and they are not interchangeable.\n\n");
    std::printf ("  %s --plugin <path to .vst3> --class <name or index>\n", executable);
    std::printf ("      Measures that class. Without it the first class the format reports is used,\n");
    std::printf ("      which may not be the one intended - and a wrong pick measures silence.\n\n");
    std::printf ("  %s --plugin <path to .vst3> --find <text>\n", executable);
    std::printf ("      Lists the parameters whose names contain <text> instead of measuring. A\n");
    std::printf ("      plugin with thousands of parameters is not something to read through, and\n");
    std::printf ("      finding the ones a recipe needs is the step before driving them.\n\n");
    std::printf ("  %s --plugin <path to .vst3> --range <first> <count>\n", executable);
    std::printf ("      Lists a range of parameters by index, for plugins whose parameter names\n");
    std::printf ("      are all the same shape and only the positions say anything.\n\n");
    std::printf ("  %s --plugin <path to .vst3> --scan-programs\n", executable);
    std::printf ("      Loads each VST3 program in turn and reports how much of the dry signal\n");
    std::printf ("      survives, which finds a preset by behaviour when the plugin names its\n");
    std::printf ("      programs nothing useful.\n\n");
    std::printf ("  %s --plugin <path> --class <name> --configure --save-state <file>\n", executable);
    std::printf ("      Opens the plugin's OWN window. Load the preset there and set it up, then\n");
    std::printf ("      close the window, and its state is written to <file>. This is how a plugin\n");
    std::printf ("      whose patch lives in its state rather than its parameters is configured -\n");
    std::printf ("      the plugin reads its own preset format, and the file that comes back is\n");
    std::printf ("      the standard VST3 state blob, never interpreted here.\n\n");
    std::printf ("  %s --plugin <path> --class <name> --load-state <file>\n", executable);
    std::printf ("      Loads that state before measuring, so the measurement is of that patch.\n\n");
    std::printf ("  %s --all\n", executable);
    std::printf ("      The self-test, then the standard targets.\n");
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    bool selfTest = false;
    bool all = false;
    juce::String classSelector;
    ParameterDump dump;
    StateOptions state;
    juce::StringArray plugins;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String argument (argv[i]);

        if (argument == "--selftest")
            selfTest = true;
        else if (argument == "--all")
            all = true;
        else if (argument == "--plugin" && i + 1 < argc)
            plugins.add (argv[++i]);
        else if (argument == "--find" && i + 1 < argc)
            dump.findPattern = argv[++i];
        else if (argument == "--range" && i + 2 < argc)
        {
            dump.rangeStart = juce::String (argv[++i]).getIntValue();
            dump.rangeCount = juce::String (argv[++i]).getIntValue();
        }
        else if (argument == "--class" && i + 1 < argc)
            classSelector = argv[++i];
        else if (argument == "--list")
            dump.listClassesOnly = true;
        else if (argument == "--scan-programs")
            dump.scanPrograms = true;
        else if (argument == "--configure")
            state.configure = true;
        else if (argument == "--save-state" && i + 1 < argc)
            state.savePath = argv[++i];
        else if (argument == "--load-state" && i + 1 < argc)
            state.loadPath = argv[++i];
        else
        {
            printUsage (argv[0]);
            return 2;
        }
    }

    if (all)
    {
        selfTest = true;
        plugins.add ("C:/Program Files/Common Files/VST3/Serum2.vst3");
        plugins.add ("C:/Program Files/Common Files/VST3/SerumFX.vst3");

        // Which class to measure is a decision, not a default. With no --class given, --all
        // lists the classes instead of measuring one it picked itself - because the wrong pick
        // measures silence, and silence looks like a result.
        if (classSelector.isEmpty())
            dump.listClassesOnly = true;
    }

    if (! selfTest && plugins.isEmpty())
    {
        printUsage (argv[0]);
        return 2;
    }

    int result = 0;

    if (selfTest)
    {
        const int selfTestFailures = runSelfTest();

        if (selfTestFailures == 0)
            std::printf ("FX MEASURE SELF-TEST OK\n\n");
        else
            std::printf ("FX MEASURE SELF-TEST FAILED: %d check(s)\n\n", selfTestFailures);

        result = selfTestFailures == 0 ? 0 : 1;
    }

    for (const auto& path : plugins)
        if (measurePlugin (path, classSelector, dump, state) != 0)
            result = 1;

    return result;
}
