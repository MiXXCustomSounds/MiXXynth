#include "FmVoice.h"

namespace
{
    // The only helper the voice needs from outside its own class.
    //
    // This namespace used to hold two more things, and both are gone. `polyBlep` corrected
    // the saw/square discontinuities of the VA's old hand-rolled oscillators; the VA reads
    // the same band-limited morph tables the operators read now, so it has nothing to
    // correct. `twoPi` was left behind when those oscillators went, and was declared but
    // never referenced.
    float midiNoteToFreq (int note)
    {
        return 440.0f * std::pow (2.0f, static_cast<float> (note - 69) / 12.0f);
    }

    // How far an operator's pitch wanders at full drift depth. Wide enough to hear as
    // movement, narrow enough that a tuned patch still reads as in tune.
    constexpr float kMaxDriftCents = 25.0f;

    // Seconds for the drift slew to cover most of the distance to a new target. This is
    // the number that decides whether drift reads as analog instability (seconds) or as
    // vibrato (a fraction of a second). Below about half a second it stops sounding like
    // an oscillator warming up and starts sounding like an LFO.
    constexpr float kDriftSlewSeconds = 0.6f;

    // How far a filter's envelope sweeps its cutoff, in octaves, at full envelope. Four is
    // the usual amount: enough to go from closed to bright without the top of the sweep
    // disappearing past what the ear can follow.
    constexpr float kFilterEnvOctaves = 4.0f;

    // How long the VA layer levels take to reach a new value. A few milliseconds: long enough
    // that no level can step between blocks, short enough to be inaudible as a slew.
    constexpr float kVaLevelSlewSeconds = 0.003f;

    double centsToRatio (double cents)
    {
        return std::pow (2.0, cents / 1200.0);
    }
}

MiXXFmVoice::MiXXFmVoice() = default;

void MiXXFmVoice::setSampleRate (double sr)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
}

bool MiXXFmVoice::isActive() const
{
    if (gate)
        return true;

    for (const auto& op : ops)
        if (op.envState.stage != 0)
            return true;

    // The VA layer has its own envelope and its own release, and nothing above this
    // can see either. Without this the voice is declared finished the moment the
    // OPERATORS' envelopes end, which cuts the VA's release off mid-waveform - an
    // early cut-off and a loud click whenever the VA's release is longer than the
    // operators'. The VA and the sub die together because they share this envelope,
    // which is the signature of the bug.
    //
    // Gated on the layer having been audible rather than on the envelope merely
    // running: the VA envelope runs on every note whether the VA is used or not, so
    // counting it unconditionally would hold every voice open - including a patch
    // with the VA switched off entirely - for the VA's release time.
    if (vaWasAudible && vaEnvState.stage != 0)
        return true;

    return false;
}

void MiXXFmVoice::setDetune (float cents)
{
    // Range covers a 24-semitone pitch bend plus unison spread.
    detuneCents = juce::jlimit (-2400.0f, 2400.0f, cents);

    if (currentNote >= 0)
        frequency = midiNoteToFreq (currentNote) * std::pow (2.0, static_cast<double> (detuneCents) / 1200.0);
}

void MiXXFmVoice::setPan (float pan)
{
    // Constant-power-ish stereo placement; centred leaves both channels at 1.
    const float p = juce::jlimit (-1.0f, 1.0f, pan);
    panL = (p <= 0.0f) ? 1.0f : 1.0f - p;
    panR = (p >= 0.0f) ? 1.0f : 1.0f + p;
}

void MiXXFmVoice::reset()
{
    currentNote = -1;
    gate = false;
    frequency = 440.0;
    velocity = 0.0f;
    vaPhase = 0.0;
    vaSubPhase = 0.0;
    vaEnv = 0.0f;
    noiseState = 0x2545f491u;
    vaDrift = 0.0f;
    vaDriftTarget = 0.0f;
    svfIc1 = 0.0f;
    svfIc2 = 0.0f;
    svfLastCutoff = -1.0f;
    svfLastReso = -1.0f;

    for (auto& op : ops)
    {
        op.phase = 0.0;
        op.envState.env = 0.0f;
        op.envState.stage = 0;
        op.envState.releaseFrom = 0.0f;
        op.prevOut = 0.0f;
        op.curOut = 0.0f;
        op.fbSmooth = 0.0f;
        op.drift = 0.0f;
        op.driftTarget = 0.0f;

        // The filter's own state, for the same reason as the amplitude envelope: a stale
        // envelope or a stale SVF integrator from the last note is a click at the start of
        // this one.
        op.fltEnvState = EnvState {};
        op.fltIc1 = 0.0f;
        op.fltIc2 = 0.0f;
        op.fltLastCutoff = -1.0f;
        op.fltLastReso = -1.0f;
    }
}

float MiXXFmVoice::nextDriftRandom()
{
    // xorshift32. Deterministic from the seed, which is what lets the tests assert that
    // drift moves the pitch at all, and that two voices with different seeds do not move
    // together - neither of which is checkable against a random source.
    driftSeed ^= driftSeed << 13;
    driftSeed ^= driftSeed >> 17;
    driftSeed ^= driftSeed << 5;

    return static_cast<float> (driftSeed & 0xffffffu) / static_cast<float> (0xffffffu);
}

void MiXXFmVoice::updateDrift (const VoiceParams& p, int numSamples)
{
    if (p.drift <= 0.0f)
        return;

    const float blockSeconds = static_cast<float> (numSamples) / static_cast<float> (sampleRate);

    // Two rates, because they do different jobs. The walk decides where the pitch wants
    // to be; the slew decides how quickly it gets there, and is what stops the movement
    // from stepping audibly at block boundaries.
    //
    // Both are scaled by the block length, so the wander's speed does not change when the
    // host changes its buffer size.
    const float walkStep = blockSeconds * 1.5f;
    const float slew = juce::jlimit (0.0f, 1.0f, blockSeconds / kDriftSlewSeconds);

    auto step = [this, walkStep, slew] (float& value, float& target)
    {
        target = juce::jlimit (-1.0f, 1.0f,
                               target + (nextDriftRandom() * 2.0f - 1.0f) * walkStep);
        value += (target - value) * slew;
    };

    for (auto& op : ops)
        step (op.drift, op.driftTarget);

    // The VA walks on its own trajectory, so it detunes against the operators over time
    // rather than moving with them.
    step (vaDrift, vaDriftTarget);
}

void MiXXFmVoice::noteOn (int noteNumber, float vel, float phaseOffset01)
{
    currentNote = noteNumber;
    frequency = midiNoteToFreq (noteNumber) * std::pow (2.0, static_cast<double> (detuneCents) / 1200.0);
    velocity = juce::jlimit (0.0f, 1.0f, vel);
    gate = true;
    vaPhase = 0.0;
    vaSubPhase = 0.0;
    svfIc1 = 0.0f;
    svfIc2 = 0.0f;

    const double startPhase = static_cast<double> (phaseOffset01)
                                - std::floor (static_cast<double> (phaseOffset01));

    // The VA oscillator is scattered too, so a patch that uses it does not stay
    // phase-aligned while the FM operators decorrelate.
    vaPhase = startPhase;
    vaSubPhase = startPhase;

    // The layer levels start at silence and slew up to the block's targets. The envelope is
    // also starting from zero, so this costs nothing audible, and it means a note that begins
    // with the sub already up does not begin with a step - which is the same click, arriving
    // at note-on instead of at the knob.
    vaLevelSm = 0.0f;
    vaSubLevelSm = 0.0f;
    vaNoiseLevelSm = 0.0f;

    // A new note has not made a sound yet, so a stale true here would keep a voice
    // alive for the previous note's VA release.
    vaWasAudible = false;

    for (auto& op : ops)
    {
        op.phase = startPhase;
        op.envState.env = 0.0f;
        op.envState.stage = 1;
        op.envState.releaseFrom = 0.0f;
        op.prevOut = 0.0f;
        op.curOut = 0.0f;
        op.fbSmooth = 0.0f;

        // The filter's envelope starts with the note, so a swept filter sweeps from the
        // note rather than from wherever it happened to be left.
        op.fltEnvState = EnvState {};
        op.fltEnvState.stage = 1;
        op.fltIc1 = 0.0f;
        op.fltIc2 = 0.0f;
    }
}

void MiXXFmVoice::noteOff (int noteNumber)
{
    if (noteNumber != currentNote)
        return;

    gate = false;

    for (auto& op : ops)
    {
        if (op.envState.stage != 0 && op.envState.stage != 4)
        {
            op.envState.stage = 4;
            op.envState.releaseFrom = op.envState.env;
        }

        if (op.fltEnvState.stage != 0 && op.fltEnvState.stage != 4)
        {
            op.fltEnvState.stage = 4;
            op.fltEnvState.releaseFrom = op.fltEnvState.env;
        }
    }
}

float MiXXFmVoice::updateEnvelope (EnvState& e, float a, float d, float s, float r, double sr)
{
    const float sampleRateF = static_cast<float> (sr);

    switch (e.stage)
    {
        case 1: // attack
        {
            e.env += 1.0f / juce::jmax (1.0f, a * sampleRateF);
            if (e.env >= 1.0f) { e.env = 1.0f; e.stage = 2; }
            break;
        }
        case 2: // decay
        {
            e.env -= (1.0f - s) / juce::jmax (1.0f, d * sampleRateF);
            if (e.env <= s) { e.env = s; e.stage = 3; }
            break;
        }
        case 3: // sustain
            e.env = s;
            break;
        case 4: // release
        {
            e.env -= e.releaseFrom / juce::jmax (1.0f, r * sampleRateF);
            if (e.env <= 0.0f) { e.env = 0.0f; e.stage = 0; }
            break;
        }
        default:
            e.env = 0.0f;
            break;
    }

    return e.env;
}

void MiXXFmVoice::renderRange (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& p)
{
    const int numCh = buffer.getNumChannels();

    // Drift first: everything below is derived from the drifted frequencies, so it has to
    // be advanced before any of them is resolved.
    updateDrift (p, numSamples);

    // Velocity, resolved once per block rather than per sample. It used to be applied as a
    // bare `* velocity` on the output, which meant the sensitivity was permanently at 100%
    // and could not be dialled back. velToLevel = 1 is exactly that old behaviour, so the
    // default preserves it and the control only ever reduces it.
    const float velAmount = juce::jlimit (0.0f, 1.0f, velocity);
    const float velScale = 1.0f - p.velToLevel * (1.0f - velAmount);
    const float indexScale = 1.0f - p.velToIndex * (1.0f - velAmount);

    // Resolve each operator's mip band, morph pair AND phase increment once per block.
    // Doing any of it per sample would put a log2 and a table pick in the hot loop. The
    // increment lives here rather than in the loop because drift makes it vary per block,
    // which is also why the loop no longer multiplies frequency by ratio per sample.
    std::array<double, numOps> opInc {};
    std::array<mixxynth::OpWaveTables::Cursor, numOps> cursors;

    for (int i = 0; i < numOps; ++i)
    {
        const double opFreq = frequency * static_cast<double> (p.ratio[i])
                                * centsToRatio (static_cast<double> (ops[static_cast<size_t> (i)].drift)
                                                  * static_cast<double> (p.drift)
                                                  * kMaxDriftCents);

        opInc[static_cast<size_t> (i)] = opFreq / sampleRate;

        if (p.waveTables != nullptr)
            cursors[static_cast<size_t> (i)] = p.waveTables->makeCursor (p.opWave[i], opFreq);
    }

    // The VA runs at a single frequency for the whole range too, so its morph pair and
    // its sub's are resolved here for the same reason the operator cursors are: doing it
    // per sample would put a log2 and a table pick in the hot loop. Its drift adds to its
    // own detune, so the two are separate controls rather than one.
    const double vaFreq = frequency * std::pow (2.0, static_cast<double> (p.vaOctave))
                            * std::pow (2.0, static_cast<double> (p.vaDetune) / 1200.0)
                            * centsToRatio (static_cast<double> (vaDrift)
                                              * static_cast<double> (p.drift)
                                              * kMaxDriftCents);
    const double vaInc = juce::jlimit (0.0, 0.49, vaFreq / sampleRate);
    const double vaSubInc = juce::jlimit (0.0, 0.49, vaFreq * 0.5 / sampleRate);

    // One-pole coefficient for the VA layer levels, about 3 ms. Short enough that a knob still
    // feels immediate, long enough that a square's amplitude cannot step between blocks.
    const float levelSmooth = 1.0f - std::exp (-1.0f / (kVaLevelSlewSeconds
                                                        * static_cast<float> (sampleRate)));

    mixxynth::OpWaveTables::Cursor vaCursor, vaSubCursor;

    if (p.waveTables != nullptr)
    {
        vaCursor = p.waveTables->makeCursor (p.vaShape, vaFreq);

        // The sub stays a square whatever the morph is doing. It is there for weight, and
        // a narrow pulse an octave down is not weight.
        vaSubCursor = p.waveTables->makeCursor (
            3.0f / static_cast<float> (mixxynth::OpWaveTables::numShapes - 1), vaFreq * 0.5);
    }

    for (int n = 0; n < numSamples; ++n)
    {
        float sum = 0.0f;

        // Descending order: every DX7 edge goes from a higher op number to a
        // lower one, so those edges resolve within the current sample.
        for (int dst = numOps - 1; dst >= 0; --dst)
        {
            auto& op = ops[static_cast<size_t> (dst)];

            updateEnvelope (op.envState, p.attack[dst], p.decay[dst], p.sustain[dst], p.release[dst], sampleRate);
            op.fbSmooth += (p.feedback[dst] - op.fbSmooth) * 0.001f;

            double modSum = 0.0;

            for (int src = 0; src < numOps; ++src)
            {
                const float depth = p.mod[src][dst];

                if (depth == 0.0f)
                    continue;

                const auto& srcOp = ops[static_cast<size_t> (src)];
                modSum += static_cast<double> (depth) * (src > dst ? srcOp.curOut : srcOp.prevOut);
            }

            // Self-feedback uses this op's own previous output.
            modSum += static_cast<double> (op.fbSmooth) * op.prevOut;

            // FM modulation is a phase offset, so the waveform is evaluated at
            // the modulated phase. A disabled operator outputs silence, which
            // also stops it modulating anything downstream.
            const double modulated = op.phase + modSum * p.index * indexScale;
            const float ph01 = static_cast<float> (modulated - std::floor (modulated));

            const float raw = p.opOn[dst]
                                ? mixxynth::OpWaveTables::read (cursors[static_cast<size_t> (dst)], ph01)
                                : 0.0f;

            const float amp = raw * op.envState.env;
            op.curOut = amp;

            // The operator's filter, if it has one. Filtered ONCE and then routed, rather
            // than filtered twice for the two paths: it is one filter, so running it twice
            // would mean two sets of state that can disagree about the same signal - and
            // "Both" would not be the sum of the two, it would be a third thing.
            float filtered = amp;

            const bool filterOn = p.opFilterOn[dst];
            const int route = p.opFilterRouting[dst];

            if (filterOn)
            {
                updateEnvelope (op.fltEnvState, p.opFilterAttack[dst], p.opFilterDecay[dst],
                                p.opFilterSustain[dst], p.opFilterRelease[dst], sampleRate);

                // The coefficients are refreshed every 32 samples, not every sample. The
                // envelope itself is cheap (an add and a compare) but a tan() per operator
                // per sample would be the most expensive thing in the voice by a wide
                // margin, and the cutoff cannot move far in 0.7 ms.
                //
                // The envelope sweeps the cutoff UP from where the control is set, which is
                // the one thing a filter envelope is for: set the cutoff low and the
                // envelope opens it, set it high and the envelope has nowhere to go.
                if ((n % 32) == 0)
                {
                    const float target = juce::jlimit (20.0f, 20000.0f,
                        p.opFilterCutoff[dst]
                          * std::pow (2.0f, op.fltEnvState.env * kFilterEnvOctaves));

                    if (std::abs (target - op.fltLastCutoff) > 0.01f
                          || std::abs (p.opFilterReso[dst] - op.fltLastReso) > 0.0005f)
                    {
                        const float sr = static_cast<float> (sampleRate);
                        const float fc = juce::jlimit (20.0f, sr * 0.45f, target);
                        const float g = std::tan (juce::MathConstants<float>::pi * fc / sr);

                        op.fltK = 2.0f - 2.0f * juce::jlimit (0.0f, 0.98f, p.opFilterReso[dst]);
                        op.fltA1 = 1.0f / (1.0f + g * (g + op.fltK));
                        op.fltA2 = g * op.fltA1;
                        op.fltA3 = g * op.fltA2;
                        op.fltLastCutoff = target;
                        op.fltLastReso = p.opFilterReso[dst];
                    }
                }

                float in = amp;

                if (p.opFilterDrive[dst] > 0.0f)
                    in = std::tanh (in * (1.0f + p.opFilterDrive[dst] * 8.0f));

                const float v3 = in - op.fltIc2;
                const float v1 = op.fltA1 * op.fltIc1 + op.fltA2 * v3;
                const float v2 = op.fltIc2 + op.fltA2 * op.fltIc1 + op.fltA3 * v3;
                op.fltIc1 = 2.0f * v1 - op.fltIc1;
                op.fltIc2 = 2.0f * v2 - op.fltIc2;

                if (p.opFilterType[dst] == 1)       filtered = in - op.fltK * v1 - v2;
                else if (p.opFilterType[dst] == 2)  filtered = v1;
                else                                filtered = v2;
            }

            // Routing: 0 = the output, 1 = the modulation, 2 = both.
            //
            // The modulation path is what downstream operators READ, so this decides whether
            // the filter shapes what you HEAR or what the operator DOES - which is the whole
            // reason the filter is per operator rather than once across the voice.
            if (filterOn && route != 0)
                op.curOut = filtered;

            if (p.carrier[dst])
            {
                const float toOutput = (filterOn && route == 1) ? amp : filtered;
                sum += toOutput * p.level[dst];
            }

            op.phase += opInc[static_cast<size_t> (dst)];
            if (op.phase >= 1.0)
                op.phase -= std::floor (op.phase);
        }

        for (auto& op : ops)
            op.prevOut = op.curOut;

        // --- M3b: VA layer, ADDED to the FM stack ---
        // The VA's own ADSR, driven by the gate EDGE rather than followed as a one-pole
        // average of it. That is what makes attack, decay, sustain and release real
        // parameters instead of a fixed ~8 ms / ~50 ms shape with the sustain pinned.
        //
        // It runs whether or not the VA is in use, so raising VA Level mid-note does not
        // start with a silent ramp-in.
        if (gate != vaGateLast)
        {
            if (gate)
            {
                vaEnvState.stage = 1;   // attack
                vaEnvState.env = 0.0f;
            }
            else
            {
                vaEnvState.releaseFrom = vaEnvState.env;
                vaEnvState.stage = 4;   // release
            }

            vaGateLast = gate;
        }

        vaEnv = updateEnvelope (vaEnvState, p.vaAttack, p.vaDecay, p.vaSustain, p.vaRelease,
                                sampleRate);

        // Slew the three layer levels towards this block's targets before anything uses them.
        // This is the fix for the Sub clicking as it is turned up: the level reaches the mix
        // one sample at a time instead of in one jump per block, so the square's amplitude is
        // continuous even though the square itself is not.
        vaLevelSm      += (p.vaLevel - vaLevelSm) * levelSmooth;
        vaSubLevelSm   += (p.vaSubLevel - vaSubLevelSm) * levelSmooth;
        vaNoiseLevelSm += (p.vaNoiseLevel - vaNoiseLevelSm) * levelSmooth;

        float va = 0.0f;        // the morphed oscillator, scaled by the FM/VA blend
        float vaExtras = 0.0f;  // sub and noise, which carry their own levels

        if (vaLevelSm > 0.0f || vaSubLevelSm > 0.0f || vaNoiseLevelSm > 0.0f)
        {
            // Remembered for isActive(), which cannot see these parameters. See the
            // comment there.
            vaWasAudible = true;

            vaPhase += vaInc;

            if (vaPhase >= 1.0)
                vaPhase -= std::floor (vaPhase);

            // The morph, read from the same tables the operators read, so a VA saw and an
            // operator saw are the same waveform and inherit the same band-limiting. This
            // replaced a polyBLEP saw/square pair, which also means the VA is now clean at
            // high pitch rather than only mostly clean.
            va = mixxynth::OpWaveTables::read (vaCursor, static_cast<float> (vaPhase));

            if (vaSubLevelSm > 0.0f)
            {
                vaSubPhase += vaSubInc;

                if (vaSubPhase >= 1.0)
                    vaSubPhase -= std::floor (vaSubPhase);

                vaExtras += mixxynth::OpWaveTables::read (vaSubCursor,
                                                          static_cast<float> (vaSubPhase))
                              * vaSubLevelSm;
            }

            // Noise, from the same LCG the old Noise entry used.
            if (vaNoiseLevelSm > 0.0f)
            {
                noiseState = noiseState * 1664525u + 1013904223u;
                vaExtras += (static_cast<float> ((noiseState >> 8) & 0xffffu) / 32768.0f - 1.0f)
                              * vaNoiseLevelSm;
            }
        }

        // Additive, not a crossfade: turning VA Level up must not duck the FM
        // patch, which is what the old (1 - vaLevel) blend did.
        //
        // Sub and noise deliberately bypass the blend. They are their own levels, so a
        // patch with the FM stack in charge can still add a sub, which is the whole point
        // of taking them out of the shape list.
        const float mixed = sum + (va * vaLevelSm + vaExtras) * vaEnv;

        // --- M3b: per-voice TPT state-variable filter ---
        float filtered = mixed;

        if (p.filterOn)
        {
            // Recompute the TPT coefficients only when cutoff or resonance move.
            if (std::abs (p.cutoff - svfLastCutoff) > 0.01f
                  || std::abs (p.resonance - svfLastReso) > 0.0005f)
            {
                const float sr = static_cast<float> (sampleRate);
                const float fc = juce::jlimit (20.0f, sr * 0.45f, p.cutoff);
                const float g = std::tan (juce::MathConstants<float>::pi * fc / sr);

                svfK = 2.0f - 2.0f * juce::jlimit (0.0f, 0.98f, p.resonance);
                svfA1 = 1.0f / (1.0f + g * (g + svfK));
                svfA2 = g * svfA1;
                svfA3 = g * svfA2;
                svfLastCutoff = p.cutoff;
                svfLastReso = p.resonance;
            }

            float in = mixed;

            if (p.drive > 0.0f)
                in = std::tanh (in * (1.0f + p.drive * 8.0f));

            const float v3 = in - svfIc2;
            const float v1 = svfA1 * svfIc1 + svfA2 * v3;
            const float v2 = svfIc2 + svfA2 * svfIc1 + svfA3 * v3;
            svfIc1 = 2.0f * v1 - svfIc1;
            svfIc2 = 2.0f * v2 - svfIc2;

            if (p.filterType == 1)       filtered = in - svfK * v1 - v2; // high pass
            else if (p.filterType == 2)  filtered = v1;                  // band pass
            else                         filtered = v2;                  // low pass
        }

        const float out = juce::jlimit (-1.0f, 1.0f, filtered * velScale * p.master);

        if (numCh >= 2)
        {
            buffer.addSample (0, startSample + n, out * panL);
            buffer.addSample (1, startSample + n, out * panR);
        }
        else
        {
            for (int ch = 0; ch < numCh; ++ch)
                buffer.addSample (ch, startSample + n, out);
        }
    }
}

void MiXXFmVoice::renderAdd (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, const VoiceParams& params)
{
    renderRange (buffer, startSample, numSamples, params);
}

void MiXXFmVoice::renderBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const VoiceParams& params)
{
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    int pos = 0;

    for (const auto metadata : midi)
    {
        const int t = juce::jlimit (0, numSamples, metadata.samplePosition);

        if (t > pos)
        {
            renderRange (buffer, pos, t - pos, params);
            pos = t;
        }

        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            noteOn (msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            noteOff (msg.getNoteNumber());
    }

    if (pos < numSamples)
        renderRange (buffer, pos, numSamples - pos, params);

    if (! isActive())
        currentNote = -1;
}
