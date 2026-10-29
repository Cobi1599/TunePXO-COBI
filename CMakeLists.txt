cmake_minimum_required(VERSION 3.22)
project(TunePXO_COBI VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(TunePXO_COBI
    COMPANY_NAME "TunePXO"
    BUNDLE_ID "com.tunepxo.cobi"
    PLUGIN_MANUFACTURER_CODE TPXO
    PLUGIN_CODE TPCB
    FORMATS VST3 Standalone
    PRODUCT_NAME "TunePXO COBI"
    DESCRIPTION "Vocal tuning plugin"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(TunePXO_COBI PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_compile_features(TunePXO_COBI PRIVATE cxx_std_17)
target_compile_definitions(TunePXO_COBI PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(TunePXO_COBI PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
