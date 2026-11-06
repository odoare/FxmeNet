/*
  ------------------------------------------------------------------------------
    Panels.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Panels.h"
#include "AppSettings.h"
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
    addChildComponent (editorViewport);

    viewButton.setTooltip (tips::editorView);
    viewButton.onClick = [this]
    {
        preferPluginEditor = ! preferPluginEditor;
        dropPluginEditor();
        shownSignature = {};   // rebuilt by the next refresh
    };
    styleButton (viewButton);
    addChildComponent (viewButton);

    lastTouchedButton.setTooltip (tips::lastTouched);
    lastTouchedButton.onClick = [this] { lastTouchedMenu(); };
    styleButton (lastTouchedButton);
    addChildComponent (lastTouchedButton);
}

void ParameterPanel::lastTouchedMenu()
{
    const auto target = processor.getLastTouched();
    if (! target.isMapped() || target.slotId != editorSlot)
        return;

    const int pool = processor.findPoolFor (target);
    juce::PopupMenu menu;
    if (pool >= 0)
        menu.addItem (1, "Unmap from " + poolLabel (pool));
    else
        menu.addItem (2, "Map to a new automation parameter");
    menu.addItem (3, "Learn: map it to the next FxmeNet parameter moved in the DAW");
    if (processor.getLearnTarget().isMapped())
        menu.addItem (4, "Cancel learn");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&lastTouchedButton),
                        [safe = juce::Component::SafePointer<ParameterPanel> (this), target, pool] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            auto& p = safe->processor;
                            switch (result)
                            {
                                case 1: p.unmapPool (pool); break;
                                case 2: p.mapToFreePool (target); break;
                                case 3: p.startLearn (target); break;
                                case 4: p.cancelLearn(); break;
                                default: break;
                            }
                        });
}

ParameterPanel::~ParameterPanel()
{
    dropPluginEditor();
}

void ParameterPanel::dropPluginEditor (std::uint32_t slotId)
{
    if (pluginEditor == nullptr || (slotId != 0 && slotId != editorSlot))
        return;
    pluginEditor->removeComponentListener (this);
    editorViewport.setViewedComponent (nullptr, false);
    pluginEditor.reset();   // editorBeingDeleted on the shadow
    processor.setEditorShown (editorSlot, false);   // no more telemetry
    editorSlot = 0;
    editorViewport.setVisible (false);
    if (onWantsSize)
        onWantsSize (0, 0);
}

void ParameterPanel::refresh (const client::SlotModel* slot)
{
    const std::uint32_t id = slot != nullptr ? slot->slotId : 0;
    if (pluginEditor != nullptr && editorSlot != id)
        dropPluginEditor();

    // The plugin's own editor, when it can have one here.
    juce::String why;
    fxmenet::plugin::Shadow* shadow = nullptr;
    if (slot != nullptr && slot->loaded)
        shadow = processor.getShadow (id, &why);
    const bool canShowEditor = shadow != nullptr && shadow->getInstance().hasEditor();

    viewButton.setVisible (canShowEditor);
    viewButton.setButtonText (preferPluginEditor ? "Generic controls" : "Plugin editor");

    if (canShowEditor && preferPluginEditor)
    {
        if (pluginEditor == nullptr)
        {
            rows.clear();
            viewport.setVisible (false);
            message = {};
            pluginEditor.reset (shadow->getInstance().createEditorAndMakeActive());
            if (pluginEditor != nullptr)
            {
                editorSlot = id;
                processor.setEditorShown (id, true);   // the server sends its telemetry
                pluginEditor->addComponentListener (this);
                editorViewport.setViewedComponent (pluginEditor.get(), false);
                editorViewport.setVisible (true);
                info = slot->name + ": its own editor";
                shownSlot = id;
                shownSignature = "editor";
                if (onWantsSize)
                    onWantsSize (pluginEditor->getWidth(), pluginEditor->getHeight());
                resized();
                repaint();
            }
        }
        if (pluginEditor != nullptr)
        {
            // The last control touched in the plugin's editor, to map it.
            const auto touched = processor.getLastTouched();
            const bool here = touched.isMapped() && touched.slotId == id;
            juce::String text = "Map last touched";
            if (here)
                for (auto& p : slot->info.params)
                    if ((std::uint32_t) p.index == touched.paramIndex)
                    {
                        const int pool = processor.findPoolFor (touched);
                        text = (processor.getLearnTarget() == touched ? "Learning: " : "Last touched: ")
                             + p.name + (pool >= 0 ? " (" + poolLabel (pool) + ")" : juce::String (", unmapped"));
                    }
            lastTouchedButton.setButtonText (text);
            lastTouchedButton.setEnabled (here);
            lastTouchedButton.setVisible (true);
            return;
        }
    }

    if (pluginEditor != nullptr)
        dropPluginEditor();
    lastTouchedButton.setVisible (false);
    info = slot == nullptr ? juce::String()
         : canShowEditor   ? slot->name + ": generic controls"
         : why.isNotEmpty() ? slot->name + ": generic controls (" + why + ")"
                            : slot->name;
    showGeneric (slot);
}

void ParameterPanel::showGeneric (const client::SlotModel* slot)
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
        viewport.setVisible (true);
        message = slot == nullptr ? "Select a plugin in the chain to see its parameters."
                : ! slot->loaded  ? slot->name + " is not loaded on the server."
                                  : juce::String();

        if (slot != nullptr && slot->loaded)
            for (auto& paramInfo : slot->info.params)
            {
                rows.push_back (std::make_unique<Row> (processor, id, paramInfo));
                content.addAndMakeVisible (*rows.back());
            }
        resized();
        repaint();
    }

    if (slot != nullptr && slot->loaded)
        for (std::size_t i = 0; i < rows.size() && i < slot->info.params.size(); ++i)
            rows[i]->update (client::ClientEngine::currentValue (*slot, slot->info.params[i]));
}

void ParameterPanel::componentMovedOrResized (juce::Component& c, bool, bool wasResized)
{
    if (&c == pluginEditor.get() && wasResized && onWantsSize)
        onWantsSize (c.getWidth(), c.getHeight());
}

void ParameterPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.setColour (theme::dimText);
    g.drawText (info, infoArea, juce::Justification::centredLeft, true);
    if (message.isNotEmpty())
    {
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (message, getLocalBounds().withTrimmedTop (topRowHeight).reduced (12), juce::Justification::centred, true);
    }
}

void ParameterPanel::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (topRowHeight).reduced (8, 3);
    viewButton.setBounds (top.removeFromRight (130));
    top.removeFromRight (6);
    lastTouchedButton.setBounds (top.removeFromRight (juce::jmin (300, top.getWidth() / 2)));
    infoArea = top;

    editorViewport.setBounds (r);
    viewport.setBounds (r);
    const int w = viewport.getWidth() - viewport.getScrollBarThickness();
    content.setSize (w, (int) rows.size() * 26);
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds (0, y, w, 26);
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

SettingsPanel::SettingsPanel (FxmeNetAudioProcessor& p) : processor (p), routingView (p)
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

    resetRoutingButton.setTooltip ("Back to the default routing: each plugin fed by the one above it, in series, "
                                   "and the outputs by the last one.");
    resetRoutingButton.onClick = [this] { processor.getEngine().resetRouting(); };
    addAndMakeVisible (routingView);

    rescanButton.setTooltip (tips::rescan);
    fullRescanButton.setTooltip (tips::fullRescan);
    rescanButton.onClick = [this] { processor.getLocalCatalog().rescan (false); };
    fullRescanButton.onClick = [this] { processor.getLocalCatalog().rescan (true); };
    foldersButton.setTooltip (tips::pluginFolders);
    foldersButton.onClick = [this] { foldersMenu(); };

    flushButton.setTooltip (tips::flushOnReset);
    logButton.setTooltip (tips::logEvents);
    for (auto* b : { &flushButton, &logButton })
    {
        styleButton (*b);
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    flushButton.onClick = [this]
    {
        setFlushOnReset (flushButton.getToggleState());
        processor.getEngine().setFlushOnReset (flushButton.getToggleState());
    };
    logButton.onClick = [this] { setLogEvents (logButton.getToggleState()); };

    for (auto* b : { &resetRoutingButton, &rescanButton, &fullRescanButton, &foldersButton })
    {
        styleButton (*b);
        addAndMakeVisible (*b);
    }
}

void SettingsPanel::refresh()
{
    const auto s = processor.getStreamSettings();
    int blockIndex = 0;
    for (int i = 0; i < blockBox.getNumItems(); ++i)
        if (blockBox.getItemText (i).getIntValue() == s.blockSize)
            blockIndex = i;
    blockBox.setSelectedItemIndex (blockIndex, juce::dontSendNotification);
    budgetBox.setSelectedId (s.budgetBlocks, juce::dontSendNotification);


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

    routingView.refresh();

    flushButton.setToggleState (processor.getEngine().getFlushOnReset(), juce::dontSendNotification);
    logButton.setToggleState (getLogEvents(), juce::dontSendNotification);

    auto& local = processor.getLocalCatalog();
    const auto lt = local.getProgress();
    rescanButton.setEnabled (! local.isScanning());
    fullRescanButton.setEnabled (! local.isScanning());
    if (lt != localText)
    {
        localText = lt;
        repaint (localArea);
    }

    const auto folders = local.getExtraFolders();
    const auto ft = folders.isEmpty() ? juce::String ("Searched: the standard VST3 folders")
                                      : "Searched: the standard VST3 folders, and " + folders.joinIntoString ("; ");
    if (ft != foldersText)
    {
        foldersText = ft;
        repaint (foldersArea);
    }
}

void SettingsPanel::foldersMenu()
{
    auto& local = processor.getLocalCatalog();
    const auto folders = local.getExtraFolders();

    juce::PopupMenu menu, remove;
    for (int i = 0; i < folders.size(); ++i)
        remove.addItem (100 + i, folders[i]);
    menu.addItem (1, "Add a folder...");
    menu.addSubMenu ("Remove", remove, ! folders.isEmpty());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (foldersButton),
                        [this, folders] (int result)
    {
        auto& catalog = processor.getLocalCatalog();
        if (result >= 100 && result - 100 < folders.size())
        {
            auto remaining = catalog.getExtraFolders();
            remaining.removeString (folders[result - 100]);
            catalog.setExtraFolders (remaining);
        }
        else if (result == 1)
        {
            chooser = std::make_unique<juce::FileChooser> ("A folder holding VST3 plugins",
                                                           juce::File::getSpecialLocation (juce::File::userHomeDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
            {
                const auto folder = fc.getResult();
                if (! folder.isDirectory())
                    return;
                auto& scan = processor.getLocalCatalog();
                auto current = scan.getExtraFolders();
                if (current.contains (folder.getFullPathName()))
                    return;
                current.add (folder.getFullPathName());
                scan.setExtraFolders (current);
            });
        }
    });
}

void SettingsPanel::apply()
{
    auto s = processor.getStreamSettings();
    s.blockSize = blockBox.getText().getIntValue() > 0 ? blockBox.getText().getIntValue() : s.blockSize;
    s.budgetBlocks = budgetBox.getSelectedId() > 0 ? budgetBox.getSelectedId() : s.budgetBlocks;

    processor.setStreamSettings (s);
}

void SettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.setColour (theme::dimText);
    g.drawText ("Block size N", blockLabel, juce::Justification::centredLeft);
    g.drawText ("Network budget D", budgetLabel, juce::Justification::centredLeft);
    g.drawText ("ROUTING", routingLabel, juce::Justification::centredLeft);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText ("Drag between pins or bus names; shift-click a connection to remove it; right-click an input pin.",
                routingHint, juce::Justification::centredLeft, true);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText ("LOCAL PLUGINS (for the plugins' own editors)", localArea.withHeight (18), juce::Justification::centredLeft);
    g.drawText ("TRANSPORT", diagnosticsLabel, juce::Justification::centredLeft);
    g.setColour (theme::text);
    g.drawFittedText (latencyText, latencyArea, juce::Justification::centredLeft, 2);
    g.drawText (localText, localArea.withTrimmedTop (22).withHeight (24), juce::Justification::centredLeft, true);
    g.setColour (theme::dimText);
    g.drawText (foldersText, foldersArea, juce::Justification::centredLeft, true);
}

void SettingsPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);

    // Local plugins, at the bottom.
    localArea = r.removeFromBottom (70);
    foldersArea = localArea.withTrimmedTop (50);
    auto buttons = localArea.withTrimmedTop (22).removeFromRight (286);
    fullRescanButton.setBounds (buttons.removeFromRight (100).withHeight (24));
    buttons.removeFromRight (6);
    rescanButton.setBounds (buttons.removeFromRight (80).withHeight (24));
    buttons.removeFromRight (6);
    foldersButton.setBounds (buttons.removeFromRight (90).withHeight (24));
    localArea = localArea.withTrimmedRight (296);
    r.removeFromBottom (8);

    // Reset behaviour and the events log, above.
    auto diagnostics = r.removeFromBottom (24);
    diagnosticsLabel = diagnostics.removeFromLeft (90);
    flushButton.setBounds (diagnostics.removeFromLeft (230));
    diagnostics.removeFromLeft (6);
    logButton.setBounds (diagnostics.removeFromLeft (170));
    r.removeFromBottom (8);

    auto row = r.removeFromTop (26);
    blockLabel = row.removeFromLeft (120);
    blockBox.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (24);
    budgetLabel = row.removeFromLeft (130);
    budgetBox.setBounds (row.removeFromLeft (110));
    r.removeFromTop (6);
    latencyArea = r.removeFromTop (34);
    r.removeFromTop (8);

    // The routing: the rest.
    auto head = r.removeFromTop (24);
    resetRoutingButton.setBounds (head.removeFromRight (110));
    routingLabel = head.removeFromLeft (70);
    routingHint = head.withTrimmedRight (8);
    r.removeFromTop (4);
    routingView.setBounds (r);
}

} // namespace fxmenet::plugin
