/*
  ------------------------------------------------------------------------------
    Panels.h

    The three tabs of FxmeNet's editor (7d):

      - ParameterPanel: the selected slot's parameters, one row each: name,
        a slider (normalised; stepped ones snap), the value text, the pool
        entry it is mapped to, Learn (the next pool parameter moved in the
        DAW is mapped to it) and Map / Unmap (the first free pool entry);
      - MappingPanel: every mapped pool entry, with Unmap, and the state of
        a learn in progress;
      - SettingsPanel: block size N and budget D with the latency they give,
        one toggle per input and output channel (the masks), and the local
        plugin scan (its extra folders, rescans).

    Each is refreshed by the editor's timer from the processor; none keeps a
    copy of anything the processor owns beyond what it shows.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "RoutingView.h"

namespace fxmenet::plugin
{

//==============================================================================
/** The selected slot: its own editor (through its shadow, tier 1 and 2), or
    the generic rows; a button switches between them. */
class ParameterPanel : public juce::Component,
                       private juce::ComponentListener
{
public:
    explicit ParameterPanel (FxmeNetAudioProcessor&);
    ~ParameterPanel() override;   // Row is complete in Panels.cpp only

    /** Shows `slot` (null: nothing selected). Message thread, from the timer. */
    void refresh (const client::SlotModel* slot);

    /** Deletes the plugin editor if it shows `slotId` (0: whichever): its
        shadow is about to go. */
    void dropPluginEditor (std::uint32_t slotId = 0);

    /** The size the plugin editor needs below the panel's top row, or 0, 0
        when none is shown: the window adapts to it. */
    std::function<void (int width, int height)> onWantsSize;

    /** The height of the row above the plugin editor or the generic rows. */
    static constexpr int topRowHeight = 30;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    void showGeneric (const client::SlotModel* slot);
    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override;

    /** The menu of the last touched control: map, unmap, learn. */
    void lastTouchedMenu();

    FxmeNetAudioProcessor& processor;
    juce::TextButton viewButton, lastTouchedButton;
    juce::Rectangle<int> infoArea;
    juce::String info;

    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;

    juce::Viewport editorViewport;
    std::unique_ptr<juce::AudioProcessorEditor> pluginEditor;
    std::uint32_t editorSlot = 0;
    bool preferPluginEditor = true;

    std::uint32_t shownSlot = 0;
    juce::String shownSignature, message;
};

//==============================================================================
class MappingPanel : public juce::Component,
                     private juce::ListBoxModel
{
public:
    explicit MappingPanel (FxmeNetAudioProcessor&);

    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override   { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;

    FxmeNetAudioProcessor& processor;
    std::vector<int> entries;   ///< mapped pool indices
    juce::ListBox list { "Mapping", this };
    juce::TextButton unmapButton { "Unmap" }, unmapAllButton { "Unmap all" }, cancelLearnButton { "Cancel learn" };
    juce::String learnText;
    juce::Rectangle<int> learnArea;
};

//==============================================================================
class SettingsPanel : public juce::Component
{
public:
    explicit SettingsPanel (FxmeNetAudioProcessor&);

    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void apply();

    /** The menu of the extra plugin folders: add one, remove one. */
    void foldersMenu();

    FxmeNetAudioProcessor& processor;
    juce::ComboBox blockBox, budgetBox;
    RoutingView routingView;
    juce::TextButton resetRoutingButton { "Reset routing" };
    juce::TextButton rescanButton { "Rescan" }, fullRescanButton { "Full rescan" }, foldersButton { "Folders..." };
    juce::TextButton flushButton { "Silence in-flight audio at reset" }, logButton { "Log transport events" };
    juce::TextButton aroundButton { "Channels go around plugins that lack them" },
                     extraOutputsButton { "Connect extra outputs" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String latencyText, localText, foldersText;
    juce::Rectangle<int> blockLabel, budgetLabel, latencyArea, routingLabel, routingHint, localArea, foldersArea, diagnosticsLabel,
                         defaultsLabel;
};

} // namespace fxmenet::plugin
