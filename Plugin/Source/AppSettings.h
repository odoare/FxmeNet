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

} // namespace fxmenet::plugin
