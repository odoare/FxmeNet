/*
  ------------------------------------------------------------------------------
    AppSettings.h

    Machine-wide settings of FxmeNet (every instance, every session):
    FX-Mechanics/FxmeNet.settings in the user's configuration folder.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxmenet::plugin
{

inline juce::ApplicationProperties& appProperties()
{
    static juce::ApplicationProperties props;
    static const bool inited = []
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "FxmeNet";
        o.filenameSuffix      = "settings";
       #if JUCE_LINUX || JUCE_BSD
        o.folderName          = ".config/FX-Mechanics";   // JUCE puts it under ~ itself on Linux
       #else
        o.folderName          = "FX-Mechanics";           // already under the application data folder
       #endif
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
        return true;
    }();
    juce::ignoreUnused (inited);
    return props;
}

/** Whether the hover help is shown (on by default). */
inline bool getUiTooltips()
{
    auto* s = appProperties().getUserSettings();
    return s == nullptr || s->getBoolValue ("uiTooltips", true);
}

inline void setUiTooltips (bool on)
{
    if (auto* s = appProperties().getUserSettings())
    {
        s->setValue ("uiTooltips", on);
        s->saveIfNeeded();
    }
}

/** The last server connected to ("host:port"), offered to new instances. */
inline juce::String getLastServer()
{
    auto* s = appProperties().getUserSettings();
    return s != nullptr ? s->getValue ("lastServer") : juce::String();
}

inline void setLastServer (const juce::String& hostAndPort)
{
    if (auto* s = appProperties().getUserSettings())
    {
        s->setValue ("lastServer", hostAndPort);
        s->saveIfNeeded();
    }
}

/** Whether a host reset drops the audio in flight (on by default). */
inline bool getFlushOnReset()
{
    auto* s = appProperties().getUserSettings();
    return s == nullptr || s->getBoolValue ("flushOnReset", true);
}

inline void setFlushOnReset (bool on)
{
    if (auto* s = appProperties().getUserSettings())
    {
        s->setValue ("flushOnReset", on);
        s->saveIfNeeded();
    }
}

/** Whether transport events seen by the audio thread are logged (off by
    default): FX-Mechanics/fxmenet-client-events.log. */
inline bool getLogEvents()
{
    auto* s = appProperties().getUserSettings();
    return s != nullptr && s->getBoolValue ("logEvents", false);
}

inline void setLogEvents (bool on)
{
    if (auto* s = appProperties().getUserSettings())
    {
        s->setValue ("logEvents", on);
        s->saveIfNeeded();
    }
}

inline juce::File getEventsLogFile()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("FX-Mechanics").getChildFile ("fxmenet-client-events.log");
}

/** Folders scanned for local plugins besides the VST3 default ones. */
inline juce::StringArray getLocalPluginFolders()
{
    auto* s = appProperties().getUserSettings();
    if (s == nullptr)
        return {};
    juce::StringArray folders;
    folders.addLines (s->getValue ("localPluginFolders"));
    folders.removeEmptyStrings();
    return folders;
}

inline void setLocalPluginFolders (const juce::StringArray& folders)
{
    if (auto* s = appProperties().getUserSettings())
    {
        s->setValue ("localPluginFolders", folders.joinIntoString ("\n"));
        s->saveIfNeeded();
    }
}

} // namespace fxmenet::plugin
