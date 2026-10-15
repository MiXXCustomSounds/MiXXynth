#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <sst/jucegui/data/Continuous.h>
#include <sst/jucegui/data/Discrete.h>

// M3a: adapters exposing MiXXynth's APVTS parameters to sst-jucegui widgets.
// All edits go through the parameter so host automation and undo behave.
namespace mixxynth::ui
{
    struct ContinuousSource : public sst::jucegui::data::Continuous
    {
        explicit ContinuousSource (juce::RangedAudioParameter* param) : p (param) {}

        std::string getLabel() const override
        {
            return p != nullptr ? p->getName (64).toStdString() : std::string {};
        }

        // AudioProcessorParameter::getValue() is the NORMALISED 0..1 value, while
        // getMin/getMax below are the real range, so it has to be denormalised
        // here or every dial reads the wrong position.
        float getValue() const override
        {
            return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
        }

        float getDefaultValue() const override
        {
            return p != nullptr ? p->convertFrom0to1 (p->getDefaultValue()) : 0.0f;
        }

        // sst-jucegui draws the dial from getValue01(). The parameter's
        // normalised value already is that fraction, so return it directly:
        // this is also correct for skewed ranges such as the log filter cutoff.
        float getValue01() override
        {
            return p != nullptr ? p->getValue() : 0.0f;
        }

        float getMin() const override
        {
            return p != nullptr ? p->getNormalisableRange().start : 0.0f;
        }

        float getMax() const override
        {
            return p != nullptr ? p->getNormalisableRange().end : 1.0f;
        }

        void setValueFromGUI (const float& f) override
        {
            if (p == nullptr)
                return;

            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (getMin(), getMax(), f)));
            p->endChangeGesture();
            notifyAll();
        }

        void setValueFromModel (const float& f) override
        {
            if (p == nullptr)
                return;

            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (getMin(), getMax(), f)));
            notifyAll();
        }

        std::string getValueAsStringFor (float f) const override
        {
            return p != nullptr ? p->getText (p->convertTo0to1 (f), 8).toStdString()
                                : std::to_string (f);
        }

        void notifyAll()
        {
            for (auto* l : guilisteners)
                l->dataChanged();
        }

        juce::RangedAudioParameter* p = nullptr;
    };

    struct DiscreteSource : public sst::jucegui::data::Discrete
    {
        explicit DiscreteSource (juce::AudioParameterChoice* param) : p (param) {}

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
    };
}
