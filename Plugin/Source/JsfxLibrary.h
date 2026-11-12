/*
  ------------------------------------------------------------------------------
    JsfxLibrary.h

    The JSFX scripts on this machine that can be added to the chain
    (doc/jsfx-plan.md): those of REAPER's Effects folder and of the folders
    added in the Settings tab. Scripts live on the client; the server
    receives one when it is added (jsfx::BundleBuilder).

    One library for every FxmeNet instance of the process
    (SharedResourcePointer); the scan runs on its own thread.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "AppSettings.h"

#include <FxmeNet/jsfx/JsfxScripts.h>

#include <atomic>
#include <mutex>

namespace fxmenet::plugin
{

class JsfxLibrary : private juce::Thread
{
public:
    JsfxLibrary() : juce::Thread ("FxmeNet JSFX scan")
    {
        rescan();
    }

    ~JsfxLibrary() override
    {
        stopThread (4000);
    }

    /** Scans the folders again. While a scan runs, another follows it. Any thread. */
    void rescan()
    {
        again.store (true);
        startThread();
        notify();
    }

    bool isScanning() const noexcept    { return scanning.load(); }

    /** Changes with every finished scan. */
    int getRevision() const noexcept    { return revision.load(); }

    std::vector<jsfx::Script> getScripts() const
    {
        const std::lock_guard<std::mutex> l (lock);
        return scripts;
    }

    int getNumScripts() const
    {
        const std::lock_guard<std::mutex> l (lock);
        return (int) scripts.size();
    }

    /** The Effects folders searched: REAPER's, then the added ones. */
    static juce::StringArray getFolders()
    {
        juce::StringArray folders;
        folders.add (jsfx::getReaperEffectsFolder().getFullPathName());
        folders.addArray (getJsfxFolders());
        folders.removeDuplicates (false);
        return folders;
    }

    /** The added folders (AppSettings), and a scan. */
    void setExtraFolders (const juce::StringArray& folders)
    {
        setJsfxFolders (folders);
        rescan();
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            if (! again.exchange (false))
            {
                wait (-1);
                continue;
            }
            scanning.store (true);
            std::vector<jsfx::Script> found;
            for (auto& folder : getFolders())
            {
                auto part = jsfx::findScripts (juce::File (folder), [this] { return threadShouldExit(); });
                found.insert (found.end(), part.begin(), part.end());
            }
            if (threadShouldExit())
                return;
            {
                const std::lock_guard<std::mutex> l (lock);
                scripts = std::move (found);
            }
            scanning.store (false);
            revision.fetch_add (1);
        }
    }

    mutable std::mutex lock;
    std::vector<jsfx::Script> scripts;
    std::atomic<bool> again { false }, scanning { false };
    std::atomic<int> revision { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JsfxLibrary)
};

} // namespace fxmenet::plugin
