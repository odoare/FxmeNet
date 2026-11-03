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
        and one toggle per input and output channel (the masks).

    Each is refreshed by the editor's timer from the processor; none keeps a
    copy of anything the processor owns beyond what it shows.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

namespace fxmenet::plugin
{

//==============================================================================
class ParameterPanel : public juce::Component
{
public:
    explicit ParameterPanel (FxmeNetAudioProcessor&);
    ~ParameterPanel() override;   // Row is complete in Panels.cpp only

    /** Shows `slot` (null: nothing selected). Message thread, from the timer. */
    void refresh (const client::SlotModel* slot);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    FxmeNetAudioProcessor& processor;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;
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
    void rebuildChannels (int inputs, int outputs);
    void apply();

    FxmeNetAudioProcessor& processor;
    juce::ComboBox blockBox, budgetBox;
    juce::OwnedArray<juce::TextButton> inputButtons, outputButtons;
    juce::TextButton allInputs { "All" }, allOutputs { "All" };
    int shownInputs = -1, shownOutputs = -1;
    juce::String latencyText;
    juce::Rectangle<int> blockLabel, budgetLabel, latencyArea, inputsLabel, outputsLabel;
};

} // namespace fxmenet::plugin
