# FxmeNet's shared code: the wire protocol (JUCE-free) and the audio
# transports (JUCE). Header-only: JUCE code in a static library linked into
# several JUCE targets gives duplicate symbols.

add_library(FxmeNetProtocol INTERFACE)
target_include_directories(FxmeNetProtocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(FxmeNetProtocol INTERFACE FxmeCore)
target_compile_features(FxmeNetProtocol INTERFACE cxx_std_17)

# ── Tests ─────────────────────────────────────────────────────────────────────
# The wire format and fragmentation, without JUCE: seconds to build.
add_executable(FxmeNetProtocolTests tests/ProtocolTests.cpp)
target_link_libraries(FxmeNetProtocolTests PRIVATE FxmeNetProtocol)
target_compile_options(FxmeNetProtocolTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
add_test(NAME FxmeNetProtocolTests COMMAND FxmeNetProtocolTests)

# The client's audio-thread core (re-blocking, latency, jitter buffer,
# concealment, reply ring), without JUCE.
add_executable(FxmeNetClientTests tests/ClientTests.cpp)
target_link_libraries(FxmeNetClientTests PRIVATE FxmeNetProtocol)
target_compile_options(FxmeNetClientTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
add_test(NAME FxmeNetClientTests COMMAND FxmeNetClientTests)

# The routing model between a chain's plugins (doc/routing.md), without JUCE.
add_executable(FxmeNetRoutingTests tests/RoutingTests.cpp)
target_link_libraries(FxmeNetRoutingTests PRIVATE FxmeNetProtocol)
target_compile_options(FxmeNetRoutingTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
add_test(NAME FxmeNetRoutingTests COMMAND FxmeNetRoutingTests)

# UdpTransport over the loopback interface (juce_core only).
juce_add_console_app(FxmeNetTransportTests PRODUCT_NAME "FxmeNetTransportTests")
target_sources(FxmeNetTransportTests PRIVATE tests/TransportTests.cpp)
target_compile_definitions(FxmeNetTransportTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(FxmeNetTransportTests
    PRIVATE
        FxmeNetProtocol
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
add_test(NAME FxmeNetTransportTests COMMAND FxmeNetTransportTests)

# The control channel: JSON messages and a TCP exchange on loopback.
juce_add_console_app(FxmeNetControlTests PRODUCT_NAME "FxmeNetControlTests")
target_sources(FxmeNetControlTests PRIVATE tests/ControlTests.cpp)
target_compile_definitions(FxmeNetControlTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(FxmeNetControlTests
    PRIVATE
        FxmeNetProtocol
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
add_test(NAME FxmeNetControlTests COMMAND FxmeNetControlTests)
