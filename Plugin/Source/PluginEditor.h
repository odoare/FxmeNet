/*
  ------------------------------------------------------------------------------
    PluginEditor.h

    FxmeNet's window, Phase 3 minimal version (7c): the FX-Mechanics top bar,
    the server (found on the network or typed) with its status, the server's
    plugins, the chain with its edit buttons, and one line of statistics.
    The full editor (generic parameter view, learn mapping, channel grid,
    stream settings) is step 7d.

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
        };

        std::vector<Row> rows;
        std::function<void (int)> onDoubleClick;

        int getNumRows() override   { return (int) rows.size(); }
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
    std::uint32_t selectedSlot() const;

    static juce::Image assetImage (const char* resourceName);
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
    juce::TextButton addButton { "Add" };
    juce::TextButton upButton { "Up" }, downButton { "Down" }, bypassButton { "Bypass" }, removeButton { "Remove" };

    std::vector<fxmenet::control::PluginEntry> catalog;
    std::vector<fxmenet::client::SlotModel> slots;
    juce::String catalogSignature, chainSignature, statusLine, statsLine;
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

    // The server field is a TextEditor: one fixer, after the children.
    fxme::TextEntryFocusFixer textEntryFixer { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeNetAudioProcessorEditor)
};
