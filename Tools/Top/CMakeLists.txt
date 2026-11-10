# fxmenet-top: an fxmenet-server at a glance in a terminal (sessions, load,
# traffic, log) and its plugin folders. Linux, macOS and Windows; connects
# to the server's control port as a monitor (protocol.md section 4.7).

juce_add_console_app(FxmeNetTop PRODUCT_NAME "fxmenet-top")
set_target_properties(FxmeNetTop PROPERTIES OUTPUT_NAME "fxmenet-top")

target_sources(FxmeNetTop PRIVATE main.cpp Terminal.cpp)

target_compile_definitions(FxmeNetTop
    PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        FXMENET_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(FxmeNetTop
    PRIVATE
        FxmeNetProtocol
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
