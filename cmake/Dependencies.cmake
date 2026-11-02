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
    94e4e4d9147bf9ba51db0c88814eea60104d13a2)

if(NOT COMMAND juce_add_plugin)
    add_subdirectory(${JUCE_DIR} JUCE)
endif()

# Registers the `FxmeTools` JUCE module, FxmeCore and fxmetools_attach().
include(${FXMETOOLS_DIR}/cmake/FxmeTools.cmake)
