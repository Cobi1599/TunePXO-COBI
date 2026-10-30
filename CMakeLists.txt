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

juce_generate_juce_header(TunePXO_COBI)

target_sources(TunePXO_COBI PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    ThirdParty/psola/sola.cpp
)
target_include_directories(TunePXO_COBI PRIVATE ThirdParty/psola)
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

# Exercise the PSOLA processor with synthetic voiced signals in CI.
enable_testing()
add_executable(PsolaSmokeTest
    Tests/PsolaSmokeTest.cpp
    ThirdParty/psola/sola.cpp
)
target_include_directories(PsolaSmokeTest PRIVATE Source ThirdParty/psola)
target_compile_features(PsolaSmokeTest PRIVATE cxx_std_17)
target_link_libraries(PsolaSmokeTest PRIVATE
    juce::juce_audio_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME PsolaSmokeTest COMMAND PsolaSmokeTest)


# Exercise the complete AudioProcessor paths, including host-reported latency.
juce_add_console_app(ProcessorIntegrationTest PRODUCT_NAME "Processor Integration Test")
target_sources(ProcessorIntegrationTest PRIVATE
    Tests/ProcessorIntegrationTest.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    ThirdParty/psola/sola.cpp
)
juce_generate_juce_header(ProcessorIntegrationTest)
target_include_directories(ProcessorIntegrationTest PRIVATE Source ThirdParty/psola)
target_compile_features(ProcessorIntegrationTest PRIVATE cxx_std_17)
target_compile_definitions(ProcessorIntegrationTest PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(ProcessorIntegrationTest PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME ProcessorIntegrationTest COMMAND ProcessorIntegrationTest)
