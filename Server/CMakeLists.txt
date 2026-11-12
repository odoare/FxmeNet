# fxmenet-server: the headless FxmeNet daemon. Hosts VST3 chains for its
# clients, with JUCE's GUI-free hosting module only (no display needed,
# see plan.md Phase 0 results).

juce_add_console_app(FxmeNetServer PRODUCT_NAME "fxmenet-server")
# juce_add_console_app ignores PRODUCT_NAME for the file name.
set_target_properties(FxmeNetServer PROPERTIES OUTPUT_NAME "fxmenet-server")

target_sources(FxmeNetServer PRIVATE
    Source/Main.cpp
    Source/PluginCatalog.cpp
    Source/Chain.cpp
    Source/AudioWorker.cpp
    Source/Session.cpp
    Source/Server.cpp
    Source/Monitor.cpp
    Source/SystemLoad.cpp
)

target_include_directories(FxmeNetServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

target_compile_definitions(FxmeNetServer
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        FXMENET_VERSION="${PROJECT_VERSION}"
)

# JSFX scripts in the chain (doc/jsfx-plan.md).
if(FXMENET_JSFX)
    target_sources(FxmeNetServer PRIVATE Source/JsfxInstance.cpp)
    target_compile_definitions(FxmeNetServer PRIVATE FXMENET_JSFX=1)
    target_link_libraries(FxmeNetServer PRIVATE fxjsfx)
else()
    target_compile_definitions(FxmeNetServer PRIVATE FXMENET_JSFX=0)
endif()

# SystemLoad: the process's memory (GetProcessMemoryInfo).
if(WIN32)
    target_link_libraries(FxmeNetServer PRIVATE psapi)
endif()

target_link_libraries(FxmeNetServer
    PRIVATE
        FxmeNetProtocol
        FxmeCore
        juce::juce_audio_processors_headless
        juce::juce_core
        juce::juce_cryptography
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
