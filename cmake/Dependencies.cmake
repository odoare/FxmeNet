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
    b2b4e458baa55b7dc80f122f036089311da7c712)

# The JSFX engine (Cockos' sources as a library, doc/jsfx-plan.md): the
# fork's branch fxmenet-branch. Locally the clone is ~/src/jsfx.
option(FXMENET_JSFX "JSFX scripts as plugins of the chain (needs nasm on x86-64 Linux)" ON)
if(FXMENET_JSFX)
    fxme_dependency(jsfx https://github.com/odoare/jsfx
        66faf49948b7c4954345344359eb01a676c5577e SIBLING jsfx)
endif()

if(NOT COMMAND juce_add_plugin)
    add_subdirectory(${JUCE_DIR} JUCE)
endif()

# Registers the `FxmeTools` JUCE module, FxmeCore and fxmetools_attach().
include(${FXMETOOLS_DIR}/cmake/FxmeTools.cmake)
