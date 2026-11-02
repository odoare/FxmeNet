/*
  ------------------------------------------------------------------------------
    Main.cpp  (FxmeNet, Phase 1 loopback harness)

    Loads one FxmeNet-aware VST3 twice and plays both roles FxmeNet gives it,
    in one process and without a network:

      - the source: role "source", processes a test signal (two sines under
        slow, different amplitude swells) in real time on its own thread;
      - the mirror: role "mirror", prepared but never processed.

    Both editors are shown side by side. A 30 Hz timer reads a telemetry
    frame from the source and writes it into the mirror, and parameters are
    copied both ways, as the FxmeNet client and server will do over the LAN.
    The mirror's meters following the source's is Phase 1's exit criterion.
    The "Telemetry" switch stops the pump: the mirror's meters then stop too,
    which shows they are not fed by any processing of its own.

    Usage: FxmeNetLoopback [plugin.vst3]   (default ~/.vst3/FxmeNetGainPan.vst3)

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <FxmeTools/net/NetAbi.h>
#include <FxmeTools/net/TelemetryCodec.h>

#include <atomic>
#include <cmath>
#include <vector>

namespace
{

using fxme::net::INetPlugin;
using fxme::net::Role;

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;

//==============================================================================
/** Feeds the source a test signal in real time, on its own thread, as the
    server's audio worker will feed it blocks from the network. */
class SignalThread : public juce::Thread
{
public:
    explicit SignalThread (juce::AudioPluginInstance& p)
        : juce::Thread ("FxmeNet loopback signal"), plugin (p) {}

    void run() override
    {
        const int channels = juce::jmax (plugin.getTotalNumInputChannels(),
                                         plugin.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer (channels, blockSize);
        juce::MidiBuffer midi;

        const double blockMs = 1000.0 * blockSize / sampleRate;
        double due = juce::Time::getMillisecondCounterHiRes();
        std::int64_t t = 0;

        while (! threadShouldExit())
        {
            for (int i = 0; i < blockSize; ++i, ++t)
            {
                const double s = (double) t / sampleRate;
                const double swellL = 0.5 * (1.0 + std::sin (juce::MathConstants<double>::twoPi * 0.25 * s));
                const double swellR = 0.5 * (1.0 + std::sin (juce::MathConstants<double>::twoPi * 0.4 * s + 1.0));
                const auto left  = (float) (0.7 * swellL * std::sin (juce::MathConstants<double>::twoPi * 220.0 * s));
                const auto right = (float) (0.7 * swellR * std::sin (juce::MathConstants<double>::twoPi * 330.0 * s));
                for (int ch = 0; ch < channels; ++ch)
                    buffer.setSample (ch, i, ch % 2 == 0 ? left : right);
            }

            midi.clear();
            plugin.processBlock (buffer, midi);

            due += blockMs;
            const double remaining = due - juce::Time::getMillisecondCounterHiRes();
            if (remaining > 1.0)
                juce::Thread::wait (remaining);
        }
    }

private:
    juce::AudioPluginInstance& plugin;
};

//==============================================================================
/** Copies one instance's parameter changes to the other's, as the client and
    server will. A shared flag stops the echo of a copy coming back. */
class ParameterLink : private juce::AudioProcessorParameter::Listener
{
public:
    ParameterLink (juce::AudioPluginInstance& fromIn, juce::AudioPluginInstance& toIn,
                   std::atomic<bool>& copyingIn)
        : from (fromIn), to (toIn), copying (copyingIn)
    {
        for (auto* p : from.getParameters())
            p->addListener (this);
    }

    ~ParameterLink() override
    {
        for (auto* p : from.getParameters())
            p->removeListener (this);
    }

private:
    void parameterValueChanged (int index, float value) override
    {
        const auto& targets = to.getParameters();
        if (copying.load() || ! juce::isPositiveAndBelow (index, targets.size()))
            return;
        auto* target = targets[index];
        if (std::abs (target->getValue() - value) < 1.0e-6f)
            return;

        copying = true;
        target->setValueNotifyingHost (value);
        copying = false;
    }

    void parameterGestureChanged (int, bool) override {}

    juce::AudioPluginInstance& from;
    juce::AudioPluginInstance& to;
    std::atomic<bool>& copying;
};

//==============================================================================
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    explicit MainComponent (const juce::File& pluginFile)
    {
        addAndMakeVisible (status);
        status.setFont (juce::Font (juce::FontOptions (14.0f)));
        status.setColour (juce::Label::textColourId, juce::Colours::white);

        telemetryButton.setButtonText ("Telemetry");
        telemetryButton.setToggleState (true, juce::dontSendNotification);
        telemetryButton.onClick = [this] { setTelemetry (telemetryButton.getToggleState()); };
        addAndMakeVisible (telemetryButton);

        for (auto* l : { &sourceTitle, &mirrorTitle })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (*l);
        }
        sourceTitle.setText ("Source: processes the test signal (the server's role)", juce::dontSendNotification);
        mirrorTitle.setText ("Mirror: never processes, fed by telemetry (the DAW's role)", juce::dontSendNotification);

        const auto error = load (pluginFile);
        if (error.isNotEmpty())
        {
            status.setText ("Error: " + error, juce::dontSendNotification);
            setSize (900, 120);
            return;
        }

        setSize (sourceEditor->getWidth() + mirrorEditor->getWidth() + 3 * margin,
                 headerHeight + titleHeight + juce::jmax (sourceEditor->getHeight(),
                                                          mirrorEditor->getHeight()) + margin);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        if (signal != nullptr)
            signal->stopThread (2000);
        links.clear();
        sourceEditor.reset();
        mirrorEditor.reset();
        for (auto* net : { sourceNet, mirrorNet })
            if (net != nullptr)
                net->release();
        source.reset();
        mirror.reset();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff101018));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (margin, 0);
        auto header = area.removeFromTop (headerHeight);
        telemetryButton.setBounds (header.removeFromRight (110).reduced (0, 8));
        status.setBounds (header);

        if (sourceEditor == nullptr)
            return;

        auto titles = area.removeFromTop (titleHeight);
        sourceTitle.setBounds (titles.removeFromLeft (sourceEditor->getWidth()));
        titles.removeFromLeft (margin);
        mirrorTitle.setBounds (titles);

        sourceEditor->setTopLeftPosition (area.getPosition());
        mirrorEditor->setTopLeftPosition (area.getX() + sourceEditor->getWidth() + margin, area.getY());
    }

private:
    static constexpr int margin = 12, headerHeight = 44, titleHeight = 22;

    /** Loads both instances, reaches their FxmeNet interface, sets the roles
        (before prepareToPlay, as the client and server will), shows both
        editors and starts the source's signal. Returns an error, or {}. */
    juce::String load (const juce::File& pluginFile)
    {
        juce::addDefaultFormatsToManager (formats);

        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* format : formats.getFormats())
            if (format->fileMightContainThisPluginType (pluginFile.getFullPathName()))
                format->findAllTypesForFile (found, pluginFile.getFullPathName());
        if (found.isEmpty())
            return "no plugin in " + pluginFile.getFullPathName();

        juce::String error;
        source = formats.createPluginInstance (*found[0], sampleRate, blockSize, error);
        if (source != nullptr)
            mirror = formats.createPluginInstance (*found[0], sampleRate, blockSize, error);
        if (source == nullptr || mirror == nullptr)
            return "cannot instantiate: " + error;

        sourceNet = netOf (*source);
        mirrorNet = netOf (*mirror);
        if (sourceNet == nullptr || mirrorNet == nullptr)
            return found[0]->name + " is not FxmeNet-aware (no fxme::net::NetBridge answers)";

        sourceNet->setRole ((std::int32_t) Role::source);
        mirrorNet->setRole ((std::int32_t) Role::mirror);

        source->prepareToPlay (sampleRate, blockSize);
        mirror->prepareToPlay (sampleRate, blockSize);
        sourceNet->setTelemetryActive (1);

        links.push_back (std::make_unique<ParameterLink> (*mirror, *source, copying));
        links.push_back (std::make_unique<ParameterLink> (*source, *mirror, copying));

        sourceEditor.reset (source->createEditorAndMakeActive());
        mirrorEditor.reset (mirror->createEditorAndMakeActive());
        if (sourceEditor == nullptr || mirrorEditor == nullptr)
            return "the plugin has no editor";
        addAndMakeVisible (*sourceEditor);
        addAndMakeVisible (*mirrorEditor);

        description = found[0]->name + ", ABI " + juce::String ((int) sourceNet->getAbiVersion())
                    + ", channels: " + schemaNames (*sourceNet);

        signal = std::make_unique<SignalThread> (*source);
        signal->startThread (juce::Thread::Priority::high);
        return {};
    }

    static INetPlugin* netOf (juce::AudioPluginInstance& instance)
    {
        auto* vst3 = instance.getVST3Client();
        return vst3 != nullptr ? fxme::net::queryNetPlugin (vst3->getIComponentPtr()) : nullptr;
    }

    /** The channel names of the plugin's telemetry schema. */
    static juce::String schemaNames (INetPlugin& net)
    {
        std::vector<std::uint8_t> bytes (4096);
        const int n = net.getTelemetrySchema (bytes.data(), (std::int32_t) bytes.size());
        if (n <= 0)
            return "(schema unavailable)";

        fxme::net::ByteReader r (bytes.data(), (std::size_t) n);
        r.u32();
        r.u16();
        juce::StringArray names;
        for (int i = 0, count = r.u16(); i < count && r.ok(); ++i)
        {
            r.u32();
            r.u8();
            const auto len = r.u8();
            if (const auto* name = r.take (len))
                names.add (juce::String::fromUTF8 ((const char*) name, len));
        }
        return names.joinIntoString (" ");
    }

    void setTelemetry (bool on)
    {
        pumping = on;
        sourceNet->setTelemetryActive (on ? 1 : 0);
    }

    /** The pump: one frame from the source into the mirror. */
    void timerCallback() override
    {
        if (pumping)
        {
            int n = sourceNet->readTelemetry (frame.data(), (std::int32_t) frame.size());
            if (n < 0)
            {
                frame.resize ((size_t) -n);
                n = sourceNet->readTelemetry (frame.data(), (std::int32_t) frame.size());
            }
            if (n > 0)
            {
                mirrorNet->writeTelemetry (frame.data(), n);
                ++frames;
                lastFrameBytes = n;
            }
        }

        if (++statusTick % 15 == 0)
            status.setText (description + "  |  " + juce::String (frames) + " frames, "
                              + juce::String (lastFrameBytes) + " bytes each"
                              + (pumping ? "" : "  (telemetry off)"),
                            juce::dontSendNotification);
    }

    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> source, mirror;
    INetPlugin* sourceNet = nullptr;
    INetPlugin* mirrorNet = nullptr;
    std::unique_ptr<juce::AudioProcessorEditor> sourceEditor, mirrorEditor;
    std::unique_ptr<SignalThread> signal;

    std::atomic<bool> copying { false };
    std::vector<std::unique_ptr<ParameterLink>> links;

    std::vector<std::uint8_t> frame = std::vector<std::uint8_t> (4096);
    bool pumping = true;
    int frames = 0, lastFrameBytes = 0, statusTick = 0;
    juce::String description;

    juce::Label status, sourceTitle, mirrorTitle;
    juce::ToggleButton telemetryButton;
};

//==============================================================================
class LoopbackApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        const auto plugin = args.isEmpty()
            ? juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".vst3/FxmeNetGainPan.vst3")
            : juce::File::getCurrentWorkingDirectory().getChildFile (args[0]);

        window = std::make_unique<Window> (getApplicationName(), plugin);
    }

    void shutdown() override                { window.reset(); }
    void systemRequestedQuit() override     { quit(); }

private:
    class Window : public juce::DocumentWindow
    {
    public:
        Window (const juce::String& name, const juce::File& plugin)
            : DocumentWindow (name, juce::Colour (0xff101018), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (plugin), true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<Window> window;
};

} // namespace

START_JUCE_APPLICATION (LoopbackApplication)
