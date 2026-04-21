cmake_minimum_required(VERSION 3.22)

project(CUE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/JUCE.\n"
        "Run: git submodule add https://github.com/juce-framework/JUCE.git JUCE && "
        "git -C JUCE checkout 8.0.4")
endif()

add_subdirectory(JUCE)

juce_add_plugin(CUE
    COMPANY_NAME                "Ghost Note"
    PLUGIN_MANUFACTURER_CODE    GhNt
    PLUGIN_CODE                 CuEn
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "CUE"
    BUNDLE_ID                   com.ghostnote.cue
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             "Fx"
)

target_sources(CUE PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/OscBridge.cpp
)

target_include_directories(CUE PRIVATE Source)

target_compile_definitions(CUE PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
)

target_link_libraries(CUE PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_basics
    juce::juce_osc
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
