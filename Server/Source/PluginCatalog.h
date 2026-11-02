/*
  ------------------------------------------------------------------------------
    PluginCatalog.h

    The VST3 plugins the server can load: scanned once at startup on the
    message thread, listed to clients (PLUGIN_LIST), and looked up by the
    identity protocol.md section 4.3 defines (uid, name, vendor).

    Version 1 limits, noted in doc/todo.md: the scan runs in-process (a
    plugin crashing while scanned takes the server down), and the catalog
    leaves sha256 empty and fxmenetAbi at 0 (unknown) until a slot is loaded,
    when SLOT_READY reports the real ABI.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/control/ControlMessages.h>

#include <vector>

namespace fxmenet::server
{

class PluginCatalog
{
public:
    PluginCatalog();

    /** Scans the default VST3 locations (unless told not to) and `extraPaths`.
        Message thread. Returns the number of plugins found. */
    int scan (const juce::StringArray& extraPaths, bool useDefaultPaths);

    std::vector<control::PluginEntry> getEntries() const;

    /** The description of a plugin, or nullptr. Name and vendor must match
        when given (a guard against a uid collision). */
    const juce::PluginDescription* find (std::uint32_t uid, const juce::String& name,
                                         const juce::String& vendor) const;

    /** Creates an instance. Message thread. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error);

private:
    juce::AudioPluginFormatManager formats;
    juce::OwnedArray<juce::PluginDescription> plugins;

    JUCE_DECLARE_NON_COPYABLE (PluginCatalog)
};

} // namespace fxmenet::server
