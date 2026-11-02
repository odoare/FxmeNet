/*
  ------------------------------------------------------------------------------
    AppSettings.h

    Machine-wide settings of GainPan (every instance, every session):
    FX-Mechanics/FxmeNetGainPan.settings in the user's configuration folder.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace gainpan
{

inline juce::ApplicationProperties& appProperties()
{
    static juce::ApplicationProperties props;
    static const bool inited = []
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "FxmeNetGainPan";
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

} // namespace gainpan
