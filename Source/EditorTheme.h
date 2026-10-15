#pragma once

#include <memory>

#include <sst/jucegui/style/StyleAndSettingsConsumer.h>
#include <sst/jucegui/style/StyleSheet.h>

// M3a: shared sst-jucegui styling setup.
// The look is deliberately plain for now (dark built-in stylesheet); the
// black-metal / dark-oak / glow pass happens later, as agreed.
namespace mixxynth::ui
{
    inline void initialiseWidgetStyles()
    {
        static bool done = false;

        if (! done)
        {
            sst::jucegui::style::StyleSheet::initializeStyleSheets ([]() {});
            done = true;
        }
    }

    inline sst::jucegui::style::StyleSheet::ptr_t darkStyle()
    {
        return sst::jucegui::style::StyleSheet::getBuiltInStyleSheet (
            sst::jucegui::style::StyleSheet::DARK);
    }

    inline sst::jucegui::style::Settings::ptr_t sharedSettings()
    {
        static auto settings = std::make_shared<sst::jucegui::style::Settings>();
        return settings;
    }

    // Applies the shared dark style + settings to any sst-jucegui widget.
    template <typename T>
    inline void style (T& widget)
    {
        widget.setStyle (darkStyle());
        widget.setSettings (sharedSettings());
    }
}
