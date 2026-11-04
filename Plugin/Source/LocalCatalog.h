/*
  ------------------------------------------------------------------------------
    LocalCatalog.h

    The VST3 plugins installed on the client (Phase 4, step 8a), and how each
    plugin of the server's catalog compares with its local copy: the tier
    (plan.md section 1) a slot can reach.

      tier 0  no local copy, or another version: generic controls only;
      tier 1  the same version here: its own editor, fed by a local shadow
              instance (8b), flat meters;
      tier 2  as tier 1, and the plugin is FxmeNet-aware on the server: its
              meters and spectra show the server's audio (8c).

    A server plugin and a local one are the same plugin when uid, name and
    vendor match (protocol.md section 4.3); they are compatible when their
    versions are equal. The sha256 only says more when both run on the same
    platform (binaries for different OSes always differ): equal, the very
    same build; different, another build of the same version (shown, not
    refused).

    One scan per process, shared by every FxmeNet instance
    (juce::SharedResourcePointer), on a background thread, with JUCE's
    PluginDirectoryScanner: a plugin that crashes the DAW while scanned is
    named in a "dead man's pedal" file and skipped from then on. Results are
    cached (FX-Mechanics/fxmenet-client-plugins.xml) with each binary's
    sha256, so only new or changed plugins are scanned or hashed again.

    Searched: the VST3 default folders and the user's extra folders
    (AppSettings, set from the Settings tab), with their subfolders. The
    scan thread lives as long as the catalog; a rescan asked for while one
    runs follows it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <FxmeNet/control/ControlMessages.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace fxmenet::plugin
{

/** A server plugin compared with the local plugins. */
struct LocalMatch
{
    enum class Kind
    {
        unknown,        ///< the local scan has not finished yet
        missing,        ///< not installed here
        otherVersion,   ///< installed here in another version
        compatible      ///< the same version here
    };

    Kind kind = Kind::unknown;
    int tier = 0;
    juce::PluginDescription local;   ///< valid unless missing / unknown
    bool samePlatform = false;       ///< the server runs this platform too
    bool sameBinary = false;         ///< ... and its binary is this one (sha256)
    juce::String text;               ///< one line for the editor
};

class LocalCatalog : private juce::Thread
{
public:
    LocalCatalog();
    ~LocalCatalog() override;

    /** Scans again: only what changed, or everything (`full`). While a scan
        runs, another follows it. Any thread. */
    void rescan (bool full);

    /** The folders searched besides the VST3 default ones. Setting them
        saves them (AppSettings) and rescans. Message thread. */
    juce::StringArray getExtraFolders() const;
    void setExtraFolders (const juce::StringArray& folders);

    bool isScanning() const noexcept           { return scanning.load(); }
    juce::String getProgress() const;
    int getNumPlugins() const;

    /** Increases whenever the results change (for displays). */
    std::uint32_t getRevision() const noexcept { return revision.load(); }

    LocalMatch match (const control::PluginEntry& server) const;

    static juce::File getCacheFile();

private:
    struct Entry
    {
        juce::PluginDescription description;
        juce::String sha256;
    };

    void run() override;
    void scan (bool full);

    mutable std::mutex lock;
    std::vector<Entry> entries;
    bool scanned = false;
    juce::String progress;
    juce::StringArray extraFolders;
    bool requested = false, fullRequested = false;   // under `lock`

    std::atomic<bool> scanning { false };
    std::atomic<std::uint32_t> revision { 0 };

    JUCE_DECLARE_NON_COPYABLE (LocalCatalog)
};

} // namespace fxmenet::plugin
