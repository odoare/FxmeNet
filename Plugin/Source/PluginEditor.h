/*
  ------------------------------------------------------------------------------
    PluginEditor.h

    FxmeNet's window: the FX-Mechanics top bar, the server (found on the
    network or typed) with its status; on the left the server's plugins and
    the chain with its edit buttons; on the right four tabs (Panels.h): the
    selected slot's parameters, the automation mapping, the routing, the
    settings (stream, transport, local plugins); one line of statistics at the bottom.

    Everything is read from the processor and its engine by a 10 Hz timer;
    nothing here touches the network itself, apart from the discovery
    broadcast, run on a background thread.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <FxmeNet/control/Discovery.h>

#include "PluginProcessor.h"
#include "AppSettings.h"
#include "Panels.h"
#include "Theme.h"

class FxmeNetAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit FxmeNetAudioProcessorEditor (FxmeNetAudioProcessor&);
    ~FxmeNetAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    /** A list drawn from a vector of rows, refreshed by the timer. */
    struct RowList : public juce::ListBoxModel
    {
        struct Row
        {
            juce::String text, detail;
            juce::Colour colour;
            juce::String tip;   ///< hover text of the row (optional)
        };

        std::vector<Row> rows;
        std::function<void (int)> onDoubleClick;

        int getNumRows() override   { return (int) rows.size(); }
        juce::String getTooltipForRow (int row) override
        {
            return juce::isPositiveAndBelow (row, (int) rows.size()) ? rows[(std::size_t) row].tip : juce::String();
        }
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            if (onDoubleClick)
                onDoubleClick (row);
        }
    };

    void timerCallback() override;
    void refreshCatalog();
    void refreshChain();

    void connectClicked();
    void findServers();
    void showServers (const std::vector<fxmenet::discovery::Announce>&);
    void addSelected();

    /** The catalog entry shown at a row of the list (filtered and sorted), or nullptr. */
    const fxmenet::control::PluginEntry* catalogEntryAt (int row) const;
    std::uint32_t selectedSlot() const;

    static juce::Image assetImage (const char* resourceName);

    /** The window's size for a plugin editor of w x h in the Parameters
        tab (0, 0: none, the default size). */
    void fitToPluginEditor (int w, int h);

    /** "T0", "T1", "T2" or "?" (local scan not done), and its colour. */
    static juce::String tierTag (const fxmenet::plugin::LocalMatch&);
    static juce::Colour tierColour (const fxmenet::plugin::LocalMatch&);
    static void styleButton (juce::TextButton&);

    FxmeNetAudioProcessor& processor;
    fxme::FxmeLookAndFeel laf;

    fxme::TopBar topBar { fxmenet::plugin::theme::displayName, "Plugins processed on a server on the network",
                          JucePlugin_VersionString, assetImage ("logo686_png") };
    fxme::AccentToggle tooltipsButton;
    fxme::InfoButton infoButton;

    juce::ComboBox serverBox;
    juce::TextButton findButton { "Find" }, connectButton { "Connect" };
    std::vector<fxmenet::discovery::Announce> found;

    RowList catalogRows, chainRows;
    juce::ListBox catalogList { "Server plugins", &catalogRows }, chainList { "Chain", &chainRows };
    juce::TextEditor filterBox;
    juce::TextButton addButton { "Add" };
    juce::TextButton upButton { "Up" }, downButton { "Down" }, bypassButton { "Bypass" }, removeButton { "Remove" };

    fxmenet::plugin::ParameterPanel parameterPanel { processor };
    fxmenet::plugin::MappingPanel mappingPanel { processor };
    fxmenet::plugin::RoutingPanel routingPanel { processor };
    fxmenet::plugin::SettingsPanel settingsPanel { processor };
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    /** The server's plugins, then this machine's JSFX scripts (as entries
        of the vendor "JSFX", named by their path): one list. */
    std::vector<fxmenet::control::PluginEntry> catalog;
    std::vector<fxmenet::jsfx::Script> scripts;   ///< those entries' scripts, in order
    int numServerEntries = 0;                      ///< catalog[numServerEntries + i] is scripts[i]
    std::vector<int> shown;   ///< the catalog rows listed: filtered, T2 then T1 then T0, by name
    std::vector<fxmenet::client::SlotModel> slots;
    juce::String catalogSignature, chainSignature, statusLine, statsLine, catalogCount, shownFilter;
    juce::Colour statusColour;
    juce::Rectangle<int> statusArea, statsArea, catalogTitle, chainTitle;

    struct ToggleableTooltipWindow : public juce::TooltipWindow
    {
        using juce::TooltipWindow::TooltipWindow;
        juce::String getTipFor (juce::Component& c) override
        {
            return fxmenet::plugin::getUiTooltips() ? juce::TooltipWindow::getTipFor (c) : juce::String();
        }
    };
    ToggleableTooltipWindow tooltipWindow { this, 700 };

    // The server field and the filter are TextEditors: one fixer, after the children.
    fxme::TextEntryFocusFixer textEntryFixer { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeNetAudioProcessorEditor)
};
