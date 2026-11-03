/*
  ------------------------------------------------------------------------------
    Panels.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Panels.h"
#include "Theme.h"
#include "Tooltips.h"

namespace fxmenet::plugin
{

namespace
{
    void styleButton (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, theme::panel);
        b.setColour (juce::TextButton::buttonOnColourId, theme::accent.darker (0.6f));
        b.setColour (juce::TextButton::textColourOffId, theme::text);
        b.setColour (juce::TextButton::textColourOnId, theme::text);
    }

    juce::String poolLabel (int poolIndex)
    {
        return poolIndex >= 0 ? "P" + juce::String (poolIndex + 1).paddedLeft ('0', 3) : juce::String ("-");
    }

    /** A parameter's value as text: its step's name, else the normalised value. */
    juce::String valueText (const control::ParamInfo& p, float v)
    {
        if (p.stepTexts.size() > 1)
            return p.stepTexts[juce::jlimit (0, p.stepTexts.size() - 1, (int) std::lround (v * (float) (p.stepTexts.size() - 1)))];
        return juce::String (v, 3);
    }
}

//==============================================================================
// ParameterPanel

class ParameterPanel::Row : public juce::Component
{
public:
    Row (FxmeNetAudioProcessor& p, std::uint32_t slotId, const control::ParamInfo& paramInfo)
        : processor (p), target { slotId, (std::uint32_t) paramInfo.index }, info (paramInfo)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        const double interval = info.numSteps > 1 ? 1.0 / (info.numSteps - 1) : 0.0;
        slider.setRange (0.0, 1.0, interval);
        slider.setDoubleClickReturnValue (true, info.defaultValue);
        slider.setColour (juce::Slider::trackColourId, theme::accent);
        slider.setColour (juce::Slider::thumbColourId, theme::accent.brighter (0.3f));
        slider.setColour (juce::Slider::backgroundColourId, theme::panelLine);
        slider.setTooltip (tips::paramSlider);
        slider.onDragStart = [this]
        {
            processor.cancelLearn();   // a move here must not be taken for the DAW's
            processor.beginSlotParameterGesture (target);
        };
        slider.onDragEnd = [this] { processor.endSlotParameterGesture (target); };
        slider.onValueChange = [this]
        {
            processor.setSlotParameter (target, (float) slider.getValue());
            value.setText (valueText (info, (float) slider.getValue()), juce::dontSendNotification);
        };
        addAndMakeVisible (slider);

        for (auto* l : { &name, &value, &pool })
        {
            l->setColour (juce::Label::textColourId, theme::text);
            l->setFont (juce::Font (juce::FontOptions (12.5f)));
            addAndMakeVisible (*l);
        }
        name.setText (info.name + (info.label.isNotEmpty() ? " (" + info.label + ")" : juce::String()), juce::dontSendNotification);
        name.setTooltip (info.name);
        value.setJustificationType (juce::Justification::centredRight);
        pool.setColour (juce::Label::textColourId, theme::dimText);
        pool.setJustificationType (juce::Justification::centred);
        pool.setTooltip (tips::paramPool);

        learn.setClickingTogglesState (false);
        learn.setTooltip (tips::learn);
        learn.onClick = [this]
        {
            if (processor.getLearnTarget() == target)
                processor.cancelLearn();
            else
                processor.startLearn (target);
        };
        map.setTooltip (tips::map);
        map.onClick = [this]
        {
            const int i = processor.findPoolFor (target);
            if (i >= 0)
                processor.unmapPool (i);
            else
                processor.mapToFreePool (target);
        };
        for (auto* b : { &learn, &map })
        {
            styleButton (*b);
            addAndMakeVisible (*b);
        }

        if (! info.automatable)
        {
            map.setEnabled (false);
            learn.setEnabled (false);
        }
    }

    /** The current value and mapping, unless the user is dragging. */
    void update (float v)
    {
        if (! slider.isMouseButtonDown())
        {
            slider.setValue (v, juce::dontSendNotification);
            value.setText (valueText (info, v), juce::dontSendNotification);
        }
        const int poolIndex = processor.findPoolFor (target);
        pool.setText (poolLabel (poolIndex), juce::dontSendNotification);
        map.setButtonText (poolIndex >= 0 ? "Unmap" : "Map");
        learn.setToggleState (processor.getLearnTarget() == target, juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 2);
        name.setBounds (r.removeFromLeft (170));
        map.setBounds (r.removeFromRight (56));
        r.removeFromRight (4);
        learn.setBounds (r.removeFromRight (56));
        r.removeFromRight (4);
        pool.setBounds (r.removeFromRight (44));
        value.setBounds (r.removeFromRight (90));
        r.removeFromRight (6);
        slider.setBounds (r);
    }

    std::uint32_t getIndex() const noexcept   { return target.paramIndex; }

private:
    FxmeNetAudioProcessor& processor;
    const Mapping target;
    const control::ParamInfo info;
    juce::Label name, value, pool;
    juce::Slider slider;
    juce::TextButton learn { "Learn" }, map { "Map" };
};

ParameterPanel::ParameterPanel (FxmeNetAudioProcessor& p) : processor (p)
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

ParameterPanel::~ParameterPanel() = default;

void ParameterPanel::refresh (const client::SlotModel* slot)
{
    const std::uint32_t id = slot != nullptr ? slot->slotId : 0;
    juce::String signature;
    if (slot != nullptr)
        signature << (int) id << (slot->loaded ? "L" : "-") << (int) slot->info.params.size();

    if (id != shownSlot || signature != shownSignature)
    {
        shownSlot = id;
        shownSignature = signature;
        rows.clear();
        message = slot == nullptr ? "Select a plugin in the chain to see its parameters."
                : ! slot->loaded  ? slot->name + " is not loaded on the server."
                                  : juce::String();

        if (slot != nullptr && slot->loaded)
            for (auto& info : slot->info.params)
            {
                rows.push_back (std::make_unique<Row> (processor, id, info));
                content.addAndMakeVisible (*rows.back());
            }
        resized();
        repaint();
    }

    if (slot != nullptr && slot->loaded)
        for (std::size_t i = 0; i < rows.size() && i < slot->info.params.size(); ++i)
            rows[i]->update (client::ClientEngine::currentValue (*slot, slot->info.params[i]));
}

void ParameterPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    if (message.isNotEmpty())
    {
        g.setColour (theme::dimText);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (message, getLocalBounds().reduced (12), juce::Justification::centred, true);
    }
}

void ParameterPanel::resized()
{
    viewport.setBounds (getLocalBounds());
    const int w = viewport.getWidth() - viewport.getScrollBarThickness();
    content.setSize (w, (int) rows.size() * 26);
    int y = 0;
    for (auto& r : rows)
    {
        r->setBounds (0, y, w, 26);
        y += 26;
    }
}

//==============================================================================
// MappingPanel

MappingPanel::MappingPanel (FxmeNetAudioProcessor& p) : processor (p)
{
    list.setRowHeight (theme::rowHeight);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    list.setColour (juce::ListBox::outlineColourId, theme::panelLine);
    list.setOutlineThickness (1);
    list.setMultipleSelectionEnabled (true);
    list.setTooltip (tips::mappingList);
    addAndMakeVisible (list);

    unmapButton.setTooltip (tips::unmap);
    unmapButton.onClick = [this]
    {
        const auto selected = list.getSelectedRows();
        std::vector<int> toUnmap;
        for (int i = 0; i < selected.size(); ++i)
            if (juce::isPositiveAndBelow (selected[i], (int) entries.size()))
                toUnmap.push_back (entries[(std::size_t) selected[i]]);
        for (int i : toUnmap)
            processor.unmapPool (i);
        list.deselectAllRows();
        refresh();
    };
    unmapAllButton.setTooltip (tips::unmapAll);
    unmapAllButton.onClick = [this]
    {
        for (int i : entries)
            processor.unmapPool (i);
        refresh();
    };
    cancelLearnButton.onClick = [this] { processor.cancelLearn(); };

    for (auto* b : { &unmapButton, &unmapAllButton, &cancelLearnButton })
    {
        styleButton (*b);
        addAndMakeVisible (*b);
    }
}

void MappingPanel::refresh()
{
    std::vector<int> now;
    for (int i = 0; i < poolSize; ++i)
        if (processor.getMapping (i).isMapped())
            now.push_back (i);
    if (now != entries)
    {
        entries = std::move (now);
        list.updateContent();
    }
    list.repaint();   // values move

    const auto learning = processor.getLearnTarget();
    juce::String text;
    if (learning.isMapped())
    {
        text = "Learning: move a parameter in the DAW to map it to ";
        for (auto& s : processor.getEngine().getSlotInfos())
            if (s.slotId == learning.slotId)
                for (auto& p : s.info.params)
                    if ((std::uint32_t) p.index == learning.paramIndex)
                        text << s.name << ": " << p.name;
    }
    cancelLearnButton.setVisible (learning.isMapped());
    if (text != learnText)
    {
        learnText = text;
        repaint (learnArea);
    }
}

void MappingPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) entries.size()))
        return;
    const int i = entries[(std::size_t) row];
    auto& p = processor.getPoolParameter (i);
    if (selected)
        g.fillAll (theme::selection);

    auto r = juce::Rectangle<int> (0, 0, width, height).reduced (8, 0);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.setColour (theme::dimText);
    g.drawText (poolLabel (i), r.removeFromLeft (50), juce::Justification::centredLeft);
    g.setColour (theme::text);
    const auto valueText = p.getCurrentValueAsText();
    g.drawText (valueText, r.removeFromRight (90), juce::Justification::centredRight);
    g.drawText (p.getName (128), r, juce::Justification::centredLeft, true);
}

void MappingPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.setColour (learnText.isNotEmpty() ? theme::warning : theme::dimText);
    g.drawText (learnText.isNotEmpty() ? learnText
                                       : juce::String (entries.size()) + " of " + juce::String (poolSize)
                                             + " automation parameters mapped",
                learnArea, juce::Justification::centredLeft, true);
}

void MappingPanel::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto top = r.removeFromTop (24);
    cancelLearnButton.setBounds (top.removeFromRight (100));
    learnArea = top;
    r.removeFromTop (6);
    auto buttons = r.removeFromBottom (26);
    unmapAllButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (6);
    unmapButton.setBounds (buttons.removeFromRight (80));
    r.removeFromBottom (6);
    list.setBounds (r);
}

//==============================================================================
// SettingsPanel

SettingsPanel::SettingsPanel (FxmeNetAudioProcessor& p) : processor (p)
{
    int id = 1;
    for (int n : { 32, 64, 128, 256, 512, 1024 })
        blockBox.addItem (juce::String (n) + " samples", id++);
    for (int d = 1; d <= 8; ++d)
        budgetBox.addItem (juce::String (d) + (d == 1 ? " block" : " blocks"), d);
    blockBox.setTooltip (tips::blockSize);
    budgetBox.setTooltip (tips::budget);
    blockBox.onChange = [this] { apply(); };
    budgetBox.onChange = [this] { apply(); };
    addAndMakeVisible (blockBox);
    addAndMakeVisible (budgetBox);

    allInputs.setTooltip (tips::allChannels);
    allOutputs.setTooltip (tips::allChannels);
    allInputs.onClick = [this]
    {
        for (auto* b : inputButtons)
            b->setToggleState (true, juce::dontSendNotification);
        apply();
    };
    allOutputs.onClick = [this]
    {
        for (auto* b : outputButtons)
            b->setToggleState (true, juce::dontSendNotification);
        apply();
    };
    rescanButton.setTooltip (tips::rescan);
    fullRescanButton.setTooltip (tips::fullRescan);
    rescanButton.onClick = [this] { processor.getLocalCatalog().rescan (false); };
    fullRescanButton.onClick = [this] { processor.getLocalCatalog().rescan (true); };

    for (auto* b : { &allInputs, &allOutputs, &rescanButton, &fullRescanButton })
    {
        styleButton (*b);
        addAndMakeVisible (*b);
    }
}

void SettingsPanel::rebuildChannels (int inputs, int outputs)
{
    shownInputs = inputs;
    shownOutputs = outputs;
    inputButtons.clear();
    outputButtons.clear();

    auto make = [this] (juce::OwnedArray<juce::TextButton>& into, int count, const char* tip)
    {
        for (int c = 0; c < count; ++c)
        {
            auto* b = into.add (new juce::TextButton (juce::String (c + 1)));
            styleButton (*b);
            b->setClickingTogglesState (true);
            b->setTooltip (tip);
            b->onClick = [this] { apply(); };
            addAndMakeVisible (b);
        }
    };
    make (inputButtons, inputs, tips::inputChannel);
    make (outputButtons, outputs, tips::outputChannel);
    resized();
}

void SettingsPanel::refresh()
{
    const int ins = processor.getPreparedInputs(), outs = processor.getPreparedOutputs();
    if (ins != shownInputs || outs != shownOutputs)
        rebuildChannels (ins, outs);

    const auto s = processor.getStreamSettings();
    int blockIndex = 0;
    for (int i = 0; i < blockBox.getNumItems(); ++i)
        if (blockBox.getItemText (i).getIntValue() == s.blockSize)
            blockIndex = i;
    blockBox.setSelectedItemIndex (blockIndex, juce::dontSendNotification);
    budgetBox.setSelectedId (s.budgetBlocks, juce::dontSendNotification);

    for (int c = 0; c < inputButtons.size(); ++c)
        inputButtons[c]->setToggleState (((s.inputMask >> c) & 1) != 0, juce::dontSendNotification);
    for (int c = 0; c < outputButtons.size(); ++c)
        outputButtons[c]->setToggleState (((s.outputMask >> c) & 1) != 0, juce::dontSendNotification);

    auto& engine = processor.getEngine();
    const double rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    const int stream = engine.getStreamLatency(), total = engine.getLatency();
    const auto text = "Latency " + juce::String (total) + " samples (" + juce::String (1000.0 * total / rate, 1)
                    + " ms): " + juce::String (stream) + " for the network and re-blocking (host blocks of "
                    + juce::String (processor.getPreparedBlockSize()) + "), "
                    + juce::String (total - stream) + " for the chain itself";
    if (text != latencyText)
    {
        latencyText = text;
        repaint (latencyArea);
    }

    auto& local = processor.getLocalCatalog();
    const auto lt = local.getProgress();
    rescanButton.setEnabled (! local.isScanning());
    fullRescanButton.setEnabled (! local.isScanning());
    if (lt != localText)
    {
        localText = lt;
        repaint (localArea);
    }
}

void SettingsPanel::apply()
{
    auto s = processor.getStreamSettings();
    s.blockSize = blockBox.getText().getIntValue() > 0 ? blockBox.getText().getIntValue() : s.blockSize;
    s.budgetBlocks = budgetBox.getSelectedId() > 0 ? budgetBox.getSelectedId() : s.budgetBlocks;

    // Channels beyond the host's keep their bit: a wider layout later gets them.
    for (int c = 0; c < inputButtons.size(); ++c)
        s.inputMask = inputButtons[c]->getToggleState() ? s.inputMask | (std::uint64_t (1) << c)
                                                        : s.inputMask & ~(std::uint64_t (1) << c);
    for (int c = 0; c < outputButtons.size(); ++c)
        s.outputMask = outputButtons[c]->getToggleState() ? s.outputMask | (std::uint64_t (1) << c)
                                                          : s.outputMask & ~(std::uint64_t (1) << c);
    processor.setStreamSettings (s);
}

void SettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.setColour (theme::dimText);
    g.drawText ("Block size N", blockLabel, juce::Justification::centredLeft);
    g.drawText ("Network budget D", budgetLabel, juce::Justification::centredLeft);
    g.drawText ("INPUTS sent to the server", inputsLabel, juce::Justification::centredLeft);
    g.drawText ("OUTPUTS received from the server", outputsLabel, juce::Justification::centredLeft);
    g.drawText ("LOCAL PLUGINS (for the plugins' own editors)", localArea.withHeight (18), juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.drawFittedText (latencyText, latencyArea, juce::Justification::centredLeft, 2);
    g.drawText (localText, localArea.withTrimmedTop (22).withHeight (24), juce::Justification::centredLeft, true);
}

void SettingsPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);

    // Local plugins, at the bottom.
    localArea = r.removeFromBottom (48);
    auto buttons = localArea.withTrimmedTop (22).removeFromRight (190);
    fullRescanButton.setBounds (buttons.removeFromRight (100).withHeight (24));
    buttons.removeFromRight (6);
    rescanButton.setBounds (buttons.removeFromRight (80).withHeight (24));
    localArea = localArea.withTrimmedRight (200);
    r.removeFromBottom (8);

    auto row = r.removeFromTop (26);
    blockLabel = row.removeFromLeft (120);
    blockBox.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (24);
    budgetLabel = row.removeFromLeft (130);
    budgetBox.setBounds (row.removeFromLeft (110));
    r.removeFromTop (6);
    latencyArea = r.removeFromTop (34);
    r.removeFromTop (10);

    constexpr int cell = 34, perRow = 16;
    auto layoutGrid = [&] (juce::Rectangle<int>& area, juce::Rectangle<int>& label, juce::TextButton& all,
                           juce::OwnedArray<juce::TextButton>& buttons)
    {
        auto head = area.removeFromTop (22);
        all.setBounds (head.removeFromRight (50));
        label = head;
        area.removeFromTop (4);
        const int rowsNeeded = (buttons.size() + perRow - 1) / perRow;
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setBounds (area.getX() + (i % perRow) * (cell + 2), area.getY() + (i / perRow) * 24, cell, 22);
        area.removeFromTop (rowsNeeded * 24 + 10);
    };
    layoutGrid (r, inputsLabel, allInputs, inputButtons);
    layoutGrid (r, outputsLabel, allOutputs, outputButtons);
}

} // namespace fxmenet::plugin
