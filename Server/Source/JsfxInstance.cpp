/*
  ------------------------------------------------------------------------------
    JsfxInstance.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "JsfxInstance.h"

#include <jsfx_embed.h>

#include <cmath>

namespace fxmenet::server
{

namespace
{
    constexpr int maxJsfxChannels = 64;

    juce::AudioChannelSet channelSet (int channels)
    {
        return channels == 1 ? juce::AudioChannelSet::mono()
             : channels == 2 ? juce::AudioChannelSet::stereo()
                             : juce::AudioChannelSet::discreteChannels (channels);
    }

    /** The bundle's files under `folder`: Effects/ and Data/, as the engine
        expects them. The paths were checked when the bundle was decoded. */
    bool writeBundle (const jsfx::Bundle& bundle, const juce::File& folder, juce::String& error)
    {
        for (auto& f : bundle.files)
        {
            const auto file = folder.getChildFile (f.data ? "Data" : "Effects")
                                    .getChildFile (juce::String::fromUTF8 (f.path.c_str()));
            if (! file.getParentDirectory().createDirectory()
                || ! file.replaceWithData (f.bytes.data(), f.bytes.size()))
            {
                error = "cannot write " + file.getFullPathName();
                return false;
            }
        }
        // Both must exist, even empty.
        folder.getChildFile ("Effects").createDirectory();
        folder.getChildFile ("Data").createDirectory();
        return true;
    }
}

//==============================================================================
/** One slider. Its normalised value is linear between the slider's ends. */
class JsfxInstance::SliderParameter : public juce::HostedAudioProcessorParameter,
                                      public RangedParameter
{
public:
    SliderParameter (SX_Instance* instance, int parameter) : sx (instance), index (parameter)
    {
        char text[256] = {};
        sx_getParmName (sx, index, text, (int) sizeof (text));
        name = juce::String::fromUTF8 (text);
        isEnum = sx_parmIsEnum (sx, index) > 0;
        const double v = sx_getParmVal (sx, index, &minimum, &maximum, &step);
        defaultValue = normalise (v);
        lastSeen = defaultValue;
    }

    juce::String getParameterID() const override   { return "slider" + juce::String (index); }

    float getValue() const override   { return normalise (sx_getParmVal (sx, index, nullptr, nullptr, nullptr)); }

    void setValue (float newValue) override
    {
        sx_setParmVal (sx, index, real (newValue), 0);
        lastSeen = getValue();
    }

    float getDefaultValue() const override                  { return defaultValue; }
    juce::String getName (int length) const override        { return name.substring (0, length); }
    juce::String getLabel() const override                  { return {}; }

    int getNumSteps() const override
    {
        return isEnum ? juce::jmax (2, (int) std::lround (std::abs (maximum - minimum)) + 1)
                      : juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override   { return isEnum; }

    juce::String getText (float normalised, int length) const override
    {
        char text[256] = {};
        double v = real (normalised);
        sx_getParmDisplay (sx, index, text, (int) sizeof (text), &v);
        return juce::String::fromUTF8 (text).substring (0, length);
    }

    float getValueForText (const juce::String& text) const override
    {
        double v = 0.0;
        if (sx_getParmValueFromString (sx, index, text.toRawUTF8(), &v))
            return normalise (v);
        return normalise (text.getDoubleValue());
    }

    bool getRange (double& lo, double& hi, double& stepOut) const override
    {
        if (isEnum || ! (std::abs (maximum - minimum) > 0.0))
            return false;
        lo = minimum;
        hi = maximum;
        stepOut = step;
        return true;
    }

    /** Message thread: tells the listeners when the script (or a state)
        moved the slider. */
    void notifyIfChanged()
    {
        const float now = getValue();
        if (std::abs (now - lastSeen) > 1.0e-7f)
        {
            lastSeen = now;
            sendValueChangedMessageToListeners (now);
        }
    }

private:
    float normalise (double v) const
    {
        const double span = maximum - minimum;
        return std::abs (span) > 0.0 ? (float) juce::jlimit (0.0, 1.0, (v - minimum) / span) : 0.0f;
    }
    double real (float normalised) const   { return minimum + (double) juce::jlimit (0.0f, 1.0f, normalised) * (maximum - minimum); }

    SX_Instance* const sx;
    const int index;
    juce::String name;
    bool isEnum = false;
    double minimum = 0.0, maximum = 1.0, step = 0.0;
    float defaultValue = 0.0f;
    float lastSeen = 0.0f;
};

//==============================================================================
/** MIDI between the block's buffers and the script's midirecv / midisend
    (the engine's midi_sendrecv hook, as REAPER implements it): events the
    script does not read pass through. Audio thread; nothing allocates. */
struct JsfxInstance::Midi
{
    Midi()
    {
        out.ensureSize (8192);
        scratch.resize (65536);
    }

    void begin (const juce::MidiBuffer& input, int frames)
    {
        in = &input;
        next = input.cbegin();
        out.clear();
        numFrames = frames;
        scratchUsed = 0;
        numPending = 0;
    }

    /** The events the script left unread, then what it sent as buffers. */
    void end()
    {
        for (; next != in->cend(); ++next)
        {
            const auto m = *next;
            out.addEvent (m.data, m.numBytes, m.samplePosition);
        }
        for (int i = 0; i < numPending; ++i)
            out.addEvent (scratch.data() + pending[i].offset, pending[i].size, pending[i].time);
        numPending = 0;
    }

    int clampTime (double t) const   { return juce::jlimit (0, juce::jmax (0, numFrames - 1), (int) t); }

    const juce::MidiBuffer* in = nullptr;
    juce::MidiBufferIterator next;
    juce::MidiBuffer out;
    int numFrames = 0;

    struct Pending { int offset, size, time; };
    std::vector<std::uint8_t> scratch;
    int scratchUsed = 0;
    Pending pending[64];
    int numPending = 0;
};

double JsfxInstance::midiHook (void* context, int action, double* time, double* message1, double* message23, double*)
{
    auto& m = *static_cast<Midi*> (context);
    if (m.in == nullptr)
        return 0.0;

    switch (action)
    {
        case -1:   // midirecv: the next short message
        {
            while (m.next != m.in->cend())
            {
                const auto e = *m.next;
                ++m.next;
                if (e.numBytes < 1 || e.numBytes > 3 || e.data[0] == 0xF0)
                {
                    m.out.addEvent (e.data, e.numBytes, e.samplePosition);   // not for midirecv: through
                    continue;
                }
                if (time != nullptr)       *time = (double) e.samplePosition;
                if (message1 != nullptr)   *message1 = (double) e.data[0];
                if (message23 != nullptr)  *message23 = (double) ((e.numBytes > 1 ? e.data[1] : 0)
                                                                 | ((e.numBytes > 2 ? e.data[2] : 0) << 8));
                return 1.0;
            }
            return 0.0;
        }

        case 1:   // midisend
        {
            if (time == nullptr || message1 == nullptr || message23 == nullptr)
                return 0.0;
            const int status = (int) *message1 & 0xFF;
            const int data = (int) *message23;
            if (status < 0x80)
                return 0.0;
            const std::uint8_t bytes[3] = { (std::uint8_t) status, (std::uint8_t) (data & 0x7F), (std::uint8_t) ((data >> 8) & 0x7F) };
            const int length = juce::MidiMessage::getMessageLengthFromFirstByte ((std::uint8_t) status);
            m.out.addEvent (bytes, juce::jlimit (1, 3, length), m.clampTime (*time));
            return 1.0;
        }

        case 0x101:   // midirecv_buf: the next message, whole
        {
            if (m.next == m.in->cend() || message1 == nullptr || message23 == nullptr)
                return 0.0;
            const auto e = *m.next;
            ++m.next;
            if (time != nullptr)
                *time = (double) e.samplePosition;
            *reinterpret_cast<int*> (message1) = e.numBytes;
            *reinterpret_cast<const std::uint8_t**> (message23) = e.data;
            return 1.0;
        }

        case 0x100:   // midisend_buf: room for a message the script then fills
        {
            if (time == nullptr || message1 == nullptr || message23 == nullptr)
                return 0.0;
            const int size = *reinterpret_cast<int*> (message1);
            if (size < 1 || m.scratchUsed + size > (int) m.scratch.size() || m.numPending >= (int) std::size (m.pending))
                return 0.0;
            m.pending[m.numPending++] = { m.scratchUsed, size, m.clampTime (*time) };
            *reinterpret_cast<std::uint8_t**> (message23) = m.scratch.data() + m.scratchUsed;
            m.scratchUsed += size;
            return 1.0;
        }

        default:   // 0: the end of the block (end() does it)
            break;
    }
    return 0.0;
}

//==============================================================================
std::unique_ptr<JsfxInstance> JsfxInstance::create (const std::vector<std::uint8_t>& slotState,
                                                    double sampleRate, juce::String& error)
{
    jsfx::SlotState state;
    if (! jsfx::decode (slotState.data(), slotState.size(), state))
    {
        error = "the script did not arrive whole (not a JSFX bundle)";
        return nullptr;
    }

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("fxmenet-jsfx-" + juce::Uuid().toString());
    if (! folder.createDirectory() || ! writeBundle (state.bundle, folder, error))
    {
        if (error.isEmpty())
            error = "cannot create " + folder.getFullPathName();
        folder.deleteRecursively();
        return nullptr;
    }

    char message[1024] = {};
    auto* sx = jsfx_embed_create (folder.getFullPathName().toRawUTF8(), state.bundle.main.c_str(),
                                  (int) std::lround (sampleRate), message, (int) sizeof (message));
    if (sx == nullptr)
    {
        error = juce::String::fromUTF8 (message);
        folder.deleteRecursively();
        return nullptr;
    }

    // The script's pins. -1: it declares none (stereo, as REAPER assumes).
    int inputs = 0, outputs = 0;
    sx_getPinInfo (sx, 0, &inputs);
    sx_getPinInfo (sx, 1, &outputs);
    int channels = juce::jmax (inputs, outputs);
    if (channels <= 0)
        channels = 2;   // no pin at all, or "none" both ways (MIDI only): the audio passes
    channels = juce::jmin (channels, maxJsfxChannels);

    return std::unique_ptr<JsfxInstance> (new JsfxInstance (std::move (state.bundle), folder, sx, channels));
}

JsfxInstance::JsfxInstance (jsfx::Bundle b, juce::File f, SX_Instance* instance, int channels)
    : juce::AudioPluginInstance (BusesProperties().withInput ("Input", channelSet (channels), true)
                                                  .withOutput ("Output", channelSet (channels), true)),
      bundle (std::move (b)),
      folder (std::move (f)),
      sx (instance),
      numChannels (channels),
      midi (std::make_unique<Midi>())
{
    name = juce::String::fromUTF8 (jsfx_embed_description (sx));
    if (name.isEmpty())
        name = juce::String::fromUTF8 (bundle.main.c_str());

    const int n = sx_getNumParms (sx);
    for (int i = 0; i < n; ++i)
        addHostedParameter (std::make_unique<SliderParameter> (sx, i));

    sx_set_midi_ctx (sx, &JsfxInstance::midiHook, midi.get());
    startTimerHz (20);
}

JsfxInstance::~JsfxInstance()
{
    stopTimer();
    sx_destroyInstance (sx);
    folder.deleteRecursively();
}

bool JsfxInstance::hasGfx() const   { return jsfx_embed_has_gfx (sx); }

void JsfxInstance::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = name;
    d.descriptiveName = name;
    d.pluginFormatName = "JSFX";
    d.manufacturerName = jsfx::jsfxVendor;
    d.category = "Fx";
    d.fileOrIdentifier = juce::String::fromUTF8 (bundle.main.c_str());
    d.uniqueId = (int) jsfx::jsfxUid;
    d.isInstrument = false;
    d.numInputChannels = numChannels;
    d.numOutputChannels = numChannels;
}

bool JsfxInstance::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannels() == numChannels && layout.getMainOutputChannels() == numChannels;
}

double JsfxInstance::getTailLengthSeconds() const
{
    const double samples = sx_getTailSize (sx);
    return samples > 0.0 && rate > 0.0 ? samples / rate : 0.0;
}

//==============================================================================
void JsfxInstance::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    rate = sampleRate;
    workFrames = juce::jmax (16, maximumBlockSize);
    work.assign ((std::size_t) workFrames * (std::size_t) numChannels, 0.0);
    setLatencySamples (sx_getCurrentLatency (sx));
}

void JsfxInstance::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin (buffer.getNumChannels(), numChannels);
    const int total = buffer.getNumSamples();
    if (channels < 1 || workFrames < 1)
        return;

    // The play head, as the script's play_state, tempo and positions.
    double tempo = 120.0, seconds = 0.0, beats = 0.0, playState = 0.0;
    int numerator = 4, denominator = 4;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            if (const auto bpm = position->getBpm())                 tempo = *bpm;
            if (const auto t = position->getTimeInSeconds())         seconds = *t;
            if (const auto ppq = position->getPpqPosition())         beats = *ppq;
            if (const auto sig = position->getTimeSignature())       { numerator = sig->numerator; denominator = sig->denominator; }
            playState = position->getIsRecording() ? 5.0 : position->getIsPlaying() ? 1.0 : 0.0;
        }

    midi->begin (midiMessages, total);

    // In pieces if the host gives more than it announced. (MIDI times stay
    // those of the whole block: scripts then see them late in the first
    // piece, which only happens with a host that breaks its own maximum.)
    for (int start = 0; start < total; start += workFrames)
    {
        const int n = juce::jmin (workFrames, total - start);
        for (int c = 0; c < channels; ++c)
        {
            const float* in = buffer.getReadPointer (c, start);
            double* w = work.data() + c;
            for (int i = 0; i < n; ++i, w += channels)
                *w = (double) in[i];
        }

        sx_processSamples (sx, work.data(), n, channels, (int) std::lround (rate),
                           tempo, numerator, denominator, playState,
                           seconds + (double) start / rate, beats + (double) start / rate * tempo / 60.0,
                           1.0, 1.0, 0);

        for (int c = 0; c < channels; ++c)
        {
            float* out = buffer.getWritePointer (c, start);
            const double* w = work.data() + c;
            for (int i = 0; i < n; ++i, w += channels)
                out[i] = (float) *w;
        }
    }

    midi->end();
    midiMessages.swapWith (midi->out);

    // pdc_delay can change while the script runs; the chain reads it here.
    const int latency = sx_getCurrentLatency (sx);
    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

//==============================================================================
void JsfxInstance::timerCallback()
{
    // Sliders the script moved (slider_automate, sliderchange) or a state set.
    sx_hasParmChanged (sx);   // clears the engine's flag; every slider is compared anyway
    for (auto* p : getParameters())
        if (auto* slider = dynamic_cast<SliderParameter*> (p))
            slider->notifyIfChanged();
}

void JsfxInstance::getStateInformation (juce::MemoryBlock& destination)
{
    jsfx::SlotState state;
    state.bundle = bundle;

    int length = 0;
    if (const char* text = sx_saveState (sx, &length))
        state.sliders.assign (text, (std::size_t) juce::jmax (0, length));

    length = 0;
    if (const char* bytes = sx_saveSerState (sx, &length))
        if (length > 0)
            state.serialized.assign ((const std::uint8_t*) bytes, (const std::uint8_t*) bytes + length);

    const auto encoded = jsfx::encode (state);
    destination.replaceAll (encoded.data(), encoded.size());
}

void JsfxInstance::setStateInformation (const void* data, int size)
{
    // The script itself stays the one this instance was created from.
    jsfx::SlotState state;
    if (data == nullptr || size <= 0 || ! jsfx::decode ((const std::uint8_t*) data, (std::size_t) size, state))
        return;
    if (! state.sliders.empty())
        sx_loadState (sx, state.sliders.c_str());
    if (! state.serialized.empty())
        sx_loadSerState (sx, (const char*) state.serialized.data(), (int) state.serialized.size());
}

} // namespace fxmenet::server
