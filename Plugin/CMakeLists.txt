# FxmeNetPlugin: the FxmeNet client in the DAW. Hosts a chain of plugins on
# an fxmenet-server, streams the audio to it and back, and exposes 256
# generic parameters mapped to the chain's parameters (Phase 3, tier 0).
#
# Built from the FxmeNet root CMakeLists.txt, which pulls in JUCE and
# FxmeTools (cmake/Dependencies.cmake). CLAP comes later
# (fxmetools_add_clap, once clap-juce-extensions is pinned).

juce_add_plugin(FxmeNetPlugin
    COMPANY_NAME                "FX-Mechanics"
    COMPANY_WEBSITE             "www.fx-mechanics.com"
    PLUGIN_MANUFACTURER_CODE    FXME
    PLUGIN_CODE                 FnPl
    FORMATS                     VST3 AU
    PRODUCT_NAME                "FxmeNet"
    DESCRIPTION                 "FX-Mechanics FxmeNet: plugins processed on a server on the network"
    VERSION                     ${PROJECT_VERSION}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Fx Network
    BUNDLE_ID                   "com.FXMechanics.FxmeNet"
)

target_sources(FxmeNetPlugin PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Panels.cpp
    Source/LocalCatalog.cpp
)

target_include_directories(FxmeNetPlugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# Logo and icon. Globbed: anything in Source/Assets ends up in the binary.
file(GLOB FXMENET_PLUGIN_ASSETS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/*.png)
juce_add_binary_data(FxmeNetPluginBinaryData SOURCES ${FXMENET_PLUGIN_ASSETS})
set_target_properties(FxmeNetPluginBinaryData PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

juce_generate_juce_header(FxmeNetPlugin)

target_compile_definitions(FxmeNetPlugin
    PUBLIC
        JUCE_PLUGINHOST_VST3=1   # the local plugins: scanned (8a), then shadows (8b)
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(FxmeNetPlugin
    PRIVATE
        FxmeNetPluginBinaryData
        FxmeNetProtocol
        FxmeTools
        FxmeCore
        juce::juce_audio_basics
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_cryptography
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
