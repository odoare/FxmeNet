/*
  ------------------------------------------------------------------------------
    Main.cpp  (FxmeNet, Phase 0 spike 1)

    Loads a VST3 through JUCE 9's GUI-free hosting module, runs a WAV file
    through it block by block, and round-trips its state, the way
    fxmenet-server will. Checks, in order:

      1. The plugin binary loads with no display (run it with DISPLAY and
         WAYLAND_DISPLAY unset), and which GUI libraries it pulled in anyway
         (Linux: read from /proc/self/maps).
      2. The instance reports what a tier 0 client needs: parameters with
         names and value texts, latency, bus layout, the VST3 extension
         pointer FxmeNet will use for its side channel.
      3. Processing works and how fast it runs compared with real time.
      4. State saved from one instance and loaded into a fresh one gives the
         same output (max abs difference; 0 expected for deterministic DSP).
         Faster-than-real-time processing makes any DSP that depends on a
         background thread timing-dependent, as in a DAW's offline bounce.

    Usage:
      FxmeNetSpikeHeadless <plugin.vst3> <in.wav> <out.wav> [blockSize] [settleMs]

    settleMs (default 1000): after prepareToPlay, the JUCE message loop runs
    for this long before processing starts, so the plugin's timers fire and
    its background work can finish (AmbiRR2 pushes parameters to its engine
    from a 30 Hz timer and computes image sources on a worker thread). The
    server will run that loop permanently; 0 reproduces a host that never
    dispatches messages.

    Writes <out.wav> (32-bit float) and <out.wav>.state (the plugin state).
    Throwaway code: it proves a point and then goes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace
{

void say (const juce::String& s)    { std::cout << s << std::endl; }
void fail (const juce::String& s)   { std::cerr << "FAIL: " << s << std::endl; }

/** The GUI/display libraries currently mapped into this process (Linux). */
juce::StringArray mappedGuiLibraries()
{
    juce::StringArray found;
   #if JUCE_LINUX || JUCE_BSD
    juce::StringArray lines;
    lines.addLines (juce::File ("/proc/self/maps").loadFileAsString());

    for (auto& line : lines)
    {
        const auto path = line.fromFirstOccurrenceOf ("/", true, false).trim();
        const auto name = juce::File (path).getFileName();

        for (auto* lib : { "libX11", "libxcb", "libXext", "libXrandr", "libXcursor",
                           "libGL", "libEGL", "libwayland", "libfreetype", "libfontconfig",
                           "libgtk", "libwebkit" })
            if (name.startsWith (lib))
                found.addIfNotAlreadyThere (name);
    }
   #endif
    return found;
}

void reportGuiLibraries (const char* when)
{
   #if JUCE_LINUX || JUCE_BSD
    const auto libs = mappedGuiLibraries();
    say (juce::String ("GUI libraries mapped ") + when + ": "
         + (libs.isEmpty() ? juce::String ("none") : libs.joinIntoString (", ")));
   #else
    juce::ignoreUnused (when);
   #endif
}

std::unique_ptr<juce::AudioBuffer<float>> readWav (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return {};

    sampleRate = reader->sampleRate;
    auto buffer = std::make_unique<juce::AudioBuffer<float>> ((int) reader->numChannels,
                                                              (int) reader->lengthInSamples);
    reader->read (buffer.get(), 0, buffer->getNumSamples(), 0, true, true);
    return buffer;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (buffer.getNumChannels())
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

/** Runs `input` through `plugin` in blocks of `blockSize`, as the server
    will. Input channels beyond the file's repeat its last channel (a mono
    file feeds both inputs of a stereo plugin). Returns the output channels. */
juce::AudioBuffer<float> processFile (juce::AudioPluginInstance& plugin,
                                      const juce::AudioBuffer<float>& input,
                                      double sampleRate, int blockSize,
                                      int settleMs, double& elapsedMs)
{
    const int numIns  = plugin.getTotalNumInputChannels();
    const int numOuts = plugin.getTotalNumOutputChannels();
    const int numSamples = input.getNumSamples();

    juce::AudioBuffer<float> output (numOuts, numSamples);
    juce::AudioBuffer<float> block (juce::jmax (numIns, numOuts), blockSize);
    juce::MidiBuffer midi;

    plugin.setNonRealtime (false);
    plugin.prepareToPlay (sampleRate, blockSize);

    if (settleMs > 0)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (settleMs);

    const auto start = juce::Time::getMillisecondCounterHiRes();

    for (int pos = 0; pos < numSamples; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, numSamples - pos);
        block.setSize (block.getNumChannels(), n, false, false, true);
        block.clear();

        for (int ch = 0; ch < numIns && input.getNumChannels() > 0; ++ch)
            block.copyFrom (ch, 0, input, juce::jmin (ch, input.getNumChannels() - 1), pos, n);

        midi.clear();
        plugin.processBlock (block, midi);

        for (int ch = 0; ch < numOuts; ++ch)
            output.copyFrom (ch, pos, block, ch, 0, n);
    }

    elapsedMs = juce::Time::getMillisecondCounterHiRes() - start;
    plugin.releaseResources();
    return output;
}

float maxAbsDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return std::numeric_limits<float>::infinity();

    float diff = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            diff = juce::jmax (diff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    return diff;
}

void reportInstance (juce::AudioPluginInstance& plugin)
{
    const auto desc = plugin.getPluginDescription();
    say ("Plugin: " + desc.name + " by " + desc.manufacturerName + ", version " + desc.version
         + ", format " + desc.pluginFormatName);
    say ("  identifier: " + desc.createIdentifierString());
    say ("  channels in/out: " + juce::String (plugin.getTotalNumInputChannels()) + "/"
         + juce::String (plugin.getTotalNumOutputChannels())
         + ", latency " + juce::String (plugin.getLatencySamples()) + " samples"
         + ", tail " + juce::String (plugin.getTailLengthSeconds()) + " s");
    say (juce::String ("  hasEditor(): ") + (plugin.hasEditor() ? "true" : "false")
         + " (headless hosting should say false)");
    say (juce::String ("  VST3 extensions (IComponent* for the FxmeNet side channel): ")
         + ((plugin.getVST3Client() != nullptr
             && plugin.getVST3Client()->getIComponentPtr() != nullptr) ? "available" : "MISSING"));

    const auto& params = plugin.getParameters();
    say ("  parameters: " + juce::String (params.size()));

    constexpr int maxListed = 40;
    for (int i = 0; i < juce::jmin (maxListed, params.size()); ++i)
    {
        auto* p = params[i];
        say ("    [" + juce::String (i) + "] " + p->getName (64) + " = "
             + p->getText (p->getValue(), 32) + " " + p->getLabel()
             + (p->isAutomatable() ? "" : "  (not automatable)"));
    }
    if (params.size() > maxListed)
        say ("    ... " + juce::String (params.size() - maxListed) + " more");
}

int run (const juce::StringArray& args)
{
    if (args.size() < 3)
    {
        say ("usage: FxmeNetSpikeHeadless <plugin.vst3> <in.wav> <out.wav> [blockSize] [settleMs]");
        return 2;
    }

    const juce::File pluginFile (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]));
    const juce::File inFile     (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
    const juce::File outFile    (juce::File::getCurrentWorkingDirectory().getChildFile (args[2]));
    const int blockSize = args.size() > 3 ? juce::jmax (1, args[3].getIntValue()) : 128;
    const int settleMs  = args.size() > 4 ? juce::jmax (0, args[4].getIntValue()) : 1000;

    const auto* display = std::getenv ("DISPLAY");
    const auto* wayland = std::getenv ("WAYLAND_DISPLAY");
    say (juce::String ("DISPLAY=") + (display != nullptr ? display : "(unset)")
         + ", WAYLAND_DISPLAY=" + (wayland != nullptr ? wayland : "(unset)"));
    reportGuiLibraries ("at start");

    double sampleRate = 0.0;
    auto input = readWav (inFile, sampleRate);
    if (input == nullptr)
    {
        fail ("cannot read " + inFile.getFullPathName());
        return 1;
    }
    say ("Input: " + juce::String (input->getNumChannels()) + " ch, "
         + juce::String (input->getNumSamples()) + " samples at " + juce::String (sampleRate) + " Hz"
         + ", block size " + juce::String (blockSize)
         + ", settle " + juce::String (settleMs) + " ms");

    // 1. Load.
    juce::AudioPluginFormatManager formatManager;
    juce::addHeadlessDefaultFormatsToManager (formatManager);

    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType (pluginFile.getFullPathName()))
            format->findAllTypesForFile (descriptions, pluginFile.getFullPathName());

    if (descriptions.isEmpty())
    {
        fail ("no plugin found in " + pluginFile.getFullPathName());
        return 1;
    }

    juce::String error;
    auto plugin = formatManager.createPluginInstance (*descriptions[0], sampleRate, blockSize, error);
    if (plugin == nullptr)
    {
        fail ("cannot instantiate: " + error);
        return 1;
    }
    reportGuiLibraries ("after loading the plugin");

    // 2. What a tier 0 client would be told.
    reportInstance (*plugin);

    // 3. Process.
    double elapsedMs = 0.0;
    const auto output = processFile (*plugin, *input, sampleRate, blockSize, settleMs, elapsedMs);
    const double audioMs = 1000.0 * input->getNumSamples() / sampleRate;
    say ("Processed " + juce::String (audioMs, 1) + " ms of audio in " + juce::String (elapsedMs, 1)
         + " ms (" + juce::String (audioMs / juce::jmax (elapsedMs, 1.0e-3), 1) + "x real time)");
    reportGuiLibraries ("after processing");

    if (! writeWav (outFile, output, sampleRate))
    {
        fail ("cannot write " + outFile.getFullPathName());
        return 1;
    }
    say ("Wrote " + outFile.getFullPathName());

    // 4. State round trip into a fresh instance.
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    const auto stateFile = outFile.getSiblingFile (outFile.getFileName() + ".state");
    stateFile.replaceWithData (state.getData(), state.getSize());
    say ("State: " + juce::String ((int) state.getSize()) + " bytes, saved to " + stateFile.getFileName());

    auto replica = formatManager.createPluginInstance (*descriptions[0], sampleRate, blockSize, error);
    if (replica == nullptr)
    {
        fail ("cannot instantiate a second time: " + error);
        return 1;
    }
    replica->setStateInformation (state.getData(), (int) state.getSize());

    double replicaMs = 0.0;
    const auto replicaOutput = processFile (*replica, *input, sampleRate, blockSize, settleMs, replicaMs);
    const float diff = maxAbsDifference (output, replicaOutput);
    say ("Fresh instance with the saved state: max abs difference "
         + juce::String (diff, 9)
         + (juce::exactlyEqual (diff, 0.0f) ? " (bit-identical)" : " (not identical: random DSP, timing-dependent background work, or state not fully restored)"));

    replica.reset();
    plugin.reset();
    say ("OK");
    return 0;
}

} // namespace

int main (int argc, char* argv[])
{
    // The main thread becomes the message thread: VST3 instances are created
    // and their state is set on it, as on the server's control thread.
    juce::MessageManager::getInstance();

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::CharPointer_UTF8 (argv[i]));

    const int result = run (args);

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return result;
}
