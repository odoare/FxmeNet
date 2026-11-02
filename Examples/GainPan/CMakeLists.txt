# FxmeNetGainPan: the FxmeNet howto plugin. Gain, balance and stereo
# input/output meters, with the full FX-Mechanics shell, made FxmeNet-aware
# with fxme::net::NetBridge (see Source/PluginProcessor.h).
#
# Built from the FxmeNet root CMakeLists.txt, which pulls in JUCE and
# FxmeTools (cmake/Dependencies.cmake).

juce_add_plugin(FxmeNetGainPan
    COMPANY_NAME                "FX-Mechanics"
    COMPANY_WEBSITE             "www.fx-mechanics.com"
    PLUGIN_MANUFACTURER_CODE    FXME
    PLUGIN_CODE                 FnGp
    FORMATS                     VST3 AU
    PRODUCT_NAME                "FxmeNetGainPan"
    DESCRIPTION                 "FX-Mechanics FxmeNet example: gain, balance, meters"
    VERSION                     ${PROJECT_VERSION}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Fx Tools
    BUNDLE_ID                   "com.FXMechanics.FxmeNetGainPan"
)

target_sources(FxmeNetGainPan PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_include_directories(FxmeNetGainPan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# Logo, splash, icon and factory presets. Globbed: anything in Source/Assets
# ends up in the binary, so keep the folder clean.
file(GLOB GAINPAN_ASSETS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/*.png
     ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/*.jpg
     ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/*.xml)
juce_add_binary_data(FxmeNetGainPanBinaryData SOURCES ${GAINPAN_ASSETS})
set_target_properties(FxmeNetGainPanBinaryData PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

juce_generate_juce_header(FxmeNetGainPan)

target_compile_definitions(FxmeNetGainPan
    PUBLIC
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(FxmeNetGainPan
    PRIVATE
        FxmeNetGainPanBinaryData
        FxmeTools
        FxmeCore
        juce::juce_audio_basics
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
