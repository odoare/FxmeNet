/*
  ------------------------------------------------------------------------------
    Main.cpp  (fxmenet-wavclient, Phase 2 end-to-end test)

    Streams a WAV file through a chain of plugins on an fxmenet-server, the
    way FxmeNetPlugin will: HELLO, CONFIGURE, one SLOT_ADD per plugin, then
    one audio request per block over UDP. Writes the result, then processes
    the same file locally through the same plugins in the same blocks, and
    compares (bit-identical expected for deterministic plugins).

    Two modes:
      - offline (default): each block waits for its reply, retrying every
        50 ms, as a DAW's offline render will. Nothing may be lost.
      - --realtime: blocks leave at the audio rate whether or not replies
        came back, and a reply later than the latency budget (--budget
        blocks, default 2) counts as late, as it would be in a live DAW.
        Reports round-trip times, late and lost blocks.

    The plugins are given by path: the client reads their identity (uid,
    name, vendor) from its own copy, which also makes the local reference.
    The server must have the same plugins.

    Usage:
      fxmenet-wavclient --in in.wav --out out.wav --plugin a.vst3 [--plugin b.vst3]...
                        [--server host[:port]] [--block N] [--realtime] [--budget D]
                        [--settle ms] [--no-compare]

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/control/ControlConnection.h>
#include <FxmeNet/transport/UdpTransport.h>

#include <iostream>

namespace
{

using namespace fxmenet;
using control::ControlConnection;
using control::Frame;
using control::Type;

void say (const juce::String& s)    { std::cout << s << std::endl; }
void fail (const juce::String& s)   { std::cerr << "FAIL: " << s << std::endl; }

//==============================================================================
struct Options
{
    juce::String host = "127.0.0.1";
    int port = 54322;
    juce::File in, out;
    juce::StringArray plugins;
    int blockSize = 128;
    bool realtime = false;
    int budgetBlocks = 2;
    int settleMs = 1000;
    bool compare = true;

    bool parse (const juce::StringArray& args, juce::String& error)
    {
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        for (int i = 0; i < args.size(); ++i)
        {
            const auto& a = args[i];
            const bool hasValue = i + 1 < args.size();
            if (a == "--server" && hasValue)
            {
                const auto s = args[++i];
                host = s.upToFirstOccurrenceOf (":", false, false);
                if (s.contains (":"))
                    port = s.fromFirstOccurrenceOf (":", false, false).getIntValue();
            }
            else if (a == "--in" && hasValue)        in = cwd.getChildFile (args[++i]);
            else if (a == "--out" && hasValue)       out = cwd.getChildFile (args[++i]);
            else if (a == "--plugin" && hasValue)    plugins.add (cwd.getChildFile (args[++i]).getFullPathName());
            else if (a == "--block" && hasValue)     blockSize = args[++i].getIntValue();
            else if (a == "--budget" && hasValue)    budgetBlocks = args[++i].getIntValue();
            else if (a == "--settle" && hasValue)    settleMs = args[++i].getIntValue();
            else if (a == "--realtime")              realtime = true;
            else if (a == "--no-compare")            compare = false;
            else
            {
                error = "unknown or incomplete argument: " + a;
                return false;
            }
        }
        if (in == juce::File() || out == juce::File() || plugins.isEmpty())
            error = "--in, --out and at least one --plugin are required";
        else if (blockSize < 16 || blockSize > 4096)
            error = "--block must be between 16 and 4096";
        return error.isEmpty();
    }
};

/** What the server sees when the client sends no transport: an empty
    position. The local reference gets the same, so both run alike. */
struct EmptyPlayHead : public juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override   { return PositionInfo{}; }
};

//==============================================================================
std::unique_ptr<juce::AudioBuffer<float>> readWav (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return {};
    sampleRate = reader->sampleRate;
    auto buffer = std::make_unique<juce::AudioBuffer<float>> ((int) reader->numChannels, (int) reader->lengthInSamples);
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

//==============================================================================
/** Sends a request and waits for its answer (same requestId); prints an
    ERROR. Frames that answer something else are skipped. */
bool request (ControlConnection& c, Type type, std::uint32_t id, std::uint16_t flags,
              const std::vector<std::uint8_t>& payload, Type expected, Frame& reply, int timeoutMs = 10000)
{
    if (! c.send (type, flags, id, payload.data(), payload.size()))
    {
        fail ("control connection lost while sending");
        return false;
    }

    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        const auto r = c.read (reply, 200);
        if (r == ControlConnection::ReadResult::closed)
        {
            fail ("control connection closed by the server");
            return false;
        }
        if (r != ControlConnection::ReadResult::frame || reply.requestId != id)
            continue;
        if (reply.type == Type::error)
        {
            fail ("server: " + control::ErrorMessage::fromVar (control::fromPayload (reply.payload)).text);
            return false;
        }
        if (reply.type == expected)
            return true;
    }
    fail ("no answer from the server");
    return false;
}

bool requestJson (ControlConnection& c, Type type, std::uint32_t id, const juce::var& v, Type expected, Frame& reply)
{
    return request (c, type, id, control::flagJson, control::toPayload (v), expected, reply);
}

//==============================================================================
/** RTT statistics in milliseconds. */
struct Rtt
{
    double minMs = 1.0e9, maxMs = 0.0, sumMs = 0.0;
    int count = 0;

    void add (double ms)
    {
        minMs = juce::jmin (minMs, ms);
        maxMs = juce::jmax (maxMs, ms);
        sumMs += ms;
        ++count;
    }

    juce::String describe() const
    {
        if (count == 0)
            return "no replies";
        return "round trip min " + juce::String (minMs, 3) + " ms, mean " + juce::String (sumMs / count, 3)
             + " ms, max " + juce::String (maxMs, 3) + " ms";
    }
};

class Streamer
{
public:
    Streamer (UdpTransport& t, const juce::AudioBuffer<float>& source, int numBlocks, int blockSizeIn,
              juce::AudioBuffer<float>& destination)
        : transport (t), input (source), blocks (numBlocks), n (blockSizeIn), output (destination),
          channels (source.getNumChannels())
    {
        scratch.assign ((std::size_t) channels, std::vector<float> ((std::size_t) n));
        for (auto& ch : scratch)
            pointers.push_back (ch.data());
        body.reserve (1 << 18);
        sample.resize ((std::size_t) n);
        sentAt.assign ((std::size_t) numBlocks, 0.0);
        received.assign ((std::size_t) numBlocks, false);
    }

    Rtt rtt;
    int retries = 0, late = 0, chainLatency = 0;
    double processMicrosSum = 0.0;

    /** Each block waits for its reply. False when one never comes. */
    bool runOffline()
    {
        for (int k = 0; k < blocks; ++k)
        {
            send (k, wire::requestFlags::offline);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            while (! received[(std::size_t) k])
            {
                if (! receiveOne (50))
                {
                    if (juce::Time::getMillisecondCounterHiRes() - start > 5000.0)
                    {
                        fail ("block " + juce::String (k) + ": no reply in 5 s");
                        return false;
                    }
                    send (k, wire::requestFlags::offline | wire::requestFlags::retry);
                    ++retries;
                }
            }
        }
        return true;
    }

    /** Blocks leave at the audio rate; replies later than the budget are late. */
    void runRealtime (double sampleRate, int budgetBlocks)
    {
        const double blockMs = 1000.0 * n / sampleRate;
        budgetMs = budgetBlocks * blockMs;
        const double start = juce::Time::getMillisecondCounterHiRes();
        int sent = 0;

        for (;;)
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (sent < blocks && now >= start + sent * blockMs)
            {
                send (sent++, 0);
                continue;
            }
            if (sent == blocks && (receivedCount == blocks || now > start + sent * blockMs + 500.0))
                break;

            const double due = sent < blocks ? start + sent * blockMs : now + 10.0;
            receiveOne (juce::jmax (0, (int) (due - now)));
        }
    }

    int getReceivedCount() const noexcept   { return receivedCount; }

private:
    void send (int k, std::uint16_t flags)
    {
        const int offset = k * n;
        for (int c = 0; c < channels; ++c)
        {
            auto& ch = scratch[(std::size_t) c];
            std::fill (ch.begin(), ch.end(), 0.0f);
            const int available = juce::jmin (n, input.getNumSamples() - offset);
            if (available > 0)
                std::copy (input.getReadPointer (c, offset), input.getReadPointer (c, offset) + available, ch.begin());
        }

        wire::BlockInfo info;
        info.block = (std::uint64_t) k;
        info.samples = (std::uint16_t) n;
        info.mask = wire::maskOfFirst (channels);
        body.clear();
        wire::ByteWriter w (body);
        wire::writeBlock (w, false, info, nullptr, 0, pointers.data());

        if ((flags & wire::requestFlags::retry) == 0)
            sentAt[(std::size_t) k] = juce::Time::getMillisecondCounterHiRes();
        transport.send (wire::Kind::audioRequest, flags, body.data(), body.size());
    }

    bool receiveOne (int timeoutMs)
    {
        ReceivedMessage m;
        if (! transport.receive (m, timeoutMs))
            return false;

        wire::BlockView v;
        if (m.kind != wire::Kind::audioReply || ! wire::readBlock (m.body, m.size, true, m.flags, v))
            return true;
        const auto k = (int) v.info.block;
        if (! juce::isPositiveAndBelow (k, blocks) || received[(std::size_t) k])
            return true;   // a reply to a retry we no longer need

        received[(std::size_t) k] = true;
        ++receivedCount;
        const double ms = juce::Time::getMillisecondCounterHiRes() - sentAt[(std::size_t) k];
        rtt.add (ms);
        if (budgetMs > 0.0 && ms > budgetMs)
            ++late;
        chainLatency = (int) v.info.chainLatency;
        processMicrosSum += v.info.processMicros;

        for (int c = 0; c < juce::jmin (channels, v.numChannels()); ++c)
        {
            v.copyChannel (c, sample.data());
            const int offset = k * n;
            const int count = juce::jmin (n, output.getNumSamples() - offset);
            if (count > 0)
                output.copyFrom (c, offset, sample.data(), count);
        }
        return true;
    }

    UdpTransport& transport;
    const juce::AudioBuffer<float>& input;
    const int blocks, n;
    juce::AudioBuffer<float>& output;
    const int channels;

    std::vector<std::vector<float>> scratch;
    std::vector<const float*> pointers;
    std::vector<std::uint8_t> body;
    std::vector<float> sample;
    std::vector<double> sentAt;
    std::vector<bool> received;
    int receivedCount = 0;
    double budgetMs = 0.0;
};

//==============================================================================
/** The same chain processed locally, in the same blocks. */
bool processLocally (const juce::OwnedArray<juce::PluginDescription>& descriptions,
                     juce::AudioPluginFormatManager& formats, const juce::AudioBuffer<float>& input,
                     double sampleRate, int n, int settleMs, juce::AudioBuffer<float>& output)
{
    EmptyPlayHead playHead;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> chain;
    int channels = input.getNumChannels();

    for (auto* d : descriptions)
    {
        juce::String error;
        auto instance = formats.createPluginInstance (*d, sampleRate, n, error);
        if (instance == nullptr)
        {
            fail ("local " + d->name + ": " + error);
            return false;
        }
        instance->enableAllBuses();
        instance->setPlayHead (&playHead);
        instance->prepareToPlay (sampleRate, n);
        channels = juce::jmax (channels, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
        chain.push_back (std::move (instance));
    }

    if (settleMs > 0)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (settleMs);

    juce::AudioBuffer<float> buffer (channels, n);
    juce::MidiBuffer midi;
    for (int offset = 0; offset < output.getNumSamples(); offset += n)
    {
        buffer.clear();
        const int count = juce::jmin (n, input.getNumSamples() - offset);
        for (int c = 0; c < input.getNumChannels() && count > 0; ++c)
            buffer.copyFrom (c, 0, input, c, offset, count);

        for (auto& instance : chain)
        {
            const int k = juce::jmax (instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), k, n);
            midi.clear();
            instance->processBlock (view, midi);
        }

        const int keep = juce::jmin (n, output.getNumSamples() - offset);
        for (int c = 0; c < output.getNumChannels(); ++c)
            output.copyFrom (c, offset, buffer, c, 0, keep);
    }

    for (auto& instance : chain)
        instance->releaseResources();
    return true;
}

//==============================================================================
int run (const Options& o)
{
    double sampleRate = 0.0;
    auto input = readWav (o.in, sampleRate);
    if (input == nullptr)
    {
        fail ("cannot read " + o.in.getFullPathName());
        return 1;
    }
    const int channels = input->getNumChannels();
    const int numBlocks = (input->getNumSamples() + o.blockSize - 1) / o.blockSize;
    say ("Input: " + juce::String (channels) + " ch, " + juce::String (input->getNumSamples()) + " samples at "
         + juce::String (sampleRate) + " Hz, " + juce::String (numBlocks) + " blocks of " + juce::String (o.blockSize));

    // The chain's identity, from the local copies of the plugins.
    juce::AudioPluginFormatManager formats;
    juce::addHeadlessDefaultFormatsToManager (formats);
    juce::OwnedArray<juce::PluginDescription> descriptions;
    int chainChannels = channels;
    for (auto& path : o.plugins)
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : formats.getFormats())
            if (f->fileMightContainThisPluginType (path))
                f->findAllTypesForFile (found, path);
        if (found.isEmpty())
        {
            fail ("no plugin in " + path);
            return 1;
        }
        chainChannels = juce::jmax (chainChannels, found[0]->numInputChannels, found[0]->numOutputChannels);
        descriptions.add (new juce::PluginDescription (*found[0]));
    }

    // Control: HELLO, CONFIGURE, the slots.
    auto connection = ControlConnection::connect (o.host, o.port, 3000);
    if (connection == nullptr)
    {
        fail ("cannot connect to " + o.host + ":" + juce::String (o.port));
        return 1;
    }

    std::uint32_t id = 1;
    Frame reply;
    control::Hello hello;
    hello.clientName = "fxmenet-wavclient";
    hello.os = juce::SystemStats::getOperatingSystemName();
    if (! requestJson (*connection, Type::hello, id++, hello.toVar(), Type::welcome, reply))
        return 1;
    const auto welcome = control::Welcome::fromVar (control::fromPayload (reply.payload));
    if (welcome.error.isNotEmpty())
    {
        fail ("refused: " + welcome.error);
        return 1;
    }
    say ("Session " + juce::String ((int) welcome.sessionId) + " on \"" + welcome.serverName + "\" ("
         + welcome.os + "), audio on UDP " + juce::String (welcome.udpPort));

    control::Configure config;
    config.sampleRate = sampleRate;
    config.blockSize = o.blockSize;
    config.numInputs = config.numOutputs = chainChannels;
    config.inputMask = config.outputMask = wire::maskOfFirst (channels);
    if (! requestJson (*connection, Type::configure, id++, config.toVar(), Type::configure, reply))
        return 1;

    for (int i = 0; i < descriptions.size(); ++i)
    {
        const auto* d = descriptions[i];
        control::SlotAdd add;
        add.slotId = (std::uint32_t) i + 1;
        add.position = (std::uint32_t) i;
        add.uid = (std::uint32_t) d->uniqueId;
        add.name = d->name.toStdString();
        add.vendor = d->manufacturerName.toStdString();
        if (! request (*connection, Type::slotAdd, id++, 0, encode (add), Type::slotReady, reply, 30000))
            return 1;
        const auto ready = control::SlotReady::fromVar (control::fromPayload (reply.payload));
        say ("  slot " + juce::String ((int) ready.slotId) + ": " + d->name + ", "
             + juce::String ((int) ready.params.size()) + " parameters, latency " + juce::String (ready.latency)
             + (ready.fxmenetAbi > 0 ? ", FxmeNet-aware (ABI " + juce::String (ready.fxmenetAbi) + ")" : ""));
    }

    if (o.settleMs > 0)
        juce::Thread::sleep (o.settleMs);   // the server's plugins settle on its message loop

    // Audio.
    juce::DatagramSocket socket;
    if (! socket.bindToPort (0))
    {
        fail ("cannot open a UDP socket");
        return 1;
    }
    UdpTransport::Settings settings;
    settings.sessionId = welcome.sessionId;
    settings.token = welcome.token;
    settings.datagramBudget = (std::size_t) config.datagramBudget;
    UdpTransport transport (socket, o.host, welcome.udpPort, settings);

    juce::AudioBuffer<float> remote (channels, input->getNumSamples());
    remote.clear();
    Streamer streamer (transport, *input, numBlocks, o.blockSize, remote);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    bool complete = true;
    if (o.realtime)
        streamer.runRealtime (sampleRate, o.budgetBlocks);
    else
        complete = streamer.runOffline();
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;

    connection->sendEmpty (Type::bye);

    const double audioMs = 1000.0 * input->getNumSamples() / sampleRate;
    say ("Streamed " + juce::String (audioMs, 1) + " ms of audio in " + juce::String (elapsed, 1) + " ms ("
         + juce::String (o.realtime ? "real time" : "offline") + ")");
    say ("  " + juce::String (streamer.getReceivedCount()) + "/" + juce::String (numBlocks) + " blocks back, "
         + streamer.rtt.describe());
    if (o.realtime)
        say ("  later than the " + juce::String (o.budgetBlocks) + "-block budget: " + juce::String (streamer.late)
             + ", lost: " + juce::String (numBlocks - streamer.getReceivedCount()));
    else
        say ("  retries: " + juce::String (streamer.retries));
    say ("  chain latency " + juce::String (streamer.chainLatency) + " samples, server processing "
         + juce::String (streamer.getReceivedCount() > 0 ? streamer.processMicrosSum / streamer.getReceivedCount() : 0.0, 1)
         + " us per block on average");

    if (! writeWav (o.out, remote, sampleRate))
    {
        fail ("cannot write " + o.out.getFullPathName());
        return 1;
    }
    say ("Wrote " + o.out.getFullPathName());

    if (! complete)
        return 1;
    if (! o.compare)
        return 0;

    // The same chain, locally.
    juce::AudioBuffer<float> local (channels, input->getNumSamples());
    if (! processLocally (descriptions, formats, *input, sampleRate, o.blockSize, o.settleMs, local))
        return 1;

    float diff = 0.0f;
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < local.getNumSamples(); ++i)
            diff = juce::jmax (diff, std::abs (local.getSample (c, i) - remote.getSample (c, i)));

    if (juce::exactlyEqual (diff, 0.0f))
    {
        say ("Server and local processing: bit-identical");
        return 0;
    }

    const auto localFile = o.out.getSiblingFile (o.out.getFileNameWithoutExtension() + ".local.wav");
    writeWav (localFile, local, sampleRate);
    say ("Server and local processing differ: max abs difference " + juce::String (diff, 9)
         + " (local result in " + localFile.getFileName() + ")"
         + (o.realtime ? "; expected in real time when blocks were late or lost" : ""));
    return o.realtime ? 0 : 1;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::MessageManager::getInstance();

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::CharPointer_UTF8 (argv[i]));

    Options options;
    juce::String error;
    int result = 2;
    if (options.parse (args, error))
        result = run (options);
    else
        std::cerr << error << "\nusage: fxmenet-wavclient --in in.wav --out out.wav --plugin a.vst3 [--plugin b.vst3]...\n"
                     "                         [--server host[:port]] [--block N] [--realtime] [--budget D]\n"
                     "                         [--settle ms] [--no-compare]" << std::endl;

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return result;
}
