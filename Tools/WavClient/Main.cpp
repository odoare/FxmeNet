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

    --engine runs the same test through the client engine FxmeNetPlugin
    uses (Common/FxmeNet/client/ClientEngine.h) instead of talking to the
    server directly: the WAV is fed in host buffers of --host-block samples
    (or random sizes up to that, --random-blocks), as a DAW would, offline
    or paced in real time; the output, realigned by the latency the engine
    reports, is compared with local processing the same way.

    The plugins are given by path: the client reads their identity (uid,
    name, vendor) from its own copy, which also makes the local reference.
    The server must have the same plugins.

    Usage:
      fxmenet-wavclient --in in.wav --out out.wav --plugin a.vst3 [--plugin b.vst3]...
                        [--server host[:port]] [--block N] [--realtime] [--budget D]
                        [--settle ms] [--no-compare] [--telemetry]
                        [--jsfx EFFECTS_FOLDER SCRIPT_PATH]...
                        [--engine [--host-block N] [--random-blocks]]
      fxmenet-wavclient --discover

    --telemetry switches telemetry on for every FxmeNet-aware slot (as an
    open editor would), counts the frames received while streaming and
    decodes the last one with the slot's schema. --discover broadcasts
    FXMENET_DISCOVER and lists the servers that answer.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/catalog/Buses.h>
#include <FxmeNet/client/ClientEngine.h>
#include <FxmeNet/control/ControlConnection.h>
#include <FxmeNet/control/Discovery.h>
#include <FxmeNet/jsfx/JsfxScripts.h>
#include <FxmeNet/transport/UdpTransport.h>

#include <iostream>
#include <map>
#include <random>

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
    juce::StringArray jsfxFolders, jsfxPaths;   ///< --jsfx: scripts, after the plugins, on the server only
    int blockSize = 128;
    bool realtime = false;
    int budgetBlocks = 2;
    int settleMs = 1000;
    bool compare = true;
    bool telemetry = false;
    bool discover = false;
    bool engine = false;
    int hostBlock = 512;
    bool randomBlocks = false;

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
            else if (a == "--jsfx" && i + 2 < args.size())
            {
                jsfxFolders.add (cwd.getChildFile (args[++i]).getFullPathName());
                jsfxPaths.add (args[++i]);
                compare = false;   // no local copy of a script to compare with
            }
            else if (a == "--block" && hasValue)     blockSize = args[++i].getIntValue();
            else if (a == "--budget" && hasValue)    budgetBlocks = args[++i].getIntValue();
            else if (a == "--settle" && hasValue)    settleMs = args[++i].getIntValue();
            else if (a == "--realtime")              realtime = true;
            else if (a == "--no-compare")            compare = false;
            else if (a == "--telemetry")             telemetry = true;
            else if (a == "--discover")              discover = true;
            else if (a == "--engine")                engine = true;
            else if (a == "--host-block" && hasValue) hostBlock = args[++i].getIntValue();
            else if (a == "--random-blocks")         randomBlocks = true;
            else
            {
                error = "unknown or incomplete argument: " + a;
                return false;
            }
        }
        if (discover)
            return true;
        if (in == juce::File() || out == juce::File() || (plugins.isEmpty() && jsfxPaths.isEmpty()))
            error = "--in, --out and at least one --plugin or --jsfx are required";
        else if (blockSize < 16 || blockSize > 4096)
            error = "--block must be between 16 and 4096";
        else if (hostBlock < 1 || hostBlock > 8192)
            error = "--host-block must be between 1 and 8192";
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
//==============================================================================
/** What arrived on a slot's telemetry, and the names to decode it with. */
struct SlotTelemetry
{
    std::map<std::uint32_t, juce::String> names;   ///< channel id -> name, from the schema
    int frames = 0;
    std::size_t bytes = 0;
    std::vector<std::uint8_t> last;

    void readSchema (const juce::MemoryBlock& schema)
    {
        wire::ByteReader r ((const std::uint8_t*) schema.getData(), schema.getSize());
        if (r.u32() != fxme::net::schemaMagic || r.u16() != fxme::net::codecVersion)
            return;
        for (int i = 0, count = r.u16(); i < count && r.ok(); ++i)
        {
            const auto id = r.u32();
            r.u8();
            const auto len = r.u8();
            if (const auto* name = r.take (len))
                names[id] = juce::String::fromUTF8 ((const char*) name, len);
        }
    }

    /** The last frame, channel by channel. */
    juce::String describeLast() const
    {
        wire::ByteReader r (last.data(), last.size());
        if (r.u32() != fxme::net::frameMagic || r.u16() != fxme::net::codecVersion)
            return "(no frame)";

        juce::StringArray parts;
        for (int i = 0, count = r.u16(); i < count && r.ok(); ++i)
        {
            const auto id = r.u32();
            const auto kind = (fxme::net::ChannelKind) r.u8();
            r.skip (3);
            const auto size = r.u32();
            const auto* payload = r.take (size);
            if (payload == nullptr)
                break;

            const auto it = names.find (id);
            const auto name = it != names.end() ? it->second : juce::String::toHexString ((int) id);
            wire::ByteReader p (payload, size);
            auto db = [] (float g) { return juce::String (juce::Decibels::gainToDecibels (g), 1) + " dB"; };

            switch (kind)
            {
                case fxme::net::ChannelKind::meter:
                {
                    const float rms = p.f32(), peak = p.f32();
                    parts.add (name + " rms " + db (rms) + " peak " + db (peak));
                    break;
                }
                case fxme::net::ChannelKind::value:
                case fxme::net::ChannelKind::scalar:
                case fxme::net::ChannelKind::peak:
                    parts.add (name + " " + juce::String (p.f32(), 3));
                    break;
                case fxme::net::ChannelKind::tap:
                    p.u64();
                    parts.add (name + " " + juce::String ((int) p.u32()) + " samples");
                    break;
                default:
                    parts.add (name + " (kind " + juce::String ((int) kind) + ")");
                    break;
            }
        }
        return parts.joinIntoString (", ");
    }
};

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
              juce::AudioBuffer<float>& destination, std::map<std::uint32_t, SlotTelemetry>& telemetryBySlot)
        : transport (t), input (source), blocks (numBlocks), n (blockSizeIn), output (destination),
          channels (source.getNumChannels()), telemetry (telemetryBySlot)
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

        if (m.kind == wire::Kind::telemetry)
        {
            wire::TelemetryView t;
            if (wire::readTelemetry (m.body, m.size, t))
            {
                auto& slot = telemetry[t.slotId];
                ++slot.frames;
                slot.bytes += t.size;
                slot.last.assign (t.codecFrame, t.codecFrame + t.size);
            }
            return true;
        }

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
    std::map<std::uint32_t, SlotTelemetry>& telemetry;

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
        fxmenet::catalog::useDefaultBuses (*instance);   // as the server does
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
/** Processes the same chain locally, in the same blocks, and compares. */
int compareWithLocal (const Options& o, const juce::OwnedArray<juce::PluginDescription>& descriptions,
                      juce::AudioPluginFormatManager& formats, const juce::AudioBuffer<float>& input,
                      double sampleRate, const juce::AudioBuffer<float>& remote)
{
    if (! o.compare)
        return 0;

    const int channels = input.getNumChannels();
    juce::AudioBuffer<float> local (channels, input.getNumSamples());
    if (! processLocally (descriptions, formats, input, sampleRate, o.blockSize, o.settleMs, local))
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

//==============================================================================
/** --engine: the WAV through ClientEngine, in host buffers, as a DAW would. */
int runEngine (const Options& o, const juce::AudioBuffer<float>& input, double sampleRate,
               const juce::OwnedArray<juce::PluginDescription>& descriptions,
               juce::AudioPluginFormatManager& formats)
{
    const int channels = input.getNumChannels();
    const int length = input.getNumSamples();

    client::ClientEngine engine ("fxmenet-wavclient --engine");
    client::ClientEngine::Settings settings;
    settings.blockSize = o.blockSize;
    settings.budgetBlocks = o.budgetBlocks;
    engine.prepare (sampleRate, o.hostBlock, channels, channels, settings);

    for (int i = 0; i < descriptions.size(); ++i)
        engine.addSlot ((std::uint32_t) descriptions[i]->uniqueId, descriptions[i]->name,
                        descriptions[i]->manufacturerName, i);

    engine.connectTo (o.host, o.port);
    say ("Engine: N " + juce::String (o.blockSize) + ", budget " + juce::String (o.budgetBlocks)
         + " blocks, host buffers " + (o.randomBlocks ? "of random sizes up to " : "of ") + juce::String (o.hostBlock));

    // Connected, and every slot answered.
    const auto deadline = juce::Time::getMillisecondCounter() + 60000;
    juce::String lastStatus;
    for (;;)
    {
        const auto text = engine.getStatusText();
        if (text != lastStatus)
            say ("  " + (lastStatus = text));

        bool settled = engine.getStatus() == client::ClientEngine::Status::ready;
        for (auto& s : engine.getSlots())
            settled = settled && (s.loaded || s.error.isNotEmpty());
        if (settled)
            break;
        if (juce::Time::getMillisecondCounter() > deadline)
        {
            fail ("the engine did not get ready in 60 s");
            return 1;
        }
        juce::Thread::sleep (50);
    }

    bool allLoaded = true;
    for (auto& s : engine.getSlots())
    {
        say ("  slot " + juce::String ((int) s.slotId) + ": " + s.name
             + (s.loaded ? ", " + juce::String ((int) s.info.params.size()) + " parameters, latency "
                           + juce::String (s.info.latency)
                         : ", not loaded: " + s.error));
        allLoaded = allLoaded && s.loaded;
    }
    if (! allLoaded)
        return 1;

    if (o.settleMs > 0)
        juce::Thread::sleep (o.settleMs);

    // Input padded with silence: the output runs `latency` samples behind.
    const int maxLatency = client::BlockStream::latencyFor (o.blockSize, o.hostBlock, o.budgetBlocks, false);
    // Whole host buffers: a shorter last one would rightly switch an aligned
    // stream to the larger latency, at the very end.
    const int total = (length + maxLatency + o.hostBlock) / o.hostBlock * o.hostBlock;
    juce::AudioBuffer<float> padded (channels, total), output (channels, total);
    padded.clear();
    output.clear();
    for (int c = 0; c < channels; ++c)
        padded.copyFrom (c, 0, input, c, 0, length);

    std::mt19937 rng (2026);
    std::uniform_int_distribution<int> pick (1, o.hostBlock);
    std::vector<const float*> in ((std::size_t) channels);
    std::vector<float*> out ((std::size_t) channels);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    int callbacks = 0, latency = 0;
    bool latencyChanged = false;   // reported once
    for (int pos = 0; pos < total;)
    {
        // Random sizes start with an odd one, so the latency settles at once.
        int size = o.randomBlocks ? (callbacks == 0 ? 1 : pick (rng)) : o.hostBlock;
        size = juce::jmin (size, total - pos);

        if (o.realtime)
        {
            const auto due = t0 + 1000.0 * pos / sampleRate;
            const auto now = juce::Time::getMillisecondCounterHiRes();
            if (due > now)
                juce::Thread::sleep ((int) (due - now));
        }

        for (int c = 0; c < channels; ++c)
        {
            in[(std::size_t) c] = padded.getReadPointer (c, pos);
            out[(std::size_t) c] = output.getWritePointer (c, pos);
        }
        engine.process (in.data(), out.data(), size, ! o.realtime, false, nullptr);
        if (callbacks == 0)
            latency = engine.getStreamLatency();   // what the output is aligned to from the start
        else if (engine.getStreamLatency() != latency && ! latencyChanged)
        {
            latencyChanged = true;
            say ("  latency changed after " + juce::String (pos) + " samples: "
                 + juce::String (latency) + " -> " + juce::String (engine.getStreamLatency()));
        }
        pos += size;
        ++callbacks;
    }
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;

    juce::AudioBuffer<float> remote (channels, length);
    for (int c = 0; c < channels; ++c)
        remote.copyFrom (c, 0, output, c, latency, length);

    const auto& st = engine.getStatistics();
    say ("Streamed " + juce::String (1000.0 * length / sampleRate, 1) + " ms of audio in "
         + juce::String (elapsed, 1) + " ms (" + (o.realtime ? "real time" : "offline") + "), "
         + juce::String (callbacks) + " host buffers");
    say ("  blocks sent " + juce::String ((juce::int64) st.sent.load()) + ", replies "
         + juce::String ((juce::int64) st.received.load()) + ", refused (late or duplicate) "
         + juce::String ((juce::int64) st.refused.load()) + ", retries " + juce::String ((juce::int64) st.retries.load())
         + ", concealed " + juce::String ((juce::int64) engine.getConcealedCount())
         + ", rescued by waiting " + juce::String ((juce::int64) st.rescued.load())
         + ", offline timeouts " + juce::String ((juce::int64) st.offlineTimeouts.load()));
    say ("  round trip last " + juce::String (st.lastRttMicros.load() / 1000.0, 3) + " ms, max "
         + juce::String (st.maxRttMicros.load() / 1000.0, 3) + " ms; control ping "
         + juce::String (engine.getControlRttMicros() / 1000.0, 3) + " ms");
    say ("  latency: stream " + juce::String (latency) + " samples + chain " + juce::String (st.chainLatency.load())
         + " = " + juce::String (engine.getLatency()) + " reported to the host");

    if (! writeWav (o.out, remote, sampleRate))
    {
        fail ("cannot write " + o.out.getFullPathName());
        return 1;
    }
    say ("Wrote " + o.out.getFullPathName());

    if (! o.realtime && st.offlineTimeouts.load() > 0)
        return 1;
    return compareWithLocal (o, descriptions, formats, input, sampleRate, remote);
}

//==============================================================================
int discoverServers()
{
    say ("Broadcasting FXMENET_DISCOVER on UDP " + juce::String (discovery::defaultPort) + "...");
    const auto found = discovery::discover (1000);
    for (auto& a : found)
        say ("  \"" + a.name + "\" at " + a.host + ", control " + juce::String (a.controlPort) + ", audio "
             + juce::String (a.udpPort) + ", " + a.os + ", fxmenet " + a.version + ", "
             + juce::String (a.sessions) + " session(s)");
    say (juce::String ((int) found.size()) + " server(s)");
    return found.empty() ? 1 : 0;
}

int run (const Options& o)
{
    if (o.discover)
        return discoverServers();

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

    if (o.engine)
        return runEngine (o, *input, sampleRate, descriptions, formats);

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

    std::map<std::uint32_t, SlotTelemetry> telemetry;
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

        if (o.telemetry && ready.fxmenetAbi > 0)
        {
            telemetry[ready.slotId].readSchema (ready.telemetrySchema);
            if (! request (*connection, Type::telemetryActive, id++, 0, encode (control::SlotValue { ready.slotId, 1 }),
                           Type::telemetryActive, reply))
                return 1;
        }
    }

    // The JSFX scripts, each packed with what it imports (JsfxScripts.h).
    for (int i = 0; i < o.jsfxPaths.size(); ++i)
    {
        jsfx::Bundle bundle;
        juce::String error;
        juce::StringArray notes;
        if (! jsfx::BundleBuilder (juce::File (o.jsfxFolders[i])).build (o.jsfxPaths[i], bundle, error, &notes))
        {
            fail (error);
            return 1;
        }
        for (auto& n : notes)
            say ("  note: " + n);

        control::SlotAdd add;
        add.slotId = (std::uint32_t) (descriptions.size() + i) + 1;
        add.position = (std::uint32_t) (descriptions.size() + i);
        add.uid = jsfx::jsfxUid;
        add.name = o.jsfxPaths[i].toStdString();
        add.vendor = jsfx::jsfxVendor;
        add.state = jsfx::makeSlotState (std::move (bundle));
        const auto sent = add.state.size();
        if (! request (*connection, Type::slotAdd, id++, 0, encode (add), Type::slotReady, reply, 30000))
            return 1;
        const auto ready = control::SlotReady::fromVar (control::fromPayload (reply.payload));
        say ("  slot " + juce::String ((int) ready.slotId) + ": JSFX " + o.jsfxPaths[i] + " (" + juce::String ((int) sent)
             + " bytes sent), " + juce::String ((int) ready.params.size()) + " sliders, latency " + juce::String (ready.latency)
             + (ready.jsfxGfx ? ", has @gfx" : ""));
        for (auto& p : ready.params)
            say ("      " + p.name + (p.hasRange ? "  [" + juce::String (p.rangeMin) + " .. " + juce::String (p.rangeMax) + "]" : juce::String())
                 + (p.stepTexts.isEmpty() ? juce::String() : "  {" + p.stepTexts.joinIntoString (", ") + "}"));
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
    Streamer streamer (transport, *input, numBlocks, o.blockSize, remote, telemetry);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    bool complete = true;
    if (o.realtime)
        streamer.runRealtime (sampleRate, o.budgetBlocks);
    else
        complete = streamer.runOffline();
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;

    // Parameter changes the plugins made themselves arrived meanwhile on the
    // control connection, unread: count them.
    int paramBatches = 0;
    for (Frame f; connection->read (f, 100) == ControlConnection::ReadResult::frame;)
        paramBatches += f.type == Type::paramChanged ? 1 : 0;

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

    for (auto& [slotId, t] : telemetry)
        say ("  telemetry, slot " + juce::String ((int) slotId) + ": " + juce::String (t.frames) + " frames ("
             + juce::String (t.frames / juce::jmax (0.001, elapsed / 1000.0), 1) + " per second), "
             + juce::String (t.frames > 0 ? (int) (t.bytes / (std::size_t) t.frames) : 0) + " bytes each; last: "
             + t.describeLast());
    if (paramBatches > 0)
        say ("  PARAM_CHANGED batches received: " + juce::String (paramBatches));

    if (! writeWav (o.out, remote, sampleRate))
    {
        fail ("cannot write " + o.out.getFullPathName());
        return 1;
    }
    say ("Wrote " + o.out.getFullPathName());

    if (! complete)
        return 1;
    return compareWithLocal (o, descriptions, formats, *input, sampleRate, remote);
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
                     "                         [--settle ms] [--no-compare] [--telemetry]\n"
                     "                         [--jsfx EFFECTS_FOLDER SCRIPT_PATH]...   (JSFX scripts, after the plugins; no comparison)\n"
                     "                         [--engine [--host-block N] [--random-blocks]]\n"
                     "       fxmenet-wavclient --discover" << std::endl;

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return result;
}
