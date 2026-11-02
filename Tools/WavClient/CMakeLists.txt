# FxmeNetWavClient: Phase 2's end-to-end test. Streams a WAV file through a
# chain of plugins on an fxmenet-server, writes the result, and compares it
# with the same chain processed locally.

juce_add_console_app(FxmeNetWavClient PRODUCT_NAME "FxmeNetWavClient")
set_target_properties(FxmeNetWavClient PROPERTIES OUTPUT_NAME "fxmenet-wavclient")

target_sources(FxmeNetWavClient PRIVATE Main.cpp)

target_compile_definitions(FxmeNetWavClient
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1   # runDispatchLoopUntil, to let local plugins settle
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_link_libraries(FxmeNetWavClient
    PRIVATE
        FxmeNetProtocol
        juce::juce_audio_processors_headless
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
