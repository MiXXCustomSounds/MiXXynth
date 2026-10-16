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

# UI widgets (MIT, Surge XT's component library). Fetched rather than vendored
# so fresh clones and CI stay reproducible. Requires juce_gui_basics to exist,
# which the JUCE fetch above guarantees.
FetchContent_Declare(
    sst-jucegui
    GIT_REPOSITORY https://github.com/surge-synthesizer/sst-jucegui.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sst-jucegui)

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
    Source/ModKnob.cpp
    Source/ModKnob.h
    Source/ModTarget.cpp
    Source/ModTarget.h
    Source/SquareCell.cpp
    Source/SquareCell.h
    Source/WheelComponent.cpp
    Source/WheelComponent.h
    Source/ParamSources.h
    Source/EditorTheme.h
    Source/FmVoice.cpp
    Source/FmVoice.h
    Source/VoiceManager.cpp
    Source/VoiceManager.h
    Source/ModMatrix.cpp
    Source/ModMatrix.h
    Source/ModShape.cpp
    Source/ModShape.h
    Source/OpWave.cpp
    Source/OpWave.h
    Source/ShapeEditorComponent.cpp
    Source/ShapeEditorComponent.h
    Source/Fx/FxModule.h
    Source/Fx/FxRack.cpp
    Source/Fx/FxRack.h
    Source/Fx/WeirdVerbFx.cpp
    Source/Fx/WeirdVerbFx.h
    Source/Fx/ConvolutionFx.cpp
    Source/Fx/ConvolutionFx.h
    Source/Algorithms.h
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
    sst-jucegui
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

# M2a: offline DSP smoke test (no host needed). TDD gate before pluginval.
add_executable(MiXXynthSmoke
    tests/Smoke.cpp
    Source/FmVoice.cpp
    Source/VoiceManager.cpp
    Source/ModMatrix.cpp
    Source/ModShape.cpp
    Source/OpWave.cpp
    Source/Fx/FxRack.cpp
    Source/Fx/WeirdVerbFx.cpp
    Source/Fx/ConvolutionFx.cpp
)
target_link_libraries(MiXXynthSmoke PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_warning_flags
)
target_compile_features(MiXXynthSmoke PRIVATE cxx_std_20)
target_compile_definitions(MiXXynthSmoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STANDALONE_APPLICATION=0
)

# Editor audit: builds the processor + editor offscreen and checks every knob's
# parameter/destination wiring, the destination name list, UI coverage of all
# parameters, and the drag-and-drop modulation slot logic.
add_executable(MiXXynthEditorAudit
    tests/EditorAudit.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ModKnob.cpp
    Source/ModTarget.cpp
    Source/SquareCell.cpp
    Source/WheelComponent.cpp
    Source/FmVoice.cpp
    Source/VoiceManager.cpp
    Source/ModMatrix.cpp
    Source/ModShape.cpp
    Source/OpWave.cpp
    Source/ShapeEditorComponent.cpp
    Source/Fx/FxRack.cpp
    Source/Fx/WeirdVerbFx.cpp
    Source/Fx/ConvolutionFx.cpp
)
target_link_libraries(MiXXynthEditorAudit PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    sst-jucegui
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_warning_flags
)
target_compile_features(MiXXynthEditorAudit PRIVATE cxx_std_20)
target_compile_definitions(MiXXynthEditorAudit PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STANDALONE_APPLICATION=0
)
