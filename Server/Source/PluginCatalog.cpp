/*
  ------------------------------------------------------------------------------
    PluginCatalog.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginCatalog.h"
#include "Log.h"

namespace fxmenet::server
{

namespace
{
    juce::String platformName()
    {
       #if JUCE_LINUX
        return "x86_64-linux";   // the server's own platform; arm64 builds will say so later
       #elif JUCE_MAC
        return "macos";
       #elif JUCE_WINDOWS
        return "x86_64-win";
       #else
        return "unknown";
       #endif
    }
}

PluginCatalog::PluginCatalog()
{
    formats.addFormat (std::make_unique<juce::VST3PluginFormatHeadless>());
}

int PluginCatalog::scan (const juce::StringArray& extraPaths, bool useDefaultPaths)
{
    plugins.clear();

    for (auto* format : formats.getFormats())
    {
        juce::FileSearchPath path;
        if (useDefaultPaths)
            path = format->getDefaultLocationsToSearch();
        for (auto& p : extraPaths)
            path.add (juce::File::getCurrentWorkingDirectory().getChildFile (p));

        log ("scanning " + format->getName() + " in " + path.toString());
        const auto files = format->searchPathsForPlugins (path, true, false);

        for (auto& file : files)
        {
            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, file);
            for (auto* d : found)
            {
                if (d->isInstrument)
                    continue;   // FxmeNet hosts effects
                if (find ((std::uint32_t) d->uniqueId, d->name, d->manufacturerName) != nullptr)
                    continue;   // the same plugin in two folders: the first wins
                log ("  " + d->name + " " + d->version + " (" + d->manufacturerName + "), uid "
                     + juce::String::toHexString (d->uniqueId).paddedLeft ('0', 8));
                plugins.add (new juce::PluginDescription (*d));
            }
        }
    }

    log (juce::String (plugins.size()) + " plugins");
    return plugins.size();
}

std::vector<control::PluginEntry> PluginCatalog::getEntries() const
{
    std::vector<control::PluginEntry> entries;
    for (auto* d : plugins)
    {
        control::PluginEntry e;
        e.uid = (std::uint32_t) d->uniqueId;
        e.name = d->name;
        e.vendor = d->manufacturerName;
        e.version = d->version;
        e.category = d->category;
        e.inputs = d->numInputChannels;
        e.outputs = d->numOutputChannels;
        e.platforms.add (platformName());
        entries.push_back (e);
    }
    return entries;
}

const juce::PluginDescription* PluginCatalog::find (std::uint32_t uid, const juce::String& name,
                                                    const juce::String& vendor) const
{
    for (auto* d : plugins)
        if ((std::uint32_t) d->uniqueId == uid
            && (name.isEmpty() || d->name == name)
            && (vendor.isEmpty() || d->manufacturerName == vendor))
            return d;
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> PluginCatalog::createInstance (const juce::PluginDescription& d,
                                                                          double sampleRate, int blockSize,
                                                                          juce::String& error)
{
    return formats.createPluginInstance (d, sampleRate, blockSize, error);
}

} // namespace fxmenet::server
