/*
  ------------------------------------------------------------------------------
    JsfxScripts.h

    JSFX scripts on the client's disk (doc/jsfx-plan.md): finding them in an
    Effects folder (REAPER's, or another laid out the same way), and packing
    one with what it needs into a jsfx::Bundle for the server:

      - the files it imports ("import name"), found as the JSFX loader finds
        them: beside the importing file, else anywhere under the Effects
        folder; imports of imports too;
      - the files of its "filename:" lines (images for @gfx), beside the
        script or in the Data folder next to the Effects folder;
      - the data files of its file sliders ("slider1:/folder:..."), from
        Data/folder, as long as the bundle stays small.

    Uses juce_core only. Header-only, as the rest of Common's JUCE code.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "JsfxBundle.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace fxmenet::jsfx
{

/** A script found on disk. */
struct Script
{
    juce::File effectsFolder;   ///< the Effects folder it was found in
    juce::String path;          ///< under it, '/' separated: the slot's name
    juce::String description;   ///< its desc: line
};

/** REAPER's Effects folder for this user, whether it exists or not. */
inline juce::File getReaperEffectsFolder()
{
   #if JUCE_LINUX || JUCE_BSD
    const auto base = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".config");
   #else
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                         #if JUCE_MAC
                          .getChildFile ("Application Support")
                         #endif
                          ;
   #endif
    return base.getChildFile ("REAPER").getChildFile ("Effects");
}

/** The Data folder that goes with an Effects folder (REAPER: its sibling). */
inline juce::File getDataFolder (const juce::File& effectsFolder)
{
    return effectsFolder.getSiblingFile ("Data");
}

namespace detail
{
    inline juce::String relativePath (const juce::File& file, const juce::File& folder)
    {
        return file.getRelativePathFrom (folder).replaceCharacter ('\\', '/');
    }

    /** The first lines of a file, enough for its header. */
    inline juce::String readHead (const juce::File& file, int bytes)
    {
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return {};
        juce::MemoryBlock block;
        in.readIntoMemoryBlock (block, bytes);
        return juce::String::fromUTF8 ((const char*) block.getData(), (int) block.getSize());
    }

    /** The desc: line of a script's header, empty when the file is not one. */
    inline juce::String readDescription (const juce::File& file)
    {
        for (const auto& line : juce::StringArray::fromLines (readHead (file, 4096)))
        {
            if (line.startsWith ("desc:"))
                return line.substring (5).trim();
            if (line.startsWithChar ('@'))
                break;   // the code: no header line after it
        }
        return {};
    }

    inline bool couldBeScript (const juce::File& file)
    {
        const auto extension = file.getFileExtension().toLowerCase();
        return (extension.isEmpty() || extension == ".jsfx") && ! file.getFileName().startsWithChar ('.');
    }
}

/** The scripts under an Effects folder, by path. Slow on a large folder:
    not on the message thread. `shouldStop` lets the caller give up. */
inline std::vector<Script> findScripts (const juce::File& effectsFolder,
                                        const std::function<bool()>& shouldStop = {})
{
    std::vector<Script> out;
    if (! effectsFolder.isDirectory())
        return out;
    for (const auto& entry : juce::RangedDirectoryIterator (effectsFolder, true, "*", juce::File::findFiles))
    {
        if (shouldStop && shouldStop())
            break;
        const auto file = entry.getFile();
        if (! detail::couldBeScript (file) || file.getSize() > (4 << 20))
            continue;
        const auto description = detail::readDescription (file);
        if (description.isEmpty())
            continue;
        out.push_back ({ effectsFolder, detail::relativePath (file, effectsFolder), description });
    }
    std::sort (out.begin(), out.end(), [] (const Script& a, const Script& b) { return a.path.compareNatural (b.path) < 0; });
    return out;
}

//==============================================================================
/** Packs a script. False, with the reason, when the script cannot be read
    or imports a file that is not found. What is optional (an image, a data
    folder too large) is left out and named in `notes`. */
class BundleBuilder
{
public:
    explicit BundleBuilder (juce::File effects) : effectsFolder (std::move (effects)), dataFolder (getDataFolder (effectsFolder)) {}

    bool build (const juce::String& scriptPath, Bundle& bundle, juce::String& error, juce::StringArray* notes = nullptr)
    {
        out = &bundle;
        bundle = {};
        bundle.main = scriptPath.toStdString();
        total = 0;
        optional = 0;
        added.clear();
        log = notes;

        const auto script = effectsFolder.getChildFile (scriptPath);
        if (! script.existsAsFile())
        {
            error = "no script " + script.getFullPathName();
            return false;
        }
        if (! addSource (script, error))
            return false;
        if (! isSafePath (bundle.main))
        {
            error = "the script's path cannot be sent: " + scriptPath;
            return false;
        }
        return true;
    }

private:
    static constexpr std::int64_t maxOptionalBytes = 8 << 20;   ///< images and data files, together

    void note (const juce::String& text)
    {
        if (log != nullptr)
            log->add (text);
    }

    /** Adds a file once. `data`: under the Data folder. */
    bool addFile (const juce::File& file, bool data, juce::String& error)
    {
        const auto path = detail::relativePath (file, data ? dataFolder : effectsFolder).toStdString();
        const auto key = (data ? "D:" : "E:") + path;
        if (std::find (added.begin(), added.end(), key) != added.end())
            return true;
        if (! isSafePath (path))
        {
            error = file.getFullPathName() + " is outside " + (data ? dataFolder : effectsFolder).getFullPathName();
            return false;
        }
        juce::MemoryBlock block;
        if (! file.loadFileAsData (block))
        {
            error = "cannot read " + file.getFullPathName();
            return false;
        }
        total += (std::int64_t) block.getSize();
        if (out->files.size() >= maxFiles || (std::size_t) total > maxBundleBytes)
        {
            error = "the script and its files are too large to send";
            return false;
        }
        File f;
        f.path = path;
        f.data = data;
        f.bytes.assign ((const std::uint8_t*) block.getData(), (const std::uint8_t*) block.getData() + block.getSize());
        out->files.push_back (std::move (f));
        added.push_back (key);
        return true;
    }

    /** A script or an import: the file, then what its header names. */
    bool addSource (const juce::File& file, juce::String& error)
    {
        const auto key = "E:" + detail::relativePath (file, effectsFolder).toStdString();
        if (std::find (added.begin(), added.end(), key) != added.end())
            return true;   // already in (imports can cross)
        if (! addFile (file, false, error))
            return false;

        for (auto line : juce::StringArray::fromLines (file.loadFileAsString()))
        {
            if (line.startsWithChar ('@'))
                break;   // the code starts: the header is over

            if (line.startsWith ("import") && (line.length() == 6 || line[6] == ' ' || line[6] == '\t'))
            {
                const auto name = line.substring (6).trim();
                if (name.isEmpty())
                    continue;
                const auto imported = findImport (name, file);
                if (! imported.existsAsFile())
                {
                    error = "import: cannot find '" + name + "' (from " + file.getFileName() + ")";
                    return false;
                }
                if (! addSource (imported, error))
                    return false;
            }
            else if (line.startsWith ("filename:"))
            {
                // filename:N,path : beside the script, else in the Data folder
                const auto name = line.fromFirstOccurrenceOf (",", false, false).trim();
                if (name.isEmpty())
                    continue;
                const auto beside = file.getParentDirectory().getChildFile (name);
                const auto inData = dataFolder.getChildFile (name);
                if (beside.existsAsFile())
                    addOptional (beside, false);
                else if (inData.existsAsFile())
                    addOptional (inData, true);
                else
                    note ("file not found: " + name);
            }
            else if (line.startsWith ("slider") && line.contains (":/"))
            {
                // sliderN:/folder:default:label : the files of Data/folder
                const auto folderName = line.fromFirstOccurrenceOf (":/", false, false).upToFirstOccurrenceOf (":", false, false).trim();
                const auto folder = dataFolder.getChildFile (folderName);
                if (folderName.isEmpty() || ! folder.isDirectory())
                    continue;
                for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
                    if (! addOptional (entry.getFile(), true))
                    {
                        note ("the data folder " + folderName + " is too large to send whole");
                        break;
                    }
            }
        }
        return true;
    }

    /** Images and data: left out, not an error, past the optional budget. */
    bool addOptional (const juce::File& file, bool data)
    {
        if (optional + file.getSize() > maxOptionalBytes)
        {
            note ("left out (too large): " + file.getFileName());
            return false;
        }
        juce::String problem;
        if (! addFile (file, data, problem))
        {
            note (problem);
            return false;
        }
        optional += file.getSize();
        return true;
    }

    /** As the JSFX loader: beside the importing file (dropping the leading
        folders of the name one by one), then the path under the Effects
        folder, then a file of that name anywhere under it. */
    juce::File findImport (const juce::String& name, const juce::File& from) const
    {
        const auto normal = name.replaceCharacter ('\\', '/');
        for (auto rest = normal; rest.isNotEmpty();)
        {
            const auto candidate = from.getParentDirectory().getChildFile (rest);
            if (candidate.existsAsFile())
                return candidate;
            if (! rest.containsChar ('/'))
                break;
            rest = rest.fromFirstOccurrenceOf ("/", false, false);
        }
        const auto direct = effectsFolder.getChildFile (normal);
        if (direct.existsAsFile())
            return direct;
        const auto leaf = normal.fromLastOccurrenceOf ("/", false, false);
        for (const auto& entry : juce::RangedDirectoryIterator (effectsFolder, true, leaf, juce::File::findFiles))
            return entry.getFile();
        return {};
    }

    juce::File effectsFolder, dataFolder;
    Bundle* out = nullptr;
    std::vector<std::string> added;
    std::int64_t total = 0, optional = 0;
    juce::StringArray* log = nullptr;
};

/** A new JSFX slot's state: the bundle, the script's default sliders. */
inline std::vector<std::uint8_t> makeSlotState (Bundle bundle)
{
    SlotState state;
    state.bundle = std::move (bundle);
    return encode (state);
}

} // namespace fxmenet::jsfx
