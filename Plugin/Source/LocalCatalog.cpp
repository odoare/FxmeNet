/*
  ------------------------------------------------------------------------------
    LocalCatalog.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "LocalCatalog.h"

#include <FxmeNet/catalog/PluginFiles.h>

#include <map>

namespace fxmenet::plugin
{

namespace
{
    juce::File settingsFolder()
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        base = base.getChildFile ("Application Support");
       #endif
        return base.getChildFile ("FX-Mechanics");
    }

    /** Plugins FxmeNet never offers to host locally: itself and its spikes. */
    bool isOwn (const juce::PluginDescription& d)
    {
        return d.manufacturerName == "FX-Mechanics"
            && (d.name == "FxmeNet" || d.name == "FxmeNetSpikeNested");   // not FxmeNetGainPan
    }

    struct Hash
    {
        juce::int64 size = 0, modified = 0;
        juce::String sha256;
    };
}

LocalCatalog::LocalCatalog() : juce::Thread ("FxmeNet local plugin scan")
{
    rescan (false);
}

LocalCatalog::~LocalCatalog()
{
    signalThreadShouldExit();
    stopThread (30000);   // a plugin being scanned cannot be interrupted
}

juce::File LocalCatalog::getCacheFile()
{
    return settingsFolder().getChildFile ("fxmenet-client-plugins.xml");
}

void LocalCatalog::rescan (bool full)
{
    if (scanning.exchange (true))
        return;
    fullRescan = full;
    {
        const std::lock_guard<std::mutex> l (lock);
        progress = "Scanning local plugins";
    }
    revision.fetch_add (1);
    startThread (juce::Thread::Priority::low);
}

juce::String LocalCatalog::getProgress() const
{
    const std::lock_guard<std::mutex> l (lock);
    return progress;
}

int LocalCatalog::getNumPlugins() const
{
    const std::lock_guard<std::mutex> l (lock);
    return (int) entries.size();
}

//==============================================================================
void LocalCatalog::run()
{
    const auto cacheFile = getCacheFile();
    const auto pedalFile = settingsFolder().getChildFile ("fxmenet-client-scan.pedal");
    cacheFile.getParentDirectory().createDirectory();

    // The previous results: the plugin list, and the binaries' hashes.
    juce::KnownPluginList list;
    std::map<juce::String, Hash> hashes;
    if (! fullRescan)
        if (const auto cache = juce::parseXML (cacheFile); cache != nullptr && cache->hasTagName ("FxmeNetClientPlugins"))
        {
            if (auto* known = cache->getChildByName ("KNOWNPLUGINS"))
                list.recreateFromXml (*known);
            for (auto* h : cache->getChildWithTagNameIterator ("HASH"))
                hashes[h->getStringAttribute ("path")] = { h->getStringAttribute ("size").getLargeIntValue(),
                                                           h->getStringAttribute ("modified").getLargeIntValue(),
                                                           h->getStringAttribute ("sha256") };
        }

    // New or changed plugins (in this process: the pedal file names a plugin
    // that crashed it, which is then skipped).
    juce::VST3PluginFormat vst3;
    juce::PluginDirectoryScanner scanner (list, vst3, vst3.getDefaultLocationsToSearch(), true, pedalFile, false);
    juce::String name;
    while (! threadShouldExit())
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            progress = "Scanning local plugins: " + name;
        }
        if (! scanner.scanNextFile (true, name))
            break;
    }

    // Plugins removed since: out of the list.
    for (auto& t : list.getTypes())
        if (! juce::File (t.fileOrIdentifier).exists())
            list.removeType (t);

    // Hashes, for what is new or changed.
    std::vector<Entry> found;
    juce::XmlElement cache ("FxmeNetClientPlugins");
    for (auto& t : list.getTypes())
    {
        if (threadShouldExit())
            break;
        if (t.isInstrument || isOwn (t))
            continue;   // FxmeNet hosts effects

        const auto binary = catalog::pluginBinary (juce::File (t.fileOrIdentifier));
        Hash h { binary.getSize(), binary.getLastModificationTime().toMilliseconds(), {} };
        const auto it = hashes.find (t.fileOrIdentifier);
        if (it != hashes.end() && it->second.size == h.size && it->second.modified == h.modified)
            h.sha256 = it->second.sha256;
        else
            h.sha256 = catalog::binarySha256 (juce::File (t.fileOrIdentifier));
        hashes[t.fileOrIdentifier] = h;
        found.push_back ({ t, h.sha256 });
    }

    if (auto known = list.createXml())
        cache.addChildElement (known.release());
    for (auto& [path, h] : hashes)
        if (juce::File (path).exists())
        {
            auto* e = cache.createNewChildElement ("HASH");
            e->setAttribute ("path", path);
            e->setAttribute ("size", juce::String (h.size));
            e->setAttribute ("modified", juce::String (h.modified));
            e->setAttribute ("sha256", h.sha256);
        }
    if (! threadShouldExit())
        cache.writeTo (cacheFile);

    const auto failed = scanner.getFailedFiles();
    {
        const std::lock_guard<std::mutex> l (lock);
        entries = std::move (found);
        scanned = true;
        progress = juce::String ((int) entries.size()) + " local plugins"
                 + (failed.isEmpty() ? juce::String() : ", " + juce::String (failed.size()) + " failed to scan");
    }
    scanning = false;
    revision.fetch_add (1);
}

//==============================================================================
LocalMatch LocalCatalog::match (const control::PluginEntry& server) const
{
    LocalMatch m;
    const std::lock_guard<std::mutex> l (lock);
    if (! scanned)
    {
        m.text = "local plugins not scanned yet";
        return m;
    }

    const Entry* same = nullptr;
    const Entry* other = nullptr;
    for (auto& e : entries)
    {
        const auto& d = e.description;
        if ((std::uint32_t) d.uniqueId != server.uid || d.name != server.name || d.manufacturerName != server.vendor)
            continue;
        if (d.version == server.version)
            same = &e;
        else if (other == nullptr)
            other = &e;
    }

    if (same == nullptr && other == nullptr)
    {
        m.kind = LocalMatch::Kind::missing;
        m.text = "not installed here: generic controls";
        return m;
    }

    if (same == nullptr)
    {
        m.kind = LocalMatch::Kind::otherVersion;
        m.local = other->description;
        m.text = "version " + other->description.version + " here, " + server.version
               + " on the server: generic controls";
        return m;
    }

    m.kind = LocalMatch::Kind::compatible;
    m.local = same->description;
    m.samePlatform = server.platforms.contains (catalog::bundlePlatformFolder());
    m.sameBinary = m.samePlatform && same->sha256.isNotEmpty() && same->sha256.equalsIgnoreCase (server.sha256);
    m.tier = server.fxmenetAbi > 0 ? 2 : 1;
    m.text = m.tier == 2 ? "its own editor, with the server's meters" : "its own editor";
    if (m.samePlatform && ! m.sameBinary)
        m.text << " (another build of " << server.version << ")";
    return m;
}

} // namespace fxmenet::plugin
