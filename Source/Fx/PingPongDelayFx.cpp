#include "PingPongDelayFx.h"

#include <cmath>

#include "../ModMatrix.h"

namespace mixxynth
{
    namespace
    {
        constexpr float kMixSplitAt = 0.5f;

        // Time range when not synced. 20 ms is short enough to be a doubler, 2000 ms
        // long enough for a phrase, and the range is exponential so the short end -
        // where the ear is most sensitive to a wrong setting - gets half the travel.
        constexpr float kMinTimeMs = 20.0f;
        constexpr float kMaxTimeMs = 2000.0f;

        float timeMsFor (float value01) noexcept
        {
            return kMinTimeMs * std::pow (kMaxTimeMs / kMinTimeMs,
                                          juce::jlimit (0.0f, 1.0f, value01));
        }

        float onePoleCoeff (float freqHz, double sr) noexcept
        {
            const float f = juce::jlimit (1.0f, static_cast<float> (sr) * 0.49f, freqHz);
            return 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * f
                                      / static_cast<float> (sr));
        }
    }

    float PingPongDelayFx::syncTimeMs (int syncIndex) const noexcept
    {
        // Reuses the modulation matrix's own division list, so a synced delay and a
        // synced LFO offer the same choices and mean the same thing by them.
        const int n = numSyncDivisions();

        if (n <= 0)
            return 500.0f;

        const int i = juce::jlimit (0, n - 1, syncIndex);

        const float cyclesPerBeat = syncDivisionCyclesPerBeat (i);

        if (cyclesPerBeat <= 0.0f)
            return 500.0f;

        // One cycle is a full delay period, so the time is the beat's worth divided by
        // how many cycles fit in it.
        const double beatMs = 60000.0 / juce::jmax (1.0, bpm);

        return static_cast<float> (beatMs / static_cast<double> (cyclesPerBeat));
    }

    const FxDescriptor& PingPongDelayFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Ping-Pong Delay";
            desc.numParams = kNumParams;

            desc.params[pSync].name = "Sync";
            desc.params[pSync].isChoice = true;
            desc.params[pSync].choices.add ("Off");

            for (int i = 0; i < numSyncDivisions(); ++i)
                desc.params[pSync].choices.add (syncDivisionNames()[i]);

            desc.params[pTimeL].name = "Time L";
            desc.params[pTimeL].defaultValue = 0.55f;

            desc.params[pTimeR].name = "Time R";
            desc.params[pTimeR].defaultValue = 0.55f;

            desc.params[pFeedback].name = "Feedback";
            desc.params[pFeedback].defaultValue = 0.40f;

            // Centred: partial bouncing is the useful default, and it is the setting a
            // switch could not have offered.
            desc.params[pPingPong].name = "Ping-Pong";
            desc.params[pPingPong].defaultValue = 0.50f;

            desc.params[pDamping].name = "Damping";
            desc.params[pDamping].defaultValue = 0.45f;

            desc.params[pMod].name = "Mod";
            desc.params[pMod].defaultValue = 0.20f;

            // Off by default, because it is a character rather than a correction - and
            // because a delay that drifts when you did not ask it to is a bug.
            desc.params[pDrift].name = "Drift";
            desc.params[pDrift].defaultValue = 0.0f;

            desc.params[pWidth].name = "Width";
            desc.params[pWidth].defaultValue = 0.70f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.35f;

            return desc;
        }();

        return d;
    }

    void PingPongDelayFx::DelayLine::resize (int lengthSamples)
    {
        buffer.assign (static_cast<size_t> (juce::jmax (4, lengthSamples)), 0.0f);
        writeIndex = 0;
    }

    void PingPongDelayFx::DelayLine::clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    float PingPongDelayFx::DelayLine::read (float delaySamples) const noexcept
    {
        const int size = static_cast<int> (buffer.size());

        if (size <= 0)
            return 0.0f;

        const float d = juce::jlimit (1.0f, static_cast<float> (size - 2), delaySamples);
        const float readPos = static_cast<float> (writeIndex) - d;

        int i0 = static_cast<int> (std::floor (readPos));
        const float frac = readPos - static_cast<float> (i0);

        i0 = ((i0 % size) + size) % size;
        const int i1 = (i0 + 1) % size;

        return buffer[static_cast<size_t> (i0)]
                 + frac * (buffer[static_cast<size_t> (i1)] - buffer[static_cast<size_t> (i0)]);
    }

    float PingPongDelayFx::nextRandom() noexcept
    {
        // The same LCG the noise source uses, so there is one generator in the codebase
        // rather than one per feature.
        rngState = rngState * 1664525u + 1013904223u;

        return static_cast<float> ((rngState >> 8) & 0xffffu) / 32768.0f - 1.0f;
    }

    void PingPongDelayFx::prepare (double newSampleRate, int /*maxBlockSize*/, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        channels = juce::jmax (1, numChannels);

        const int length = static_cast<int> (kMaxDelaySeconds * sampleRate) + 4;

        for (auto& line : lines)
            line.resize (length);

        reset();
        prepared = true;
    }

    void PingPongDelayFx::reset()
    {
        for (auto& line : lines)
            line.clear();

        for (auto& walk : drift)
            walk.clear();

        lfoPhase = 0.0f;
        dampState[0] = dampState[1] = 0.0f;
        currentTimeMs[0] = currentTimeMs[1] = 0.0f;

        // Not primed, so the first block starts from the settings rather than gliding
        // in from wherever the last patch left the lines.
        timePrimed = false;
    }

    void PingPongDelayFx::setTempo (double newBpm)
    {
        bpm = newBpm > 1.0 ? newBpm : 120.0;
    }

    void PingPongDelayFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // ---- what the controls are asking for ---------------------------------

        const int syncIndex = juce::jlimit (0, numSyncDivisions(),
                                static_cast<int> (std::lround (juce::jlimit (0.0f, 1.0f, params[pSync])
                                    * static_cast<float> (numSyncDivisions()))));

        const float feedback = juce::jlimit (0.0f, 1.0f, params[pFeedback]);
        const float cross = juce::jlimit (0.0f, 1.0f, params[pPingPong]);
        const float damping01 = juce::jlimit (0.0f, 1.0f, params[pDamping]);
        const float mod01 = juce::jlimit (0.0f, 1.0f, params[pMod]);
        const float drift01 = juce::jlimit (0.0f, 1.0f, params[pDrift]);
        const float width01 = juce::jlimit (0.0f, 1.0f, params[pWidth]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        // Synced, both channels take the division. The per-channel times are then
        // inactive rather than fighting it - a synced ping-pong is symmetric by nature,
        // and two independent synced times would just be a way to get it wrong.
        const float timeL = syncIndex > 0 ? syncTimeMs (syncIndex - 1)
                                          : timeMsFor (params[pTimeL]);
        const float timeR = syncIndex > 0 ? syncTimeMs (syncIndex - 1)
                                          : timeMsFor (params[pTimeR]);

        // Capped below unity: a delay that can reach 1.0 will run away the moment
        // anything else in the chain adds gain.
        const float fbGain = feedback * 0.95f;

        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        // Damping: the repeats lose their highs as they decay, 12 kHz down to 700 Hz.
        const float dampCoeff = onePoleCoeff (12000.0f * std::pow (0.058f, damping01), sampleRate);

        // Modulation: a slow LFO, a third of a percent of the delay time. Subtle on
        // purpose - this is here to stop the delay sounding static, not to be heard.
        constexpr float modRateHz = 0.23f;
        const float lfoInc = modRateHz / static_cast<float> (sampleRate);

        // Drift: the random walk, as a fraction of the delay time.
        const float driftFraction = drift01 * kMaxDriftFraction;
        const float driftSlew = 1.0f - std::exp (-1.0f
                                    / (kDriftSlewSeconds * static_cast<float> (sampleRate)));
        const float timeSlew = 1.0f - std::exp (-1.0f / (0.05f * static_cast<float> (sampleRate)));

        if (! timePrimed)
        {
            currentTimeMs[0] = timeL;
            currentTimeMs[1] = timeR;
            timePrimed = true;
        }

        const float samplesPerMs = static_cast<float> (sampleRate) * 0.001f;
        const int driftSteps = juce::jmax (1, static_cast<int> (kDriftStepSeconds
                                                                 * static_cast<float> (sampleRate)));

        auto* dataL = buffer.getWritePointer (0);
        auto* dataR = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            const float lfo = std::sin (2.0f * juce::MathConstants<float>::pi * lfoPhase);

            lfoPhase += lfoInc;

            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;

            // The two channels read the LFO a quarter cycle apart, so they move against
            // each other rather than together.
            const float lfoR = std::sin (2.0f * juce::MathConstants<float>::pi
                                           * (lfoPhase + 0.25f));

            std::array<float, 2> delayed {};

            for (int ch = 0; ch < numCh && ch < 2; ++ch)
            {
                const auto c = static_cast<size_t> (ch);

                // A new drift target every kDriftStepSeconds, and a slew towards it. A
                // walk rather than a jump, because a jump in a delay time is a click.
                if (drift[c].countdown <= 0.0f)
                {
                    drift[c].target = nextRandom();
                    drift[c].countdown = static_cast<float> (driftSteps);
                }

                drift[c].countdown -= 1.0f;
                drift[c].value += (drift[c].target - drift[c].value) * driftSlew;

                currentTimeMs[c] += ((ch == 0 ? timeL : timeR) - currentTimeMs[c]) * timeSlew;

                const float base = currentTimeMs[c];
                const float lfoValue = ch == 0 ? lfo : lfoR;

                const float modulated = base * (1.0f
                                                  + lfoValue * mod01 * 0.003f
                                                  + drift[c].value * driftFraction);

                delayed[c] = lines[c].read (modulated * samplesPerMs);
            }

            // A mono host gets the same signal in both lines, so the cross-feedback and
            // the width control still mean something rather than dividing by nothing.
            if (numCh < 2)
                delayed[1] = delayed[0];

            // Cross-feedback. Each channel takes the other's output by `cross` and its
            // own by (1 - cross): at 1 that is strict L/R bouncing, at 0 two independent
            // delays, and everything between is the useful part.
            const float fbL = delayed[0] * (1.0f - cross) + delayed[1] * cross;
            const float fbR = delayed[1] * (1.0f - cross) + delayed[0] * cross;

            // Damping sits in the feedback path, so repeats darken as they decay. This
            // is what makes a delay sit in a mix instead of cluttering it.
            dampState[0] += dampCoeff * (fbL - dampState[0]);
            dampState[1] += dampCoeff * (fbR - dampState[1]);

            const float inL = dataL[i];
            const float inR = dataR != nullptr ? dataR[i] : inL;

            lines[0].buffer[static_cast<size_t> (lines[0].writeIndex)] = inL + dampState[0] * fbGain;
            lines[0].writeIndex = (lines[0].writeIndex + 1) % static_cast<int> (lines[0].buffer.size());

            lines[1].buffer[static_cast<size_t> (lines[1].writeIndex)] = inR + dampState[1] * fbGain;
            lines[1].writeIndex = (lines[1].writeIndex + 1) % static_cast<int> (lines[1].buffer.size());

            // Width on the wet only, so the dry stays where the mix put it.
            const float mid = (delayed[0] + delayed[1]) * 0.5f;
            const float side = (delayed[0] - delayed[1]) * 0.5f * width01 * 2.0f;

            dataL[i] = inL * dry + (mid + side) * wet;

            if (dataR != nullptr)
                dataR[i] = inR * dry + (mid - side) * wet;
        }
    }
}
