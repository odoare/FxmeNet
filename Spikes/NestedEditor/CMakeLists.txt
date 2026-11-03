# Phase 0, spike 3: a JUCE plugin that hosts a VST3 and embeds the hosted
# plugin's own editor in its window (on Linux, an XEmbed window inside the
# DAW's). FxmeNet's Phase 4 shows the slots' editors this way. See Main.cpp
# for what to check.

juce_add_plugin(FxmeNetSpikeNested
    COMPANY_NAME                "FX-Mechanics"
    PLUGIN_MANUFACTURER_CODE    FXME
    PLUGIN_CODE                 FnSn
    FORMATS                     VST3
    PRODUCT_NAME                "FxmeNetSpikeNested"
    DESCRIPTION                 "FxmeNet spike 3: a hosted VST3 editor nested in a plugin window"
    VERSION                     ${PROJECT_VERSION}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Fx
    BUNDLE_ID                   "com.FXMechanics.FxmeNetSpikeNested"
)

target_sources(FxmeNetSpikeNested PRIVATE Main.cpp)

target_compile_definitions(FxmeNetSpikeNested
    PUBLIC
        JUCE_PLUGINHOST_VST3=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(FxmeNetSpikeNested
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
