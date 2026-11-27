# The FxmeNet client in the DAW, in two variants built from the same sources
# (doc/instrument-plan.md):
#   FxmeNetPlugin      "FxmeNet", an audio effect;
#   FxmeNetInstrument  "FxmeNet Instrument", an instrument (no audio input,
#                      FXMENET_INSTRUMENT=1), so that DAWs put it on
#                      instrument tracks.
# Each hosts a chain of plugins on an fxmenet-server, streams the audio (and
# MIDI) to it and back, and exposes 256 generic parameters mapped to the
# chain's parameters.
#
# Built from the FxmeNet root CMakeLists.txt, which pulls in JUCE and
# FxmeTools (cmake/Dependencies.cmake). CLAP comes later
# (fxmetools_add_clap, once clap-juce-extensions is pinned).

# Logo and icon. Globbed: anything in Source/Assets ends up in the binary.
file(GLOB FXMENET_PLUGIN_ASSETS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/*.png)
juce_add_binary_data(FxmeNetPluginBinaryData SOURCES ${FXMENET_PLUGIN_ASSETS})
set_target_properties(FxmeNetPluginBinaryData PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# One variant: the JUCE plugin target, then what both share. The sources are
# compiled once per variant (JUCE's plugin definitions are per target).
function(fxmenet_add_client target)
    cmake_parse_arguments(ARG "" "PRODUCT_NAME;PLUGIN_CODE;BUNDLE_ID;IS_SYNTH;INSTRUMENT" "VST3_CATEGORIES" ${ARGN})

    juce_add_plugin(${target}
        COMPANY_NAME                "FX-Mechanics"
        COMPANY_WEBSITE             "www.fx-mechanics.com"
        PLUGIN_MANUFACTURER_CODE    FXME
        PLUGIN_CODE                 ${ARG_PLUGIN_CODE}
        FORMATS                     VST3 AU
        PRODUCT_NAME                "${ARG_PRODUCT_NAME}"
        DESCRIPTION                 "FX-Mechanics FxmeNet: plugins processed on a server on the network"
        VERSION                     ${PROJECT_VERSION}
        IS_SYNTH                    ${ARG_IS_SYNTH}
        NEEDS_MIDI_INPUT            TRUE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        VST3_CATEGORIES             ${ARG_VST3_CATEGORIES}
        BUNDLE_ID                   "${ARG_BUNDLE_ID}"
    )

    target_sources(${target} PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Panels.cpp
        Source/RoutingView.cpp
        Source/LocalCatalog.cpp
    )

    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_compile_definitions(${target} PRIVATE FXMENET_INSTRUMENT=${ARG_INSTRUMENT})

    # JSFX scripts' @gfx display, drawn here from the server's data (doc/jsfx-plan.md).
    if(FXMENET_JSFX)
        target_sources(${target} PRIVATE Source/JsfxView.cpp
                                         ${CMAKE_SOURCE_DIR}/Common/FxmeNet/jsfx/JsfxInstance.cpp)   # local play
        target_compile_definitions(${target} PRIVATE FXMENET_JSFX=1)
        target_link_libraries(${target} PRIVATE fxjsfx)
    else()
        target_compile_definitions(${target} PRIVATE FXMENET_JSFX=0)
    endif()

    juce_generate_juce_header(${target})

    target_compile_definitions(${target}
        PUBLIC
            JUCE_PLUGINHOST_VST3=1   # the local plugins: scanned (8a), then shadows (8b)
            JUCE_STRICT_REFCOUNTEDPOINTER=1
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
    )

    target_link_libraries(${target}
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
endfunction()

fxmenet_add_client(FxmeNetPlugin
    PRODUCT_NAME    "FxmeNet"
    PLUGIN_CODE     FnPl
    BUNDLE_ID       "com.FXMechanics.FxmeNet"
    IS_SYNTH        FALSE
    INSTRUMENT      0
    VST3_CATEGORIES Fx Network)

fxmenet_add_client(FxmeNetInstrument
    PRODUCT_NAME    "FxmeNet Instrument"
    PLUGIN_CODE     FnIn
    BUNDLE_ID       "com.FXMechanics.FxmeNetInstrument"
    IS_SYNTH        TRUE
    INSTRUMENT      1
    VST3_CATEGORIES Instrument Network)
