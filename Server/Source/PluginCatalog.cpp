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
#include <thread>

namespace fxmenet::server
{

namespace
{
    const juce::String beginMarker = "FXMENET-SCAN-BEGIN";
    const juce::String endMarker   = "FXMENET-SCAN-END";
    constexpr int childTimeoutMs   = 60000;   ///< a bundle of 194 plugins (LSP) takes about 10 s

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

bool PluginCatalog::scanInChild (const juce::String& file, juce::Array<Entry>& results, const std::atomic<bool>& abort)
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    juce::ChildProcess child;
    if (! child.start (juce::StringArray { exe, "--scan-plugin", file }, juce::ChildProcess::wantStdOut))
    {
        log ("  cannot start a scan process for " + file);
        return false;
    }

    // Read while it runs: a bundle of many plugins writes more than a
    // pipe holds (LSP's 194 plugins: 80 kB, against 64 kB), and a child
    // blocked on a full pipe never finishes (it was then killed as hung,
    // and the bundle cached as failed). The reader returns at the end of
    // the output: the child exiting, or killed.
    juce::String output;
    std::thread reader ([&child, &output] { output = child.readAllProcessOutput(); });
    bool finished = false;
    for (int waited = 0; ! finished && waited < childTimeoutMs && ! abort; waited += 250)
        finished = child.waitForProcessToFinish (250);
    if (! finished)
        child.kill();
    reader.join();
    if (! finished)
    {
        if (! abort)
            log ("  skipped " + file + ": hung for " + juce::String (childTimeoutMs / 1000) + " s while loaded");
        return false;
    }

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
/** A rescan asked by fxmenet-top. */
class PluginCatalog::ScanThread : public juce::Thread
{
public:
    ScanThread (PluginCatalog& c, juce::FileSearchPath p, bool full)
        : juce::Thread ("FxmeNet plugin scan"), catalog (c), path (std::move (p)), ignoreCache (full) {}

    ~ScanThread() override
    {
        abort = true;
        stopThread (5000);
    }

    void run() override
    {
        auto outcome = catalog.scanFiles (path, ignoreCache, abort);
        if (abort)
            return;
        juce::WeakReference<PluginCatalog> weak (&catalog);
        juce::MessageManager::callAsync ([weak, outcome = std::move (outcome)]() mutable
        {
            if (auto* c = weak.get())
                c->finishRescan (std::move (outcome));
        });
    }

private:
    PluginCatalog& catalog;
    const juce::FileSearchPath path;
    const bool ignoreCache;
    std::atomic<bool> abort { false };
};

//==============================================================================
PluginCatalog::~PluginCatalog()
{
    scanThread.reset();   // stops a scan (and its child) before the rest goes
}

juce::File PluginCatalog::getSettingsFile()
{
    return getCacheFile().getSiblingFile ("fxmenet-server-settings.json");
}

void PluginCatalog::configure (const juce::StringArray& commandLinePaths, bool noDefaultPaths)
{
    const auto settings = juce::JSON::parse (getSettingsFile());
    paths.saved = control::json::toStringArray (settings["pluginPaths"]);
    paths.useDefaults = settings.hasProperty ("useDefaultPaths") ? (bool) settings["useDefaultPaths"] : true;
    if (noDefaultPaths)
        paths.useDefaults = false;   // this run only: not saved

    paths.commandLine.clear();
    for (auto& p : commandLinePaths)
        paths.commandLine.add (juce::File::getCurrentWorkingDirectory().getChildFile (p).getFullPathName());
}

void PluginCatalog::saveSettings() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("pluginPaths", control::json::stringArray (paths.saved));
    o->setProperty ("useDefaultPaths", paths.useDefaults);
    const auto file = getSettingsFile();
    file.getParentDirectory().createDirectory();
    if (! file.replaceWithText (juce::JSON::toString (juce::var (o))))
        log ("warning: cannot write the settings " + file.getFullPathName());
}

bool PluginCatalog::setPaths (const juce::StringArray& saved, bool useDefaults, juce::String& error)
{
    juce::StringArray clean;
    for (auto p : saved)
    {
        p = p.trim();
        if (p.isEmpty())
            continue;
        if (! juce::File::isAbsolutePath (p))
        {
            error = "not an absolute path: " + p;
            return false;
        }
        clean.addIfNotAlreadyThere (juce::File (p).getFullPathName());
    }

    paths.saved = clean;
    paths.useDefaults = useDefaults;
    saveSettings();
    log ("plugin paths: " + (useDefaults ? juce::String ("default folders, ") : juce::String ("no default folders, "))
         + (clean.isEmpty() ? juce::String ("no saved folder") : clean.joinIntoString (", ")));
    rescan (false);
    return true;
}

juce::StringArray PluginCatalog::getDefaultPaths() const
{
    juce::StringArray out;
    for (auto* format : formats.getFormats())
    {
        const auto path = format->getDefaultLocationsToSearch();
        for (int i = 0; i < path.getNumPaths(); ++i)
            out.addIfNotAlreadyThere (path.getRawString (i));
    }
    return out;
}

juce::FileSearchPath PluginCatalog::searchPath() const
{
    juce::FileSearchPath path;
    if (paths.useDefaults)
        for (auto& p : getDefaultPaths())
            path.add (juce::File (p));
    for (auto& p : paths.saved)
        path.addIfNotAlreadyThere (juce::File (p));
    for (auto& p : paths.commandLine)
        path.addIfNotAlreadyThere (juce::File (p));
    return path;
}

PluginCatalog::Progress PluginCatalog::getProgress() const
{
    const std::lock_guard<std::mutex> lock (progressLock);
    return progress;
}

//==============================================================================
int PluginCatalog::scan (bool ignoreCache)
{
    const std::atomic<bool> never { false };
    auto outcome = scanFiles (searchPath(), ignoreCache, never);
    entries = std::move (outcome.entries);
    ++revision;
    return entries.size();
}

void PluginCatalog::rescan (bool ignoreCache)
{
    // The thread, not the progress: the progress says "done" a moment
    // before finishRescan runs, and a scan started in between would be
    // the one finishRescan drops.
    if (scanThread != nullptr)
    {
        rescanAgain = true;
        rescanAgainFull = rescanAgainFull || ignoreCache;
        return;
    }
    {
        const std::lock_guard<std::mutex> lock (progressLock);
        progress.scanning = true;   // at once: a status asked now says so
        progress.done = progress.total = 0;
        progress.file = {};
    }
    scanThread = std::make_unique<ScanThread> (*this, searchPath(), ignoreCache);
    scanThread->startThread();
}

void PluginCatalog::finishRescan (Outcome outcome)
{
    scanThread.reset();   // its run() has returned: this joins at once
    entries = std::move (outcome.entries);
    ++revision;

    if (rescanAgain)
    {
        const bool full = rescanAgainFull;
        rescanAgain = rescanAgainFull = false;
        rescan (full);
    }
}

PluginCatalog::Outcome PluginCatalog::scanFiles (const juce::FileSearchPath& path, bool ignoreCache,
                                                 const std::atomic<bool>& abort)
{
    Outcome outcome;
    auto& found = outcome.entries;

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

    // A format of its own: this may run on a background thread.
    juce::VST3PluginFormatHeadless format;
    log ("scanning " + format.getName() + " in " + path.toString());
    const auto files = format.searchPathsForPlugins (path, true, false);
    {
        const std::lock_guard<std::mutex> lock (progressLock);
        progress.scanning = true;
        progress.done = 0;
        progress.total = files.size();
    }

    const auto isKnown = [&found] (const juce::PluginDescription& d)
    {
        for (auto& e : found)
            if (e.description.uniqueId == d.uniqueId && e.description.name == d.name
                && e.description.manufacturerName == d.manufacturerName)
                return true;
        return false;
    };

    for (auto& file : files)
    {
        if (abort)
            break;
        {
            const std::lock_guard<std::mutex> lock (progressLock);
            progress.file = file;
        }

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
            ok = scanInChild (file, results, abort);
        }

        {
            const std::lock_guard<std::mutex> lock (progressLock);
            ++progress.done;
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
            if (isKnown (d))
                continue;   // the same plugin in two folders: the first wins
            log ("  " + d.name + " " + d.version + " (" + d.manufacturerName + "), uid "
                 + juce::String::toHexString (d.uniqueId).paddedLeft ('0', 8)
                 + (entry.fxmenetAbi > 0 ? ", FxmeNet-aware" : ""));
            found.add (entry);
        }
    }

    if (abort)
        return outcome;   // an incomplete cache is not written

    const auto cacheFile = getCacheFile();
    cacheFile.getParentDirectory().createDirectory();
    if (! newCache.writeTo (cacheFile))
        log ("warning: cannot write the plugin cache " + cacheFile.getFullPathName());

    outcome.summary = juce::String (found.size()) + " plugins (" + juce::String (examined) + " examined, "
                    + juce::String (fromCache) + " from the cache, " + juce::String (failed) + " failed)";
    log (outcome.summary);
    {
        const std::lock_guard<std::mutex> lock (progressLock);
        progress.scanning = false;
        progress.file = {};
        progress.summary = juce::Time::getCurrentTime().formatted ("%H:%M:%S") + "  " + outcome.summary;
    }
    return outcome;
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
