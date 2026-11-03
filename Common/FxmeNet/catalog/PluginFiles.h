/*
  ------------------------------------------------------------------------------
    PluginFiles.h

    What both sides need to know about a VST3 on disk, so that a plugin on
    the server and its copy on the client can be compared (protocol.md
    section 4.3): the folder of a bundle holding this platform's binary
    (also the platform name the catalog gives), that binary, and its sha256.

    Header-only, needs juce_core and juce_cryptography.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace fxmenet::catalog
{

/** "x86_64-linux", "aarch64-linux", "MacOS", "x86_64-win" or "arm64-win". */
inline juce::String bundlePlatformFolder()
{
   #if JUCE_MAC
    return "MacOS";
   #elif JUCE_WINDOWS
    #if JUCE_ARM
     return "arm64-win";
    #else
     return "x86_64-win";
    #endif
   #else
    #if JUCE_ARM
     return "aarch64-linux";
    #else
     return "x86_64-linux";
    #endif
   #endif
}

/** The file holding this platform's code: the plugin file itself, or the
    binary inside a VST3 bundle; File() when there is none. */
inline juce::File pluginBinary (const juce::File& plugin)
{
    if (plugin.existsAsFile())
        return plugin;

    auto folder = plugin.getChildFile ("Contents").getChildFile (bundlePlatformFolder());
    auto files = folder.findChildFiles (juce::File::findFiles, false);
    files.sort();
    return files.isEmpty() ? juce::File() : files.getFirst();
}

/** The sha256 of a plugin's binary for this platform, in hex; empty when
    there is no binary. Reads the whole file: not on the audio thread. */
inline juce::String binarySha256 (const juce::File& plugin)
{
    const auto binary = pluginBinary (plugin);
    return binary.existsAsFile() ? juce::SHA256 (binary).toHexString() : juce::String();
}

} // namespace fxmenet::catalog
