cmake_minimum_required(VERSION 3.22)
project(HardKick VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use a local JUCE checkout with -DJUCE_DIR=/path/to/JUCE, otherwise it is downloaded.
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(HK_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND HK_FORMATS AU)
endif()

juce_add_plugin(HardKick
    COMPANY_NAME "Oldskool Audio"
    PRODUCT_NAME "HardKick"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Olsk
    PLUGIN_CODE Hkk1
    VST3_CATEGORIES Instrument Synth Drum
    FORMATS ${HK_FORMATS})

set(HK_SOURCES
    Source/Parameters.cpp
    Source/PresetManager.cpp
    Source/PluginProcessor.cpp
    Source/GUI/PluginEditor.cpp
    Source/GUI/EQDisplay.cpp)

target_sources(HardKick PRIVATE ${HK_SOURCES})

set(HK_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(HardKick PUBLIC ${HK_DEFS})

target_link_libraries(HardKick
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Optional offline renderer: renders a kick to WAV and a GUI snapshot to PNG (for testing).
option(HK_BUILD_TOOLS "Build the offline render tool" OFF)
if(HK_BUILD_TOOLS)
    juce_add_console_app(HardKickRender PRODUCT_NAME "HardKickRender")
    target_sources(HardKickRender PRIVATE tools/Render.cpp ${HK_SOURCES})
    target_compile_definitions(HardKickRender PRIVATE ${HK_DEFS} JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(HardKickRender
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
