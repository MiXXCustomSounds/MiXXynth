cmake_minimum_required(VERSION 3.25)

project(MiXXynth VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# M1a: empty synth shell. IS_SYNTH TRUE, MIDI in. No FM/DSP yet.
juce_add_plugin(MiXXynth
    COMPANY_NAME "MiXXCustomSounds"
    BUNDLE_ID "com.mixx.mixxynth"
    PLUGIN_MANUFACTURER_CODE MxCs
    PLUGIN_CODE Mxs1
    FORMATS VST3 Standalone
    PRODUCT_NAME "MiXXynth"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(MiXXynth)

target_sources(MiXXynth PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
)

target_compile_features(MiXXynth PRIVATE cxx_std_20)
target_compile_definitions(MiXXynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(MiXXynth PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags
)

add_custom_command(TARGET MiXXynth_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/VST3"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "$<TARGET_FILE_DIR:MiXXynth_VST3>/../.."
        "${CMAKE_SOURCE_DIR}/VST3/MiXXynth.vst3"
    COMMENT "Staging the installable bundle at VST3/MiXXynth.vst3"
    VERBATIM
)
