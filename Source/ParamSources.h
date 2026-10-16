#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <sst/jucegui/data/Continuous.h>
#include <sst/jucegui/data/Discrete.h>

#include "SnapValues.h"

// M3a: adapters exposing MiXXynth's APVTS parameters to sst-jucegui widgets.
// All edits go through the parameter so host automation and undo behave.
namespace mixxynth::ui
{
    struct ContinuousSource : public sst::jucegui::data::Continuous,
                              private juce::AudioProcessorParameter::Listener,
                              private juce::AsyncUpdater
    {
        explicit ContinuousSource (juce::RangedAudioParameter* param) : p (param)
        {
            if (p != nullptr)
                p->addListener (this);
        }

        ~ContinuousSource() override
        {
            // AsyncUpdater must not be left with a pending callback when the
            // widget goes away.
            cancelPendingUpdate();

            if (p != nullptr)
                p->removeListener (this);
        }

        std::string getLabel() const override
        {
            return p != nullptr ? p->getName (64).toStdString() : std::string {};
        }

        // sst-jucegui's continuous editor drags in REAL value space, scaled by
        // getMax() - getMin() with 150 px covering the whole range, while the dial
        // itself is drawn from getValue01(). For a linear parameter those agree.
        // For a skewed or log range they do not: the drag stays linear in raw
        // units (the filter cutoff moved ~133 Hz per pixel, so the bottom of the
        // knob was hypersensitive) while the dial moves logarithmically.
        //
        // Reporting the NORMALISED 0..1 range instead puts the drag and the
        // drawing in the same space. Linear parameters feel exactly as they did,
        // because normalised space is just a linear rescale of them.
        float getValue() const override
        {
            return p != nullptr ? p->getValue() : 0.0f;
        }

        float getDefaultValue() const override
        {
            return p != nullptr ? p->getDefaultValue() : 0.0f;
        }

        // The dial is drawn from the normalised value, which is the same fraction
        // the parameter already stores.
        float getValue01() override
        {
            return getValue();
        }

        float getMin() const override { return 0.0f; }
        float getMax() const override { return 1.0f; }

        void setValueFromGUI (const float& f) override
        {
            if (p == nullptr)
                return;

            // Snap mode lives HERE, and only here.
            //
            // setValueFromGUI is by definition the user moving a control, and
            // setValueFromModel below is the automation and preset path. Quantising a host's
            // automation to 5% steps would be a bug rather than a feature, so the snap goes
            // in the one place that cannot touch it.
            const float snapped = mixxynth::SnapValues::snap (*p, juce::jlimit (0.0f, 1.0f, f));

            p->beginChangeGesture();
            p->setValueNotifyingHost (snapped);
            p->endChangeGesture();
            notifyAll();
        }

        void setValueFromModel (const float& f) override
        {
            if (p == nullptr)
                return;

            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, f));
            notifyAll();
        }

        std::string getValueAsStringFor (float f) const override
        {
            return p != nullptr ? p->getText (juce::jlimit (0.0f, 1.0f, f), 8).toStdString()
                                : std::to_string (f);
        }

        void notifyAll()
        {
            for (auto* l : guilisteners)
                l->dataChanged();
        }

        juce::RangedAudioParameter* p = nullptr;

    private:
        // Any change to the parameter - the dice, a preset load, host automation,
        // undo - has to repaint the widget. Only setValueFromGUI/setValueFromModel
        // used to notify, so a knob went stale whenever something else moved the
        // parameter. Message-thread changes repaint at once; audio-thread changes
        // (host automation) are deferred via AsyncUpdater, which coalesces them.
        void parameterValueChanged (int, float) override
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                notifyAll();
            else
                triggerAsyncUpdate();
        }

        void parameterGestureChanged (int, bool) override {}
        void handleAsyncUpdate() override { notifyAll(); }
    };

    struct DiscreteSource : public sst::jucegui::data::Discrete,
                            private juce::AudioProcessorParameter::Listener,
                            private juce::AsyncUpdater
    {
        explicit DiscreteSource (juce::AudioParameterChoice* param) : p (param)
        {
            if (p != nullptr)
                p->addListener (this);
        }

        ~DiscreteSource() override
        {
            cancelPendingUpdate();

            if (p != nullptr)
                p->removeListener (this);
        }

        std::string getLabel() const override
        {
            return p != nullptr ? p->getName (64).toStdString() : std::string {};
        }

        int getValue() const override { return p != nullptr ? p->getIndex() : 0; }

        void setValueFromGUI (const int& f) override
        {
            if (p == nullptr)
                return;

            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (juce::jlimit (getMin(), getMax(), f))));
            p->endChangeGesture();
            notifyAll();
        }

        void setValueFromModel (const int& f) override
        {
            if (p == nullptr)
                return;

            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (juce::jlimit (getMin(), getMax(), f))));
            notifyAll();
        }

        int getMin() const override { return 0; }

        int getMax() const override
        {
            return p != nullptr ? juce::jmax (0, p->choices.size() - 1) : 0;
        }

        std::string getValueAsStringFor (int i) const override
        {
            if (p == nullptr || p->choices.isEmpty())
                return {};

            return p->choices[juce::jlimit (0, p->choices.size() - 1, i)].toStdString();
        }

        void notifyAll()
        {
            for (auto* l : guilisteners)
                l->dataChanged();
        }

        juce::AudioParameterChoice* p = nullptr;

    private:
        void parameterValueChanged (int, float) override
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                notifyAll();
            else
                triggerAsyncUpdate();
        }

        void parameterGestureChanged (int, bool) override {}
        void handleAsyncUpdate() override { notifyAll(); }
    };
}
