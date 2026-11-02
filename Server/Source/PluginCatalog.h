/*
  ------------------------------------------------------------------------------
    PluginCatalog.h

    The VST3 plugins the server can load: found at startup, listed to clients
    (PLUGIN_LIST), and looked up by the identity protocol.md section 4.3
    defines (uid, name, vendor).

    Each plugin file is examined in a child process (`fxmenet-server
    --scan-plugin FILE`, see scanOnePlugin), so a plugin that crashes or hangs
    while loaded is skipped instead of taking the server down. The child also
    instantiates the plugin once to read its FxmeNet ABI version, and hashes
    the plugin's binary for this platform (sha256). Results are cached by
    file, size and modification time, so only new or changed plugins are
    examined again; --rescan ignores the cache.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/control/ControlMessages.h>

#include <vector>

namespace fxmenet::server
{

class PluginCatalog
{
public:
    struct Entry
    {
        juce::PluginDescription description;
        int fxmenetAbi = 0;        ///< 0: not FxmeNet-aware
        juce::String sha256;       ///< of the binary for this platform
    };

    PluginCatalog();

    /** Finds the plugins in the default VST3 locations (unless told not to)
        and `extraPaths`. Message thread. Returns how many were found. */
    int scan (const juce::StringArray& extraPaths, bool useDefaultPaths, bool ignoreCache);

    std::vector<control::PluginEntry> getEntries() const;

    /** The description of a plugin, or nullptr. Name and vendor must match
        when given (a guard against a uid collision). */
    const juce::PluginDescription* find (std::uint32_t uid, const juce::String& name,
                                         const juce::String& vendor) const;

    /** Creates an instance. Message thread. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error);

    /** The child process's work: examines one plugin file and prints the
        result on stdout between markers. Returns the process exit code. */
    static int scanOnePlugin (const juce::String& file);

    /** The folder of a VST3 bundle holding this platform's binary
        ("x86_64-linux", "MacOS", "x86_64-win"...): also the platform name
        given in the catalog. */
    static juce::String bundlePlatformFolder();

private:
    /** Examines one file in a child process; false when it failed or hung. */
    bool scanInChild (const juce::String& file, juce::Array<Entry>& results) const;

    static juce::File getCacheFile();

    juce::AudioPluginFormatManager formats;
    juce::Array<Entry> entries;

    JUCE_DECLARE_NON_COPYABLE (PluginCatalog)
};

} // namespace fxmenet::server
