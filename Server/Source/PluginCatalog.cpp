/*
  ------------------------------------------------------------------------------
    PluginCatalog.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginCatalog.h"
#include "Log.h"

#include <FxmeNet/catalog/PluginFiles.h>
#include <FxmeTools/net/NetAbi.h>

#include <iostream>
#include <map>

namespace fxmenet::server
{

namespace
{
    const juce::String beginMarker = "FXMENET-SCAN-BEGIN";
    const juce::String endMarker   = "FXMENET-SCAN-END";
    constexpr int childTimeoutMs   = 30000;

    using catalog::pluginBinary;
}

PluginCatalog::PluginCatalog()
{
    formats.addFormat (std::make_unique<juce::VST3PluginFormatHeadless>());
}

juce::String PluginCatalog::bundlePlatformFolder()
{
    return catalog::bundlePlatformFolder();
}

juce::File PluginCatalog::getCacheFile()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("FX-Mechanics").getChildFile ("fxmenet-server-plugins.xml");
}

//==============================================================================
int PluginCatalog::scanOnePlugin (const juce::String& file)
{
    juce::AudioPluginFormatManager manager;
    manager.addFormat (std::make_unique<juce::VST3PluginFormatHeadless>());

    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* format : manager.getFormats())
        if (format->fileMightContainThisPluginType (file))
            format->findAllTypesForFile (found, file);

    const auto sha256 = catalog::binarySha256 (juce::File (file));

    juce::XmlElement root ("FxmeNetScan");
    for (auto* d : found)
    {
        int abi = 0;
        juce::String error;
        if (auto instance = manager.createPluginInstance (*d, 48000.0, 512, error))
        {
            if (auto* vst3 = instance->getVST3Client())
                if (auto* net = fxme::net::queryNetPlugin (vst3->getIComponentPtr()))
                {
                    abi = (int) net->getAbiVersion();
                    net->release();
                }
        }

        auto xml = d->createXml();
        xml->setAttribute ("fxmenetAbi", abi);
        xml->setAttribute ("sha256", sha256);
        root.addChildElement (xml.release());
    }

    // Between markers: a plugin printing on stdout must not corrupt the result.
    std::cout << beginMarker << "\n"
              << root.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader())
              << "\n" << endMarker << std::endl;
    return 0;
}

bool PluginCatalog::scanInChild (const juce::String& file, juce::Array<Entry>& results) const
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    juce::ChildProcess child;
    if (! child.start (juce::StringArray { exe, "--scan-plugin", file }, juce::ChildProcess::wantStdOut))
    {
        log ("  cannot start a scan process for " + file);
        return false;
    }

    // The result is a few kB, well within a pipe's buffer, so the child
    // never blocks writing it before we read.
    if (! child.waitForProcessToFinish (childTimeoutMs))
    {
        child.kill();
        log ("  skipped " + file + ": hung for " + juce::String (childTimeoutMs / 1000) + " s while loaded");
        return false;
    }

    const auto output = child.readAllProcessOutput();
    const auto text = output.fromFirstOccurrenceOf (beginMarker, false, false)
                            .upToFirstOccurrenceOf (endMarker, false, false).trim();
    auto xml = juce::parseXML (text);
    if (! output.contains (endMarker) || xml == nullptr || ! xml->hasTagName ("FxmeNetScan"))
    {
        log ("  skipped " + file + ": crashed while loaded (exit code "
             + juce::String ((int) child.getExitCode()) + ")");
        return false;
    }

    for (auto* e : xml->getChildIterator())
    {
        Entry entry;
        if (entry.description.loadFromXml (*e))
        {
            entry.fxmenetAbi = e->getIntAttribute ("fxmenetAbi");
            entry.sha256 = e->getStringAttribute ("sha256");
            results.add (entry);
        }
    }
    return true;
}

//==============================================================================
int PluginCatalog::scan (const juce::StringArray& extraPaths, bool useDefaultPaths, bool ignoreCache)
{
    entries.clear();

    // The cache: one FILE element per plugin file, with the binary's size
    // and date, and its PLUGIN elements (or failed="1").
    std::unique_ptr<juce::XmlElement> oldCache;
    if (! ignoreCache)
        oldCache = juce::parseXML (getCacheFile());
    std::map<juce::String, const juce::XmlElement*> cached;
    if (oldCache != nullptr && oldCache->hasTagName ("FxmeNetPluginCache"))
        for (auto* f : oldCache->getChildWithTagNameIterator ("FILE"))
            cached[f->getStringAttribute ("path")] = f;

    juce::XmlElement newCache ("FxmeNetPluginCache");
    newCache.setAttribute ("version", 1);
    int examined = 0, fromCache = 0, failed = 0;

    for (auto* format : formats.getFormats())
    {
        juce::FileSearchPath path;
        if (useDefaultPaths)
            path = format->getDefaultLocationsToSearch();
        for (auto& p : extraPaths)
            path.add (juce::File::getCurrentWorkingDirectory().getChildFile (p));

        log ("scanning " + format->getName() + " in " + path.toString());

        for (auto& file : format->searchPathsForPlugins (path, true, false))
        {
            const auto binary = pluginBinary (juce::File (file));
            const auto size = binary.getSize();
            const auto modified = binary.getLastModificationTime().toMilliseconds();

            auto* record = newCache.createNewChildElement ("FILE");
            record->setAttribute ("path", file);
            record->setAttribute ("size", juce::String (size));
            record->setAttribute ("modified", juce::String (modified));

            juce::Array<Entry> results;
            bool ok = true;
            const auto it = cached.find (file);
            if (it != cached.end()
                && it->second->getStringAttribute ("size") == juce::String (size)
                && it->second->getStringAttribute ("modified") == juce::String (modified))
            {
                ++fromCache;
                ok = ! it->second->getBoolAttribute ("failed");
                for (auto* e : it->second->getChildWithTagNameIterator ("PLUGIN"))
                {
                    Entry entry;
                    if (entry.description.loadFromXml (*e))
                    {
                        entry.fxmenetAbi = e->getIntAttribute ("fxmenetAbi");
                        entry.sha256 = e->getStringAttribute ("sha256");
                        results.add (entry);
                    }
                }
            }
            else
            {
                ++examined;
                ok = scanInChild (file, results);
            }

            if (! ok)
            {
                ++failed;
                record->setAttribute ("failed", true);
                continue;
            }

            for (auto& entry : results)
            {
                auto xml = entry.description.createXml();
                xml->setAttribute ("fxmenetAbi", entry.fxmenetAbi);
                xml->setAttribute ("sha256", entry.sha256);
                record->addChildElement (xml.release());

                const auto& d = entry.description;
                if (d.isInstrument)
                    continue;   // FxmeNet hosts effects
                if (find ((std::uint32_t) d.uniqueId, d.name, d.manufacturerName) != nullptr)
                    continue;   // the same plugin in two folders: the first wins
                log ("  " + d.name + " " + d.version + " (" + d.manufacturerName + "), uid "
                     + juce::String::toHexString (d.uniqueId).paddedLeft ('0', 8)
                     + (entry.fxmenetAbi > 0 ? ", FxmeNet-aware" : ""));
                entries.add (entry);
            }
        }
    }

    const auto cacheFile = getCacheFile();
    cacheFile.getParentDirectory().createDirectory();
    if (! newCache.writeTo (cacheFile))
        log ("warning: cannot write the plugin cache " + cacheFile.getFullPathName());

    log (juce::String (entries.size()) + " plugins (" + juce::String (examined) + " examined, "
         + juce::String (fromCache) + " from the cache, " + juce::String (failed) + " failed)");
    return entries.size();
}

std::vector<control::PluginEntry> PluginCatalog::getEntries() const
{
    std::vector<control::PluginEntry> out;
    for (auto& entry : entries)
    {
        const auto& d = entry.description;
        control::PluginEntry e;
        e.uid = (std::uint32_t) d.uniqueId;
        e.name = d.name;
        e.vendor = d.manufacturerName;
        e.version = d.version;
        e.category = d.category;
        e.sha256 = entry.sha256;
        e.inputs = d.numInputChannels;
        e.outputs = d.numOutputChannels;
        e.fxmenetAbi = entry.fxmenetAbi;
        e.platforms.add (bundlePlatformFolder());
        out.push_back (e);
    }
    return out;
}

const juce::PluginDescription* PluginCatalog::find (std::uint32_t uid, const juce::String& name,
                                                    const juce::String& vendor) const
{
    for (auto& entry : entries)
    {
        const auto& d = entry.description;
        if ((std::uint32_t) d.uniqueId == uid
            && (name.isEmpty() || d.name == name)
            && (vendor.isEmpty() || d.manufacturerName == vendor))
            return &d;
    }
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> PluginCatalog::createInstance (const juce::PluginDescription& d,
                                                                          double sampleRate, int blockSize,
                                                                          juce::String& error)
{
    return formats.createPluginInstance (d, sampleRate, blockSize, error);
}

} // namespace fxmenet::server
