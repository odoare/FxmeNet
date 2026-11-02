# ──────────────────────────────────────────────────────────────────────────
# Dependencies.cmake — FxmeNet's pinned dependencies.
#
# Included after project() by the root CMakeLists.txt. Resolution (sibling
# clone or pinned commit) is FxmeDeps.cmake's.
# ──────────────────────────────────────────────────────────────────────────

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/FxmeDeps.cmake)

fxme_dependency(JUCE https://github.com/juce-framework/JUCE 9.0.4 SHALLOW)
fxme_dependency(FxmeTools https://github.com/odoare/FxmeTools
    1a767df4182561272001cdd5f24820e763b3b826)

if(NOT COMMAND juce_add_plugin)
    add_subdirectory(${JUCE_DIR} JUCE)
endif()

# Registers the `FxmeTools` JUCE module, FxmeCore and fxmetools_attach().
include(${FXMETOOLS_DIR}/cmake/FxmeTools.cmake)
