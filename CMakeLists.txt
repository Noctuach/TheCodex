cmake_minimum_required(VERSION 3.22)
project(Codex VERSION 1.0.0)

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

# Embedded fonts (Barlow, SIL Open Font License, see Resources/Fonts/OFL-Barlow.txt)
juce_add_binary_data(CodexAssets
    HEADER_NAME CodexAssets.h
    NAMESPACE CodexAssets
    SOURCES
        Resources/Fonts/Barlow-Regular.ttf
        Resources/Fonts/Barlow-Medium.ttf
        Resources/Fonts/Barlow-SemiBold.ttf
        Resources/Fonts/BarlowCondensed-SemiBold.ttf
        Resources/Fonts/BarlowCondensed-Bold.ttf)
set_target_properties(CodexAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Branding: change these to your own name before releasing.
set(CODEX_COMPANY "Codex Audio" CACHE STRING "Company / developer name shown in hosts")
set(CODEX_BUNDLE_ID "com.codexaudio.codex" CACHE STRING "Reverse-DNS bundle identifier (macOS), no spaces")
set(CODEX_MANUFACTURER_CODE "Cdxa" CACHE STRING "4-character manufacturer code (first letter upper case)")

set(HK_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND HK_FORMATS AU)
endif()

juce_add_plugin(Codex
    COMPANY_NAME "${CODEX_COMPANY}"
    BUNDLE_ID "${CODEX_BUNDLE_ID}"
    PRODUCT_NAME "Codex"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE ${CODEX_MANUFACTURER_CODE}
    PLUGIN_CODE Cdx1
    VST3_CATEGORIES Instrument Synth Drum
    FORMATS ${HK_FORMATS})

set(HK_SOURCES
    Source/Parameters.cpp
    Source/PresetManager.cpp
    Source/PluginProcessor.cpp
    Source/GUI/PluginEditor.cpp
    Source/GUI/EQDisplay.cpp)

target_sources(Codex PRIVATE ${HK_SOURCES})

set(HK_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Codex PUBLIC ${HK_DEFS})

target_link_libraries(Codex
    PRIVATE CodexAssets juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Optional offline renderer: renders a kick to WAV and a GUI snapshot to PNG (for testing).
option(HK_BUILD_TOOLS "Build the offline render tool" OFF)
if(HK_BUILD_TOOLS)
    juce_add_console_app(CodexRender PRODUCT_NAME "CodexRender")
    target_sources(CodexRender PRIVATE tools/Render.cpp ${HK_SOURCES})
    target_compile_definitions(CodexRender PRIVATE ${HK_DEFS} JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_VersionString="${PROJECT_VERSION}")
    target_link_libraries(CodexRender
        PRIVATE CodexAssets juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
