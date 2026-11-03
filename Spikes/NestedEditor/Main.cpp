/*
  ------------------------------------------------------------------------------
    Main.cpp  (FxmeNetSpikeNested, Phase 0 spike 3)

    A plugin that hosts one VST3 and shows that plugin's own editor inside
    its window, under a small bar (Load, Reload editor, what was loaded).
    FxmeNet's Phase 4 does the same with the shadow instances of the slots:
    on Linux the hosted editor is an X11 window embedded (XEmbed, through
    JUCE's XEmbedComponent) in the plugin's window, itself embedded in the
    DAW's. The hosted plugin also processes the audio, locally, so its
    meters move and there is something to hear.

    Checks, in Reaper on Linux (doc/todo.md, item 3):
      1. the hosted editor appears inside the window, at its size, and
         redraws (meters, spectra);
      2. mouse: knobs drag, buttons click, right-click menus open in place;
      3. keyboard: a text field of the hosted editor takes typing (and the
         DAW does not steal the keys);
      4. a resizable hosted editor resizes the window (and the reverse);
      5. closing and reopening the window, several instances at once,
         moving the window, and Reload editor (delete and recreate it);
      6. no crash when removing the plugin with its window open.

    The hosted plugin is remembered (by path) in the state.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <iostream>

namespace
{

void trace (const juce::String& s)   { std::cerr << "[spike-nested] " << s << std::endl; }

//==============================================================================
class SpikeProcessor final : public juce::AudioProcessor
{
public:
    SpikeProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        juce::addDefaultFormatsToManager (formats);
    }

    ~SpikeProcessor() override
    {
        jassert (getActiveEditor() == nullptr);   // the editor deletes the hosted editor first
    }

    /** Message thread. Replaces the hosted plugin; empty error on success. */
    juce::String load (const juce::File& file)
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : formats.getFormats())
            if (f->fileMightContainThisPluginType (file.getFullPathName()))
                f->findAllTypesForFile (found, file.getFullPathName());
        if (found.isEmpty())
            return "no VST3 plugin in " + file.getFullPathName();

        const double rate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
        const int block = getBlockSize() > 0 ? getBlockSize() : 512;
        juce::String error;
        auto instance = formats.createPluginInstance (*found[0], rate, block, error);
        if (instance == nullptr)
            return "cannot load " + found[0]->name + ": " + error;

        instance->enableAllBuses();
        instance->prepareToPlay (rate, block);
        traceBuses (*instance);

        if (beforeSwap)
            beforeSwap();   // an open editor drops the hosted editor first
        suspendProcessing (true);
        std::swap (hosted, instance);
        allocateScratch (block);
        skippedTraced = false;
        hostedPath = file.getFullPathName();
        suspendProcessing (false);

        if (instance != nullptr)
            instance->releaseResources();   // the previous one, deleted here, on the message thread
        if (afterSwap)
            afterSwap();
        trace ("loaded " + hosted->getName() + " (" + juce::String (hosted->getTotalNumInputChannels()) + " in, "
               + juce::String (hosted->getTotalNumOutputChannels()) + " out), editor: "
               + (hosted->hasEditor() ? "yes" : "no"));
        return {};
    }

    juce::AudioPluginInstance* getHosted() noexcept   { return hosted.get(); }

    /** The VST3 plugins in the format's default locations, sorted by path. */
    juce::StringArray findPlugins()
    {
        juce::StringArray all;
        for (auto* f : formats.getFormats())
            all.addArray (f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, false));
        all.removeDuplicates (false);
        all.removeString ("", false);
        for (int i = all.size(); --i >= 0;)
            if (all[i].contains ("FxmeNetSpikeNested"))
                all.remove (i);   // not itself
        all.sort (true);
        return all;
    }

    /** Set by the open editor (message thread). */
    std::function<void()> beforeSwap, afterSwap;
    juce::String getHostedPath() const               { return hostedPath; }

    //==============================================================================
    void prepareToPlay (double rate, int block) override
    {
        if (hosted != nullptr)
            hosted->prepareToPlay (rate, block);
        allocateScratch (block);
    }

    void releaseResources() override
    {
        if (hosted != nullptr)
            hosted->releaseResources();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (hosted == nullptr)
            return;

        const int n = buffer.getNumSamples();
        const int needed = juce::jmax (hosted->getTotalNumInputChannels(), hosted->getTotalNumOutputChannels());
        if (needed <= buffer.getNumChannels())
        {
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), needed, n);
            hosted->processBlock (view, midi);
            return;
        }

        // More channels than ours (a sidechain bus, say): in a scratch buffer,
        // the extra inputs silent, as the server does with its 64-channel one.
        if (scratch.getNumChannels() < needed || scratch.getNumSamples() < n)
        {
            if (! std::exchange (skippedTraced, true))
                trace ("not processed: the plugin needs " + juce::String (needed) + " channels, scratch too small");
            return;
        }
        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), needed, n);
        view.clear();
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            view.copyFrom (c, 0, buffer, c, 0, n);
        hosted->processBlock (view, midi);
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            buffer.copyFrom (c, 0, view, c, 0, n);
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override         { return "FxmeNetSpikeNested"; }
    bool acceptsMidi() const override                   { return false; }
    bool producesMidi() const override                  { return false; }
    double getTailLengthSeconds() const override        { return 0.0; }
    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::XmlElement x ("FxmeNetSpikeNested");
        x.setAttribute ("path", hostedPath);
        if (hosted != nullptr)
        {
            juce::MemoryBlock state;
            hosted->getStateInformation (state);
            x.setAttribute ("state", state.toBase64Encoding());
        }
        copyXmlToBinary (x, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        const auto x = getXmlFromBinary (data, size);
        if (x == nullptr || x->getStringAttribute ("path").isEmpty())
            return;
        const auto error = load (juce::File (x->getStringAttribute ("path")));
        if (error.isNotEmpty())
        {
            trace (error);
            return;
        }
        juce::MemoryBlock state;
        if (state.fromBase64Encoding (x->getStringAttribute ("state")))
            hosted->setStateInformation (state.getData(), (int) state.getSize());
    }

private:
    void allocateScratch (int block)
    {
        if (hosted != nullptr)
            scratch.setSize (juce::jmax (hosted->getTotalNumInputChannels(), hosted->getTotalNumOutputChannels(), 2),
                             juce::jmax (block, 8192));
    }

    static void traceBuses (juce::AudioPluginInstance& p)
    {
        for (bool input : { true, false })
            for (int b = 0; b < p.getBusCount (input); ++b)
                if (auto* bus = p.getBus (input, b))
                    trace (juce::String (input ? "  input" : "  output") + " bus " + juce::String (b) + " \"" + bus->getName()
                           + "\": " + juce::String (bus->getNumberOfChannels()) + " ch, "
                           + bus->getCurrentLayout().getDescription() + (bus->isEnabled() ? "" : ", disabled"));
        trace ("  total " + juce::String (p.getTotalNumInputChannels()) + " in, "
               + juce::String (p.getTotalNumOutputChannels()) + " out, latency " + juce::String (p.getLatencySamples()));
    }

    juce::AudioBuffer<float> scratch;
    bool skippedTraced = false;
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> hosted;
    juce::String hostedPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeProcessor)
};

//==============================================================================
class SpikeEditor final : public juce::AudioProcessorEditor,
                          private juce::ComponentListener
{
public:
    explicit SpikeEditor (SpikeProcessor& p) : AudioProcessorEditor (&p), processor (p)
    {
        loadButton.onClick = [this] { choose(); };   // a menu of the plugins found
        reloadButton.onClick = [this] { showHosted(); };
        info.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        for (auto* c : std::initializer_list<juce::Component*> { &loadButton, &reloadButton, &info })
            addAndMakeVisible (c);

        processor.beforeSwap = [this] { dropHosted(); };
        processor.afterSwap = [this] { showHosted(); };

        setSize (480, barHeight + 120);
        showHosted();
    }

    ~SpikeEditor() override
    {
        processor.beforeSwap = nullptr;
        processor.afterSwap = nullptr;
        dropHosted();   // before the processor can delete the hosted instance
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1f1e29));
        if (inner == nullptr)
        {
            g.setColour (juce::Colours::grey);
            g.drawText (processor.getHosted() == nullptr ? "Load a VST3 plugin" : "The plugin has no editor",
                        getLocalBounds().withTrimmedTop (barHeight), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop (barHeight).reduced (6, 4);
        loadButton.setBounds (bar.removeFromLeft (70));
        bar.removeFromLeft (6);
        reloadButton.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (8);
        info.setBounds (bar);

        // A resizable hosted editor follows our size (check 4, the reverse way).
        if (inner != nullptr && ! settingSize)
        {
            const juce::ScopedValueSetter<bool> guard (settingSize, true);
            inner->setBounds (getLocalBounds().withTrimmedTop (barHeight));
        }
    }

private:
    static constexpr int barHeight = 30;

    /** A menu of the VST3 plugins in the standard locations (a file dialog
        would open a .vst3 bundle as a folder instead of selecting it). */
    void choose()
    {
        plugins = processor.findPlugins();
        juce::PopupMenu menu;
        juce::String folder;
        for (int i = 0; i < plugins.size(); ++i)
        {
            const juce::File f (plugins[i]);
            const auto parent = f.getParentDirectory().getFullPathName();
            if (parent != folder)
            {
                folder = parent;
                menu.addSectionHeader (folder);
            }
            menu.addItem (i + 1, f.getFileNameWithoutExtension(), true, plugins[i] == processor.getHostedPath());
        }
        if (plugins.isEmpty())
            menu.addItem (-1, "No VST3 plugin found", false);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loadButton),
                            [safe = juce::Component::SafePointer<SpikeEditor> (this)] (int result)
                            {
                                if (safe == nullptr || result <= 0 || result > safe->plugins.size())
                                    return;
                                const auto error = safe->processor.load (juce::File (safe->plugins[result - 1]));   // drops and shows the editor
                                if (error.isNotEmpty())
                                    safe->info.setText (error, juce::dontSendNotification);
                            });
    }

    /** (Re)creates the hosted editor inside this window. */
    void showHosted()
    {
        dropHosted();
        auto* hosted = processor.getHosted();
        if (hosted == nullptr || ! hosted->hasEditor())
        {
            info.setText (hosted == nullptr ? "Nothing loaded" : hosted->getName() + ": no editor", juce::dontSendNotification);
            repaint();
            return;
        }

        inner.reset (hosted->createEditorAndMakeActive());
        if (inner == nullptr)
        {
            info.setText (hosted->getName() + ": createEditor returned null", juce::dontSendNotification);
            return;
        }

        addAndMakeVisible (*inner);
        inner->addComponentListener (this);
        setResizable (inner->isResizable(), false);
        fitToInner();
        info.setText (hosted->getName() + ", editor " + juce::String (inner->getWidth()) + " x "
                          + juce::String (inner->getHeight()) + (inner->isResizable() ? ", resizable" : ""),
                      juce::dontSendNotification);
        trace ("editor shown: " + info.getText());
    }

    void dropHosted()
    {
        if (inner == nullptr)
            return;
        inner->removeComponentListener (this);
        removeChildComponent (inner.get());
        inner.reset();   // calls editorBeingDeleted on the hosted instance
    }

    /** Our size from the hosted editor's (checks 1 and 4). */
    void fitToInner()
    {
        if (inner == nullptr || settingSize)
            return;
        const juce::ScopedValueSetter<bool> guard (settingSize, true);
        inner->setTopLeftPosition (0, barHeight);
        setSize (juce::jmax (360, inner->getWidth()), barHeight + inner->getHeight());
    }

    void componentMovedOrResized (juce::Component& c, bool, bool wasResized) override
    {
        if (&c == inner.get() && wasResized)
        {
            trace ("hosted editor resized itself to " + juce::String (c.getWidth()) + " x " + juce::String (c.getHeight()));
            fitToInner();
        }
    }

    SpikeProcessor& processor;
    juce::TextButton loadButton { "Load..." }, reloadButton { "Reload editor" };
    juce::Label info;
    std::unique_ptr<juce::AudioProcessorEditor> inner;
    juce::StringArray plugins;   // the menu's entries
    bool settingSize = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeEditor)
};

juce::AudioProcessorEditor* SpikeProcessor::createEditor()
{
    return new SpikeEditor (*this);
}

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpikeProcessor();
}
