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

    Where to look: the VST3 default folders (unless turned off), the folders
    saved by fxmenet-top (`fxmenet-server-settings.json`, next to the cache),
    and the --plugin-path folders of this run. The startup scan runs on the
    message thread; later ones (fxmenet-top) on a thread of their own, the
    entries being replaced on the message thread when it ends, so sessions
    go on meanwhile.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/control/ControlMessages.h>

#include <atomic>
#include <mutex>
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

    struct Paths
    {
        juce::StringArray saved;          ///< fxmenet-top's, kept in the settings file
        bool useDefaults = true;          ///< the VST3 default folders
        juce::StringArray commandLine;    ///< --plugin-path, this run only
    };

    /** A scan running or the last one, for fxmenet-top. */
    struct Progress
    {
        bool scanning = false;
        int done = 0, total = 0;          ///< files, while scanning
        juce::String file;                ///< the one being examined
        juce::String summary;             ///< the last scan's result line
    };

    PluginCatalog();
    ~PluginCatalog();

    /** Reads the saved paths and adds this run's. `noDefaultPaths` turns the
        default folders off for this run. Message thread, before scan(). */
    void configure (const juce::StringArray& commandLinePaths, bool noDefaultPaths);

    /** Scans now, on the message thread (startup). Returns how many plugins
        were found. */
    int scan (bool ignoreCache);

    /** Scans again on a background thread. A rescan asked while one runs
        follows it. Message thread. */
    void rescan (bool ignoreCache);

    /** Saves the folders fxmenet-top manages and rescans. False with a
        message when a folder is not an absolute path. Message thread. */
    bool setPaths (const juce::StringArray& saved, bool useDefaults, juce::String& error);

    const Paths& getPaths() const noexcept   { return paths; }

    /** The VST3 default folders of this platform. */
    juce::StringArray getDefaultPaths() const;

    /** Any thread. */
    Progress getProgress() const;

    /** Message thread: changes each time the entries do. */
    int getRevision() const noexcept   { return revision; }

    /** Message thread. */
    const juce::Array<Entry>& getAll() const noexcept   { return entries; }

    std::vector<control::PluginEntry> getEntries() const;

    /** The description of a plugin, or nullptr. Name and vendor must match
        when given (a guard against a uid collision). Message thread; the
        pointer is valid until the entries change (a rescan ending). */
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
    class ScanThread;

    struct Outcome
    {
        juce::Array<Entry> entries;
        juce::String summary;
    };

    /** Where to look now: the default folders (if on), saved, this run's. */
    juce::FileSearchPath searchPath() const;

    /** A whole scan, on any thread; stops early when `abort` is set. */
    Outcome scanFiles (const juce::FileSearchPath&, bool ignoreCache, const std::atomic<bool>& abort);

    /** Message thread: a background scan ended. */
    void finishRescan (Outcome);

    /** Examines one file in a child process; false when it failed or hung. */
    static bool scanInChild (const juce::String& file, juce::Array<Entry>& results, const std::atomic<bool>& abort);

    static juce::File getCacheFile();
    static juce::File getSettingsFile();
    void saveSettings() const;

    juce::AudioPluginFormatManager formats;
    juce::Array<Entry> entries;
    Paths paths;
    int revision = 0;

    std::unique_ptr<ScanThread> scanThread;
    bool rescanAgain = false, rescanAgainFull = false;

    mutable std::mutex progressLock;
    Progress progress;

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginCatalog)
    JUCE_DECLARE_NON_COPYABLE (PluginCatalog)
};

} // namespace fxmenet::server
