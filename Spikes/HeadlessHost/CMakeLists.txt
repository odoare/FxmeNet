# Phase 0, spike 1: load a JUCE VST3 with the GUI-free JUCE 9 hosting module,
# process a WAV file, round-trip the state. See Main.cpp for what it checks.
#
# Deliberately links no GUI module: if this builds and runs with DISPLAY
# unset, the server can be built the same way.

juce_add_console_app(FxmeNetSpikeHeadless
    PRODUCT_NAME "fxmenet-spike-headless")

target_sources(FxmeNetSpikeHeadless PRIVATE Main.cpp)

target_compile_definitions(FxmeNetSpikeHeadless
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        # runDispatchLoopUntil(), used to let the plugin settle before processing.
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_link_libraries(FxmeNetSpikeHeadless
    PRIVATE
        juce::juce_audio_processors_headless
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
