#include "ConvolutionFx.h"

#include <algorithm>
#include <cmath>

namespace mixxynth
{
    namespace
    {
        constexpr int kNumBuiltIns = 6;

        constexpr double kMaxPreDelaySeconds = 0.25;
        constexpr float kMixSplitAt = 0.75f;

        // Tail trim is quantised, because each change means rebuilding the impulse
        // and handing it to the convolver. Unquantised, a drag would rebuild it
        // hundreds of times a second for no audible benefit.
        constexpr int kLengthSteps = 16;

        // One-pole coefficient for a cutoff in Hz.
        float onePoleCoeff (float hz, double sr)
        {
            const float clamped = juce::jmin (hz, static_cast<float> (sr) * 0.45f);
            return juce::jlimit (0.0f, 0.9999f,
                                 1.0f - std::exp (-juce::MathConstants<float>::twoPi * clamped
                                                    / static_cast<float> (sr)));
        }
    }

    const ConvolutionFx::IrSpec& ConvolutionFx::specFor (int index) noexcept
    {
        // Deterministic and version-stable: a preset that names "Cathedral" has to
        // sound the same on every machine and in every future build, which is why
        // the generator uses a fixed seed rather than a random one.
        static const IrSpec specs[kNumBuiltIns] =
        {
            // name            len   decay  hfDamp diff  erLvl  erSpread  bright
            { "Small Room",    0.9f, 0.70f, 0.55f, 0.50f, 0.75f, 14.0f,    0.42f },
            { "Large Room",    1.8f, 1.50f, 0.50f, 0.60f, 0.65f, 28.0f,    0.46f },
            { "Bright Plate",  2.2f, 1.90f, 0.25f, 0.85f, 0.15f, 10.0f,    0.78f },
            { "Dark Plate",    2.4f, 2.10f, 0.78f, 0.85f, 0.15f, 10.0f,    0.20f },
            { "Cathedral",     4.0f, 3.40f, 0.45f, 0.70f, 0.50f, 45.0f,    0.50f },
            { "Spring Tank",   1.4f, 1.20f, 0.35f, 0.95f, 0.20f,  6.0f,    0.62f }
        };

        return specs[static_cast<size_t> (juce::jlimit (0, kNumBuiltIns - 1, index))];
    }

    int ConvolutionFx::getNumBuiltInIrs() noexcept { return kNumBuiltIns; }

    const char* ConvolutionFx::getBuiltInIrName (int index) noexcept
    {
        return specFor (index).name;
    }

    bool ConvolutionFx::buildImpulse (int choice, const juce::MemoryBlock& data,
                                      juce::AudioBuffer<float>& out) const
    {
        // A user file wins when one is present and the slot is asking for it.
        if (choice >= kNumBuiltIns)
        {
            if (data.isEmpty())
                return false; // custom selected but nothing loaded yet

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();

            auto stream = std::make_unique<juce::MemoryInputStream> (data, false);

            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (
                    formats.createReaderFor (std::move (stream))))
            {
                const int length = static_cast<int> (
                    juce::jmin<juce::int64> (reader->lengthInSamples,
                                             static_cast<juce::int64> (sampleRate * 20.0)));

                if (length < 16)
                    return false;

                out.setSize (2, length);

                // A mono file feeds both channels rather than leaving one silent.
                reader->read (&out, 0, length, 0, true, reader->numChannels > 1);

                return true;
            }

            return false;
        }

        const auto& spec = specFor (choice);
        const int length = juce::jmax (64, static_cast<int> (spec.lengthSeconds * sampleRate));

        out.setSize (2, length);
        out.clear();

        const float decayRate = 6.907755f / juce::jmax (0.05f, spec.decaySeconds);
        const float buildUp = juce::jmax (0.004f, spec.diffusion * 0.12f);

        // Two fixed lowpasses, crossfaded over the length of the tail: the highs die
        // first, which is what makes a tail sound like air rather than a filter.
        const float brightCoeff = onePoleCoeff (1200.0f + spec.brightness * 14000.0f, sampleRate);
        const float darkCoeff = onePoleCoeff (700.0f + spec.brightness * 2500.0f, sampleRate);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto* dst = out.getWritePointer (ch);

            float brightState = 0.0f;
            float darkState = 0.0f;

            // A different seed per channel, so the two sides are decorrelated. An IR
            // with identical channels gives a mono reverb no matter what follows.
            juce::Random rng (0x5eed0000 + choice * 977 + ch * 31);

            for (int i = 0; i < length; ++i)
            {
                const float t = static_cast<float> (i) / static_cast<float> (sampleRate);
                const float x = static_cast<float> (i) / static_cast<float> (length);

                const float noise = rng.nextFloat() * 2.0f - 1.0f;

                brightState += brightCoeff * (noise - brightState);
                darkState += darkCoeff * (noise - darkState);

                // Brightness fades with time: this is the frequency-dependent decay.
                const float tilt = 1.0f - spec.hfDamping * x;
                const float shaped = brightState * tilt + darkState * (1.0f - tilt);

                // Decay, plus a build-up so the tail fades in rather than clicking.
                const float env = std::exp (-decayRate * t);
                const float ramp = 1.0f - std::exp (-t / buildUp);

                dst[i] = shaped * env * ramp;
            }
        }

        // Early reflections, as discrete taps over the first `erSpreadMs`. Without
        // these a synthetic IR has no sense of a room's size, only of its tail.
        {
            const int spread = static_cast<int> (spec.erSpreadMs * 0.001f * sampleRate);

            for (int tap = 0; tap < 12; ++tap)
            {
                const float frac = static_cast<float> (tap) / 11.0f;
                const int at = static_cast<int> (frac * frac * static_cast<float> (spread));

                if (at <= 0 || at >= length)
                    continue;

                const float gain = spec.erLevel * std::pow (0.72f, static_cast<float> (tap));

                // Alternating polarity, so the taps do not build a comb at one
                // frequency.
                const float sign = (tap % 2 == 0) ? 1.0f : -1.0f;

                for (int ch = 0; ch < 2; ++ch)
                    out.getWritePointer (ch)[at] += gain * sign * (ch == 0 ? 1.0f : 0.87f);
            }
        }

        return true;
    }

    const FxDescriptor& ConvolutionFx::descriptor()
    {
        static const FxDescriptor d = []
        {
            FxDescriptor desc;
            desc.name = "Convolution";
            desc.numParams = kNumParams;

            // The built-in impulses, then the user's own file. Listing them as a
            // choice means the selection is an ordinary automatable parameter, and
            // the card already knows how to draw one.
            desc.params[pIr].name = "Impulse";
            desc.params[pIr].isChoice = true;

            for (int i = 0; i < kNumBuiltIns; ++i)
                desc.params[pIr].choices.add (getBuiltInIrName (i));

            desc.params[pIr].choices.add ("Custom File...");

            desc.params[pLength].name = "Length";
            desc.params[pLength].defaultValue = 0.65f;

            desc.params[pPreDelay].name = "Pre-Delay";

            desc.params[pLowCut].name = "Low Cut";
            desc.params[pLowCut].defaultValue = 0.10f;

            desc.params[pHighCut].name = "High Cut";
            desc.params[pHighCut].defaultValue = 0.85f;

            desc.params[pWidth].name = "Width";
            desc.params[pWidth].defaultValue = 0.50f;

            desc.params[pMix].name = "Mix";
            desc.params[pMix].defaultValue = 0.35f;

            desc.params[pNormalise].name = "Normalise";
            desc.params[pNormalise].isChoice = true;
            desc.params[pNormalise].choices = { "Off", "On" };
            desc.params[pNormalise].defaultValue = 1.0f;

            // The impulse response is a FILE, not a parameter, so the card draws a
            // button for it. See FxDescriptor::hasFile.
            desc.hasFile = true;
            desc.fileLabel = "Impulse File";
            desc.fileFilter = "*.wav;*.aif;*.aiff;*.flac;*.ogg";

            return desc;
        }();

        return d;
    }

    ConvolutionFx::ConvolutionFx() = default;

    ConvolutionFx::~ConvolutionFx()
    {
        // Must not be left with a pending callback when this object goes away.
        cancelPendingUpdate();
    }

    void ConvolutionFx::prepare (double newSampleRate, int newBlockSize, int numChannels)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        blockSize = juce::jmax (1, newBlockSize);
        channels = juce::jmax (2, juce::jmin (numChannels, 2));

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = static_cast<juce::uint32> (channels);

        convolution.prepare (spec);

        wetBuffer.setSize (channels, blockSize);

        preLength = static_cast<int> (std::ceil (kMaxPreDelaySeconds * sampleRate)) + 2;
        preBuffer.assign (static_cast<size_t> (preLength) * static_cast<size_t> (channels), 0.0f);
        preWrite = 0;

        // Force a rebuild at the new sample rate: the impulses are rate-dependent.
        loadedIr = -1;

        prepared = true;
    }

    void ConvolutionFx::reset()
    {
        convolution.reset();

        std::fill (preBuffer.begin(), preBuffer.end(), 0.0f);
        preWrite = 0;

        for (int ch = 0; ch < 2; ++ch)
        {
            lowCutState[ch] = 0.0f;
            highCutState[ch] = 0.0f;
        }
    }

    void ConvolutionFx::setFile (const juce::String& name, const juce::MemoryBlock& data)
    {
        fileName = name;
        fileData = data;

        // A new file invalidates whatever is loaded, so the next tick rebuilds it.
        loadedIr = -1;
    }

    juce::String ConvolutionFx::getLoadedIrName() const
    {
        return loadedName;
    }

    void ConvolutionFx::reloadImpulse()
    {
        const int choice = juce::jlimit (0, kNumBuiltIns, requestedIr.load());
        const int trimSteps = juce::jlimit (1, kLengthSteps, requestedLength.load());
        const bool normalise = requestedNormalise.load() != 0;

        // Record what we are about to load, whatever happens, so a failure does not
        // retry on every tick.
        loadedIr = choice;
        loadedLength = trimSteps;
        loadedNormalise = normalise ? 1 : 0;

        if (! buildImpulse (choice, fileData, rawIr) || rawIr.getNumSamples() < 16)
        {
            loadedName = (choice >= kNumBuiltIns)
                           ? (fileName.isNotEmpty() ? fileName : juce::String ("no file"))
                           : juce::String (getBuiltInIrName (choice));

            // Nothing to convolve with. Loading an empty impulse rather than leaving
            // the previous one in place, so a missing file is silent instead of
            // quietly playing the wrong reverb.
            juce::AudioBuffer<float> empty (2, 16);
            empty.clear();

            // The buffer overload takes Stereo BEFORE Trim, and Normalise last with
            // no default - which is not the order the file overload uses.
            convolution.loadImpulseResponse (std::move (empty), sampleRate,
                                             juce::dsp::Convolution::Stereo::yes,
                                             juce::dsp::Convolution::Trim::no,
                                             juce::dsp::Convolution::Normalise::no);
            return;
        }

        const int keep = juce::jlimit (16, rawIr.getNumSamples(),
                                       rawIr.getNumSamples() * trimSteps / kLengthSteps);

        juce::AudioBuffer<float> trimmed (2, keep);
        trimmed.clear();

        for (int ch = 0; ch < 2; ++ch)
            trimmed.copyFrom (ch, 0, rawIr, juce::jmin (ch, rawIr.getNumChannels() - 1), 0, keep);

        // A short fade at the cut, so trimming does not leave a step that reads as a
        // click in the tail.
        {
            const int fade = juce::jmin (keep / 8, static_cast<int> (sampleRate * 0.01));

            for (int ch = 0; ch < 2 && fade > 1; ++ch)
            {
                auto* dst = trimmed.getWritePointer (ch);

                for (int i = 0; i < fade; ++i)
                    dst[keep - 1 - i] *= static_cast<float> (i) / static_cast<float> (fade);
            }
        }

        convolution.loadImpulseResponse (
            std::move (trimmed), sampleRate,
            juce::dsp::Convolution::Stereo::yes,
            juce::dsp::Convolution::Trim::no,
            normalise ? juce::dsp::Convolution::Normalise::yes
                      : juce::dsp::Convolution::Normalise::no);

        loadedName = (choice >= kNumBuiltIns)
                       ? (fileName.isNotEmpty() ? fileName : juce::String ("no file"))
                       : juce::String (getBuiltInIrName (choice));
    }

    void ConvolutionFx::refreshImpulse()
    {
        if (! prepared)
            return;

        const int choice = juce::jlimit (0, kNumBuiltIns, requestedIr.load());
        const int trimSteps = juce::jlimit (1, kLengthSteps, requestedLength.load());
        const int normalise = requestedNormalise.load();

        if (choice == loadedIr && trimSteps == loadedLength && normalise == loadedNormalise)
            return;

        reloadImpulse();
    }

    void ConvolutionFx::handleAsyncUpdate()
    {
        refreshImpulse();
    }

    void ConvolutionFx::process (juce::AudioBuffer<float>& buffer, const float* params)
    {
        if (! prepared || params == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numSamples <= 0 || numCh <= 0)
            return;

        // Work out what the user is asking for. Rebuilding an impulse is
        // message-thread work, so the audio thread only records the request and
        // nudges an AsyncUpdater - which is lock-free here and coalesces, so a knob
        // drag asks for one rebuild rather than hundreds.
        {
            const int wantIr = static_cast<int> (std::lround (
                                   juce::jlimit (0.0f, 1.0f, params[pIr])
                                     * static_cast<float> (kNumBuiltIns)));

            const int wantLength = juce::jlimit (1, kLengthSteps,
                static_cast<int> (std::lround (juce::jlimit (0.0f, 1.0f, params[pLength])
                                                 * static_cast<float> (kLengthSteps))));

            const int wantNormalise = params[pNormalise] > 0.5f ? 1 : 0;

            if (wantIr != requestedIr.load() || wantLength != requestedLength.load()
                  || wantNormalise != requestedNormalise.load())
            {
                requestedIr.store (wantIr);
                requestedLength.store (wantLength);
                requestedNormalise.store (wantNormalise);

                triggerAsyncUpdate();
            }
        }

        const float preDelay01 = juce::jlimit (0.0f, 1.0f, params[pPreDelay]);
        const float lowCut01 = juce::jlimit (0.0f, 1.0f, params[pLowCut]);
        const float highCut01 = juce::jlimit (0.0f, 1.0f, params[pHighCut]);
        const float width01 = juce::jlimit (0.0f, 1.0f, params[pWidth]);
        const float mix01 = juce::jlimit (0.0f, 1.0f, params[pMix]);

        // Dry/wet through the pedal's three points, matching the other reverbs so the
        // two are interchangeable in a chain.
        const float wet = mix01 <= kMixSplitAt
            ? (mix01 / kMixSplitAt) * 0.5f
            : 0.5f + ((mix01 - kMixSplitAt) / (1.0f - kMixSplitAt)) * 0.5f;
        const float dry = 1.0f - wet;

        const float width = width01 * 2.0f;
        const float lowCutCoeff = onePoleCoeff (20.0f * std::pow (50.0f, lowCut01), sampleRate);
        const float highCutCoeff = onePoleCoeff (1000.0f * std::pow (20.0f, highCut01), sampleRate);

        // ---- pre-delay, then convolve ---------------------------------------
        // The pre-delay runs into the wet buffer rather than in place, because the
        // convolver needs the dry signal of the block and the output overwrites it.
        wetBuffer.setSize (numCh, numSamples, false, false, true);

        const float preDelaySamples = preDelay01 * static_cast<float> (kMaxPreDelaySeconds)
                                        * static_cast<float> (sampleRate);

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* src = buffer.getReadPointer (ch);
            auto* dst = wetBuffer.getWritePointer (ch);
            float* pre = preBuffer.data() + static_cast<size_t> (ch) * static_cast<size_t> (preLength);

            // Write the block into the delay line in as many chunks as it takes.
            //
            // A host is free to hand us a block longer than 250 ms, and this is the
            // third version of these four lines: the first assumed the block was
            // shorter than the line (it wrote past the end), and the second assumed
            // it was shorter than TWICE the line, which is the same bug with a bigger
            // constant. A loop has neither assumption.
            {
                int written = 0;
                int writePos = preWrite;

                while (written < numSamples)
                {
                    const int chunk = juce::jmin (numSamples - written, preLength - writePos);

                    std::copy (src + written, src + written + chunk, pre + writePos);

                    written += chunk;
                    writePos += chunk;

                    if (writePos >= preLength)
                        writePos = 0;
                }
            }

            int read = preWrite - static_cast<int> (preDelaySamples);

            while (read < 0)
                read += preLength;

            for (int i = 0; i < numSamples; ++i)
            {
                dst[i] = (preDelaySamples > 0.5f) ? pre[read] : src[i];

                if (++read >= preLength)
                    read = 0;
            }
        }

        preWrite += numSamples;

        while (preWrite >= preLength)
            preWrite -= preLength;

        {
            juce::dsp::AudioBlock<float> block (wetBuffer.getArrayOfWritePointers(),
                                                static_cast<size_t> (numCh),
                                                static_cast<size_t> (numSamples));

            juce::dsp::ProcessContextReplacing<float> context (block);
            convolution.process (context);
        }

        // ---- tone, width, mix ------------------------------------------------
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* wetPtr = wetBuffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                float x = wetPtr[i];

                // High cut first, then low cut: the other order lets the highpass
                // undo part of the lowpass's work at the very top.
                highCutState[ch] += highCutCoeff * (x - highCutState[ch]);
                x = highCutState[ch];

                lowCutState[ch] += lowCutCoeff * (x - lowCutState[ch]);
                x -= lowCutState[ch];

                wetPtr[i] = x;
            }
        }

        if (numCh == 2)
        {
            auto* l = wetBuffer.getWritePointer (0);
            auto* r = wetBuffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                const float mid = (l[i] + r[i]) * 0.5f;
                const float side = (l[i] - r[i]) * 0.5f * width;

                l[i] = mid + side;
                r[i] = mid - side;
            }
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* dryPtr = buffer.getReadPointer (ch);
            auto* wetPtr = wetBuffer.getReadPointer (ch);
            auto* dst = buffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
                dst[i] = dry * dryPtr[i] + wet * wetPtr[i];
        }
    }
}
