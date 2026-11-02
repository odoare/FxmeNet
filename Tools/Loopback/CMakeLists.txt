# FxmeNetLoopback: Phase 1's test harness, no network involved. Loads an
# FxmeNet-aware VST3 twice in one process, a source that processes a test
# signal and a mirror that never processes, shows both editors side by side,
# and pumps telemetry (and parameters, both ways) between them in memory.
# The mirror's meters moving is the proof that the whole plugin-side chain
# works: NetBridge, the VST3 side channel, TelemetryRegistry, the codec.

juce_add_gui_app(FxmeNetLoopback
    PRODUCT_NAME "FxmeNetLoopback"
    COMPANY_NAME "FX-Mechanics")

target_sources(FxmeNetLoopback PRIVATE Main.cpp)

target_compile_definitions(FxmeNetLoopback
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:FxmeNetLoopback,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:FxmeNetLoopback,JUCE_VERSION>"
)

target_link_libraries(FxmeNetLoopback
    PRIVATE
        FxmeCore
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
