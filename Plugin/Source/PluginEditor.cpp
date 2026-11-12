/*
  ------------------------------------------------------------------------------
    PluginEditor.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginEditor.h"
#include "Tooltips.h"

namespace theme = fxmenet::plugin::theme;
namespace tips  = fxmenet::plugin::tips;
using fxmenet::client::ClientEngine;

FxmeNetAudioProcessorEditor::FxmeNetAudioProcessorEditor (FxmeNetAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    laf.setAccentColour (theme::accent);
    setLookAndFeel (&laf);

    // Top bar: the "?" switch and info.
    topBar.setAccentColour (theme::accent);
    topBar.setBackgroundColour (theme::topBarBg);
    topBar.setDecoration (assetImage ("icon_png"));
    addAndMakeVisible (topBar);

    tooltipsButton.setButtonText ("?");
    tooltipsButton.setAccent (theme::accent, theme::dimText);
    tooltipsButton.setTooltip (tips::tooltips);
    tooltipsButton.setToggleState (fxmenet::plugin::getUiTooltips(), juce::dontSendNotification);
    tooltipsButton.onClick = [this] { fxmenet::plugin::setUiTooltips (tooltipsButton.getToggleState()); };

    infoButton.setColours ({ theme::accent, theme::text, theme::panel, theme::panelLine });
    infoButton.setInfo (theme::displayName,
                        "Runs a chain of plugins on an FxmeNet server on the local network: "
                        "the audio goes to the server and back, the DAW sees one plugin.\n\n"
                        "Pick or type the server, connect, then add plugins from the server's list. "
                        "Their parameters appear among this plugin's 256 automation parameters, "
                        "named after them.\n\n"
                        "The added latency (network budget and re-blocking) is reported to the DAW, "
                        "which compensates it.");
    topBar.setRightControls ({ { &tooltipsButton, 24 }, { &infoButton, 22 } });

    // Server.
    serverBox.setEditableText (true);
    serverBox.setTextWhenNothingSelected ("host or host:port");
    serverBox.setTooltip (tips::server);
    const auto current = processor.getServer();
    serverBox.setText (current.isNotEmpty() ? current : fxmenet::plugin::getLastServer(), juce::dontSendNotification);
    serverBox.onChange = [this]
    {
        const int i = serverBox.getSelectedItemIndex();
        if (juce::isPositiveAndBelow (i, (int) found.size()))
            serverBox.setText (found[(std::size_t) i].host + ":" + juce::String (found[(std::size_t) i].controlPort),
                               juce::dontSendNotification);
    };
    addAndMakeVisible (serverBox);

    findButton.setTooltip (tips::find);
    findButton.onClick = [this] { findServers(); };
    connectButton.setTooltip (tips::connect);
    connectButton.onClick = [this] { connectClicked(); };

    // Lists.
    catalogList.setTooltip (tips::catalog);
    catalogRows.onDoubleClick = [this] (int) { addSelected(); };
    chainList.setTooltip (tips::chain);
    for (auto* list : { &catalogList, &chainList })
    {
        list->setRowHeight (theme::rowHeight);
        list->setColour (juce::ListBox::backgroundColourId, theme::panel);
        list->setColour (juce::ListBox::outlineColourId, theme::panelLine);
        list->setOutlineThickness (1);
        addAndMakeVisible (*list);
    }

    // The filter above the server's plugins: Return adds the selected one
    // (the first match is selected while typing), Escape clears it (the
    // focus fixer reverts the field to its text before typing).
    filterBox.setTextToShowWhenEmpty ("Filter", theme::dimText);
    filterBox.setTooltip (tips::filter);
    filterBox.setColour (juce::TextEditor::backgroundColourId, theme::panel);
    filterBox.setColour (juce::TextEditor::outlineColourId, theme::panelLine);
    filterBox.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    filterBox.setColour (juce::TextEditor::textColourId, theme::text);
    filterBox.setFont (juce::Font (juce::FontOptions (12.5f)));
    filterBox.setIndents (6, 2);
    filterBox.onTextChange = [this] { refreshCatalog(); };
    filterBox.onReturnKey = [this] { addSelected(); };
    addAndMakeVisible (filterBox);

    addButton.setTooltip (tips::add);
    addButton.onClick = [this] { addSelected(); };
    upButton.setTooltip (tips::moveUp);
    downButton.setTooltip (tips::moveDown);
    bypassButton.setTooltip (tips::bypass);
    removeButton.setTooltip (tips::remove);

    auto move = [this] (int delta)
    {
        const auto id = selectedSlot();
        const int row = chainList.getSelectedRow();
        if (id == 0)
            return;
        processor.getEngine().moveSlot (id, row + delta);
        chainList.selectRow (juce::jlimit (0, (int) slots.size() - 1, row + delta));
    };
    upButton.onClick = [move] { move (-1); };
    downButton.onClick = [move] { move (1); };
    bypassButton.onClick = [this]
    {
        const int row = chainList.getSelectedRow();
        if (juce::isPositiveAndBelow (row, (int) slots.size()))
            processor.getEngine().setBypass (slots[(std::size_t) row].slotId, ! slots[(std::size_t) row].bypassed);
    };
    removeButton.onClick = [this]
    {
        if (const auto id = selectedSlot())
            processor.getEngine().removeSlot (id);
    };

    for (auto* b : { &findButton, &connectButton, &addButton, &upButton, &downButton, &bypassButton, &removeButton })
    {
        styleButton (*b);
        addAndMakeVisible (*b);
    }

    // A slot's own editor, in the Parameters tab: the window adapts to it,
    // and drops it before its shadow goes.
    parameterPanel.onWantsSize = [this] (int w, int h) { fitToPluginEditor (w, h); };
    processor.beforeShadowRemoved = [this] (std::uint32_t slotId) { parameterPanel.dropPluginEditor (slotId); };

    // Tabs.
    tabs.setTabBarDepth (26);
    tabs.setOutline (0);
    tabs.addTab ("Parameters", theme::panel, &parameterPanel, false);
    tabs.addTab ("Mapping", theme::panel, &mappingPanel, false);
    tabs.addTab ("Routing", theme::panel, &routingPanel, false);
    tabs.addTab ("Settings", theme::panel, &settingsPanel, false);
    addAndMakeVisible (tabs);

    setSize (theme::width, theme::height);
    timerCallback();
    startTimerHz (10);
}

FxmeNetAudioProcessorEditor::~FxmeNetAudioProcessorEditor()
{
    stopTimer();
    processor.beforeShadowRemoved = nullptr;
    parameterPanel.onWantsSize = nullptr;
    parameterPanel.dropPluginEditor();
    setLookAndFeel (nullptr);
}

//==============================================================================
void FxmeNetAudioProcessorEditor::timerCallback()
{
    auto& engine = processor.getEngine();

    const auto status = engine.getStatus();
    statusLine = engine.getStatusText();
    if (status == ClientEngine::Status::ready && engine.getServerOs().isNotEmpty())
        statusLine << " (" << engine.getServerOs() << ")";
    statusColour = status == ClientEngine::Status::ready  ? theme::accent
                 : status == ClientEngine::Status::idle   ? theme::dimText
                                                          : theme::warning;
    connectButton.setButtonText (processor.getServer().isNotEmpty() ? "Disconnect" : "Connect");

    const auto& st = engine.getStatistics();
    const int latency = engine.getLatency();
    const double rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    statsLine = "Latency " + juce::String (latency) + " samples (" + juce::String (1000.0 * latency / rate, 1)
              + " ms)   round trip " + juce::String (st.lastRttMicros.load() / 1000.0, 2) + " ms"
              + "   concealed " + juce::String ((juce::int64) engine.getConcealedCount())
              + "   rescued " + juce::String ((juce::int64) st.rescued.load())
              + "   UDP dropped " + juce::String ((juce::int64) st.udpDropped.load())
              + (st.udpTooLarge.load() > 0 ? " (" + juce::String ((juce::int64) st.udpTooLarge.load()) + " too large)" : juce::String())
              + "   resets " + juce::String ((juce::int64) engine.getResetCount())
              + "   host ahead " + juce::String (engine.getHostAheadMs(), 0) + " ms"
              + "   parameters mapped " + juce::String (processor.getNumMapped()) + "/" + juce::String (fxmenet::plugin::poolSize);

    refreshCatalog();
    refreshChain();

    const int row = chainList.getSelectedRow();
    parameterPanel.refresh (juce::isPositiveAndBelow (row, (int) slots.size()) ? &slots[(std::size_t) row] : nullptr);
    mappingPanel.refresh();
    routingPanel.refresh();
    settingsPanel.refresh();
    repaint (statusArea);
    repaint (statsArea);
}

void FxmeNetAudioProcessorEditor::refreshCatalog()
{
    const auto filter = filterBox.getText().trim();
    auto latest = processor.getEngine().getCatalog();
    juce::String signature;
    signature << (int) processor.getLocalCatalog().getRevision() << ":"
              << processor.getJsfxLibrary().getRevision() << ":" << filter << ":";
    for (auto& e : latest)
        signature << fxmenet::control::json::hex32 (e.uid) << e.name << ";";
    if (signature == catalogSignature)
        return;
    catalogSignature = signature;

    // This machine's JSFX scripts, after the server's plugins.
    numServerEntries = (int) latest.size();
    scripts = processor.getJsfxLibrary().getScripts();
    for (auto& s : scripts)
    {
        fxmenet::control::PluginEntry e;
        e.uid = fxmenet::jsfx::jsfxUid;
        e.name = s.path;
        e.vendor = fxmenet::jsfx::jsfxVendor;
        e.category = s.description;
        latest.push_back (std::move (e));
    }

    // The selection follows its plugin, not its row.
    const auto* previous = catalogEntryAt (catalogList.getSelectedRow());
    const bool hadSelection = previous != nullptr;
    const auto selectedUid = hadSelection ? previous->uid : 0u;
    const auto selectedName = hadSelection ? previous->name : juce::String();
    const bool filterChanged = filter != shownFilter;
    shownFilter = filter;
    catalog = std::move (latest);

    // Every word of the filter must be in the name.
    const auto words = juce::StringArray::fromTokens (filter, " ", "");
    std::vector<fxmenet::plugin::LocalMatch> matches;
    matches.reserve (catalog.size());
    shown.clear();
    for (std::size_t i = 0; i < catalog.size(); ++i)
    {
        const bool script = (int) i >= numServerEntries;
        matches.push_back (script ? fxmenet::plugin::LocalMatch {} : processor.matchLocal (catalog[i]));
        bool keep = true;
        for (auto& w : words)   // a script: its path or its description
            keep = keep && (catalog[i].name.containsIgnoreCase (w)
                            || (script && catalog[i].category.containsIgnoreCase (w)));
        if (keep)
            shown.push_back ((int) i);
    }

    // T2, T1, T0 (and "T?" while the local scan runs), then the JSFX scripts; by name.
    const auto rank = [&] (int i)
    {
        if (i >= numServerEntries)
            return -2;
        const auto& m = matches[(std::size_t) i];
        return m.kind == fxmenet::plugin::LocalMatch::Kind::unknown ? -1 : m.tier;
    };
    std::stable_sort (shown.begin(), shown.end(), [&] (int a, int b)
    {
        const int ra = rank (a), rb = rank (b);
        if (ra != rb)
            return ra > rb;
        const auto& ea = catalog[(std::size_t) a];
        const auto& eb = catalog[(std::size_t) b];
        if (const int byName = ea.name.compareNatural (eb.name); byName != 0)
            return byName < 0;
        return ea.vendor.compareNatural (eb.vendor) < 0;
    });

    catalogRows.rows.clear();
    int reselect = -1;
    for (auto i : shown)
    {
        const auto& e = catalog[(std::size_t) i];
        const auto& m = matches[(std::size_t) i];
        if (hadSelection && e.uid == selectedUid && e.name == selectedName)
            reselect = (int) catalogRows.rows.size();
        if (i >= numServerEntries)
        {
            catalogRows.rows.push_back ({ e.name, "JSFX  JS", theme::text,
                                          e.category + "\nA JSFX script of this machine: sent to the server when added, "
                                                       "processed there, shown here with generic controls." });
            continue;
        }
        catalogRows.rows.push_back ({ e.name,
                                      e.vendor + "  " + juce::String (e.inputs) + "/" + juce::String (e.outputs)
                                          + "  " + tierTag (m),
                                      tierColour (m),
                                      m.text });
    }
    // While typing, the first match is selected, so Return adds it.
    if (reselect < 0 && filterChanged && filter.isNotEmpty() && ! shown.empty())
        reselect = 0;

    catalogCount = filter.isEmpty() ? juce::String ((int) catalog.size())
                                    : juce::String ((int) shown.size()) + " / " + juce::String ((int) catalog.size());
    catalogList.updateContent();
    if (reselect >= 0)
        catalogList.selectRow (reselect);
    else
        catalogList.deselectAllRows();
    catalogList.repaint();
    repaint (catalogTitle);
}

void FxmeNetAudioProcessorEditor::refreshChain()
{
    slots = processor.getEngine().getSlotInfos();
    juce::String signature;
    signature << (int) processor.getLocalCatalog().getRevision() << ":" << catalogSignature << ":";
    for (auto& s : slots)
        signature << (int) s.slotId << (s.loaded ? "L" : "-") << (s.bypassed ? "B" : "-") << s.error << ";";
    if (signature == chainSignature)
        return;
    chainSignature = signature;

    chainRows.rows.clear();
    int position = 1;
    for (auto& s : slots)
    {
        juce::String detail;
        juce::Colour colour = theme::text;
        fxmenet::control::PluginEntry entry;
        const bool script = FxmeNetAudioProcessor::isJsfx (s);
        auto m = ! script && processor.findCatalogEntry (s, entry) ? processor.matchLocal (entry) : fxmenet::plugin::LocalMatch {};
        if (script)
            m.text = "A JSFX script, processed on the server. "
                     + juce::String (s.info.jsfxGfx ? "Its @gfx display is drawn here, from the server's data."
                                                    : "Generic controls (it has no @gfx section).");
        if (s.error.isNotEmpty())
        {
            detail = s.error;
            colour = theme::error;
        }
        else if (! s.loaded)
        {
            detail = "loading";
            colour = theme::warning;
        }
        else
        {
            detail = (script ? juce::String ("JS") : tierTag (m)) + "  " + juce::String ((int) s.info.params.size()) + " parameters"
                   + (s.info.latency > 0 ? ", latency " + juce::String (s.info.latency) : juce::String());
        }
        if (s.bypassed)
        {
            detail = "bypassed  " + detail;
            colour = theme::dimText;
        }
        chainRows.rows.push_back ({ juce::String (position++) + "  " + s.name, detail, colour, m.text });
    }
    chainList.updateContent();
    chainList.repaint();
}

//==============================================================================
void FxmeNetAudioProcessorEditor::connectClicked()
{
    if (processor.getServer().isNotEmpty())
        processor.connectTo ({});
    else
        processor.connectTo (serverBox.getText());
    timerCallback();
}

void FxmeNetAudioProcessorEditor::findServers()
{
    findButton.setEnabled (false);
    findButton.setButtonText ("...");
    juce::Component::SafePointer<FxmeNetAudioProcessorEditor> safe (this);
    juce::Thread::launch ([safe]
    {
        auto servers = fxmenet::discovery::discover (1000);
        juce::MessageManager::callAsync ([safe, servers]
        {
            if (safe != nullptr)
                safe->showServers (servers);
        });
    });
}

void FxmeNetAudioProcessorEditor::showServers (const std::vector<fxmenet::discovery::Announce>& servers)
{
    findButton.setEnabled (true);
    findButton.setButtonText ("Find");
    found = servers;

    const auto typed = serverBox.getText();
    serverBox.clear (juce::dontSendNotification);
    int id = 1;
    for (auto& a : found)
        serverBox.addItem (a.name + "  (" + a.host + ", " + a.os + ", " + juce::String (a.sessions) + " sessions)", id++);
    serverBox.setText (typed, juce::dontSendNotification);
    if (! found.empty())
        serverBox.showPopup();
}

void FxmeNetAudioProcessorEditor::addSelected()
{
    const int row = catalogList.getSelectedRow();
    const auto* e = catalogEntryAt (row);
    if (e == nullptr)
        return;

    const int index = shown[(std::size_t) row] - numServerEntries;
    if (! juce::isPositiveAndBelow (index, (int) scripts.size()))
        return processor.addPlugin (*e);

    // A JSFX script: packed with what it imports, then sent.
    juce::String error;
    juce::StringArray notes;
    if (! processor.addScript (scripts[(std::size_t) index], error, &notes))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "JSFX",
                                                scripts[(std::size_t) index].path + "\n\n" + error);
    else if (! notes.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "JSFX",
                                                scripts[(std::size_t) index].path + " was added, without:\n\n"
                                                    + notes.joinIntoString ("\n"));
}

const fxmenet::control::PluginEntry* FxmeNetAudioProcessorEditor::catalogEntryAt (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size()))
        return nullptr;
    const auto i = shown[(std::size_t) row];
    return juce::isPositiveAndBelow (i, (int) catalog.size()) ? &catalog[(std::size_t) i] : nullptr;
}

std::uint32_t FxmeNetAudioProcessorEditor::selectedSlot() const
{
    const int row = chainList.getSelectedRow();
    return juce::isPositiveAndBelow (row, (int) slots.size()) ? slots[(std::size_t) row].slotId : 0;
}

//==============================================================================
void FxmeNetAudioProcessorEditor::RowList::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;
    const auto& r = rows[(std::size_t) row];
    if (selected)
        g.fillAll (theme::selection);

    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (8, 0);
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.setColour (r.colour);
    g.drawText (r.text, area.removeFromLeft (width / 2), juce::Justification::centredLeft, true);
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    g.setColour (theme::dimText);
    g.drawText (r.detail, area, juce::Justification::centredRight, true);
}

void FxmeNetAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.setColour (theme::dimText);
    auto title = catalogTitle;
    g.drawText ("SERVER PLUGINS", title.removeFromLeft (110), juce::Justification::centredLeft);
    g.drawText (catalogCount, title.withTrimmedRight (filterBox.getWidth() + 8), juce::Justification::centredRight);
    g.drawText ("CHAIN", chainTitle, juce::Justification::centredLeft);
    g.drawText (statsLine, statsArea, juce::Justification::centredLeft, true);

    g.setColour (statusColour);
    g.drawText (statusLine, statusArea, juce::Justification::centredLeft, true);
}

void FxmeNetAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    topBar.setBounds (area.removeFromTop (theme::topBarHeight));
    area.reduce (14, 10);

    // Server row.
    auto row = area.removeFromTop (26);
    serverBox.setBounds (row.removeFromLeft (240));
    row.removeFromLeft (6);
    findButton.setBounds (row.removeFromLeft (60));
    row.removeFromLeft (6);
    connectButton.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (12);
    statusArea = row;

    statsArea = area.removeFromBottom (22);
    area.removeFromBottom (6);
    area.removeFromTop (10);

    // Server plugins above the chain on the left, the tabs on the right.
    auto left = area.removeFromLeft (330);
    area.removeFromLeft (14);
    tabs.setBounds (area);

    auto top = left.removeFromTop ((left.getHeight() - 10) / 2);
    left.removeFromTop (10);

    catalogTitle = top.removeFromTop (22);
    filterBox.setBounds (catalogTitle.withTrimmedLeft (catalogTitle.getWidth() - 140).withTrimmedBottom (2));
    addButton.setBounds (top.removeFromBottom (26).removeFromRight (80));
    top.removeFromBottom (6);
    catalogList.setBounds (top);

    chainTitle = left.removeFromTop (18);
    auto buttons = left.removeFromBottom (26);
    left.removeFromBottom (6);
    chainList.setBounds (left);
    const int w = (buttons.getWidth() - 3 * 6) / 4;
    for (auto* b : { &upButton, &downButton, &bypassButton, &removeButton })
    {
        b->setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (6);
    }
}

//==============================================================================
void FxmeNetAudioProcessorEditor::styleButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, theme::panel);
    b.setColour (juce::TextButton::textColourOffId, theme::text);
}

void FxmeNetAudioProcessorEditor::fitToPluginEditor (int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        if (getWidth() != theme::width || getHeight() != theme::height)
            setSize (theme::width, theme::height);
        return;
    }

    // What the tab gives the editor now, at the default size.
    const int availableW = tabs.getWidth();
    const int availableH = tabs.getHeight() - tabs.getTabBarDepth() - fxmenet::plugin::ParameterPanel::topRowHeight;
    if (availableW <= 0 || availableH <= 0)
        return;
    const int extraW = juce::jmax (0, w - availableW + (getWidth() - theme::width));
    const int extraH = juce::jmax (0, h - availableH + (getHeight() - theme::height));
    setSize (theme::width + extraW, theme::height + extraH);
}

juce::String FxmeNetAudioProcessorEditor::tierTag (const fxmenet::plugin::LocalMatch& m)
{
    return m.kind == fxmenet::plugin::LocalMatch::Kind::unknown ? juce::String ("T?") : "T" + juce::String (m.tier);
}

juce::Colour FxmeNetAudioProcessorEditor::tierColour (const fxmenet::plugin::LocalMatch& m)
{
    switch (m.kind)
    {
        case fxmenet::plugin::LocalMatch::Kind::compatible:    return m.tier == 2 ? theme::accent : theme::text;
        case fxmenet::plugin::LocalMatch::Kind::otherVersion:  return theme::warning;
        case fxmenet::plugin::LocalMatch::Kind::missing:
        case fxmenet::plugin::LocalMatch::Kind::unknown:       break;
    }
    return theme::dimText;
}

juce::Image FxmeNetAudioProcessorEditor::assetImage (const char* resourceName)
{
    int size = 0;
    if (const char* data = BinaryData::getNamedResource (resourceName, size))
        return juce::ImageCache::getFromMemory (data, size);
    return {};
}
