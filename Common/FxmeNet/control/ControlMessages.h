/*
  ------------------------------------------------------------------------------
    ControlMessages.h

    The JSON control messages (protocol.md section 4.2): HELLO, WELCOME,
    CONFIGURE, ERROR, PLUGIN_LIST, SLOT_READY, as plain structs with
    toVar / fromVar. 64-bit values (tokens, channel masks) and the plugin uid
    travel as hex strings: JSON numbers are doubles, exact only to 2^53.
    A field missing from the JSON keeps its default, an unknown one is
    ignored, so either side may be a newer version.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <FxmeNet/chain/Routing.h>
#include <FxmeNet/control/ControlFrame.h>

#include <cstdint>
#include <vector>

namespace fxmenet::control
{

namespace json
{
    inline juce::String hex64 (std::uint64_t v)   { return juce::String::toHexString ((juce::int64) v).paddedLeft ('0', 16); }
    inline juce::String hex32 (std::uint32_t v)   { return juce::String::toHexString ((int) v).paddedLeft ('0', 8); }

    inline std::uint64_t fromHex64 (const juce::var& v, std::uint64_t fallback = 0)
    {
        return v.isString() ? (std::uint64_t) v.toString().getHexValue64() : fallback;
    }

    inline std::uint32_t fromHex32 (const juce::var& v, std::uint32_t fallback = 0)
    {
        return v.isString() ? (std::uint32_t) v.toString().getHexValue32() : fallback;
    }

    inline juce::var stringArray (const juce::StringArray& a)
    {
        juce::Array<juce::var> out;
        for (auto& s : a)
            out.add (s);
        return out;
    }

    inline juce::StringArray toStringArray (const juce::var& v)
    {
        juce::StringArray out;
        if (auto* a = v.getArray())
            for (auto& s : *a)
                out.add (s.toString());
        return out;
    }

    /** obj[name] if present, else `fallback`. */
    inline juce::var get (const juce::var& obj, const char* name, const juce::var& fallback = {})
    {
        return obj.hasProperty (name) ? obj[name] : fallback;
    }
}

//==============================================================================
struct Hello
{
    juce::Array<int> versions { 1 };
    juce::String clientName, os, fxmenetVersion;
    bool resume = false;
    std::uint32_t resumeSessionId = 0;
    std::uint64_t resumeToken = 0;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        juce::Array<juce::var> v;
        for (auto x : versions)
            v.add (x);
        o->setProperty ("versions", v);
        o->setProperty ("clientName", clientName);
        o->setProperty ("os", os);
        o->setProperty ("fxmenetVersion", fxmenetVersion);
        if (resume)
        {
            auto* r = new juce::DynamicObject();
            r->setProperty ("sessionId", (int) resumeSessionId);
            r->setProperty ("token", json::hex64 (resumeToken));
            o->setProperty ("resume", r);
        }
        return o;
    }

    static Hello fromVar (const juce::var& v)
    {
        Hello h;
        if (auto* a = v["versions"].getArray())
        {
            h.versions.clear();
            for (auto& x : *a)
                h.versions.add ((int) x);
        }
        h.clientName = v["clientName"].toString();
        h.os = v["os"].toString();
        h.fxmenetVersion = v["fxmenetVersion"].toString();
        const auto r = v["resume"];
        h.resume = r.isObject();
        if (h.resume)
        {
            h.resumeSessionId = (std::uint32_t) (int) r["sessionId"];
            h.resumeToken = json::fromHex64 (r["token"]);
        }
        return h;
    }
};

//==============================================================================
/** In WELCOME's capabilities: the server applies ROUTING (doc/routing.md). */
constexpr const char* capabilityRouting = "routing";

struct Welcome
{
    int version = 1;
    std::uint32_t sessionId = 0;
    std::uint64_t token = 0;
    int udpPort = 54321;
    juce::String serverName, os;
    juce::StringArray capabilities { "udp-audio" };
    bool resumed = false;
    juce::String error;   ///< non-empty: refused, nothing else is meaningful

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("version", version);
        o->setProperty ("sessionId", (int) sessionId);
        o->setProperty ("token", json::hex64 (token));
        o->setProperty ("udpPort", udpPort);
        o->setProperty ("serverName", serverName);
        o->setProperty ("os", os);
        o->setProperty ("capabilities", json::stringArray (capabilities));
        o->setProperty ("resumed", resumed);
        if (error.isNotEmpty())
            o->setProperty ("error", error);
        return o;
    }

    static Welcome fromVar (const juce::var& v)
    {
        Welcome w;
        w.version = json::get (v, "version", w.version);
        w.sessionId = (std::uint32_t) (int) v["sessionId"];
        w.token = json::fromHex64 (v["token"]);
        w.udpPort = json::get (v, "udpPort", w.udpPort);
        w.serverName = v["serverName"].toString();
        w.os = v["os"].toString();
        w.capabilities = json::toStringArray (v["capabilities"]);
        w.resumed = (bool) json::get (v, "resumed", false);
        w.error = v["error"].toString();
        return w;
    }
};

//==============================================================================
struct Configure
{
    double sampleRate = 48000.0;
    int blockSize = 128;
    int numInputs = 2, numOutputs = 2;
    std::uint64_t inputMask = 3, outputMask = 3;
    int telemetryHz = 30;
    int datagramBudget = 1400;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("sampleRate", sampleRate);
        o->setProperty ("blockSize", blockSize);
        o->setProperty ("numInputs", numInputs);
        o->setProperty ("numOutputs", numOutputs);
        o->setProperty ("inputMask", json::hex64 (inputMask));
        o->setProperty ("outputMask", json::hex64 (outputMask));
        o->setProperty ("telemetryHz", telemetryHz);
        o->setProperty ("datagramBudget", datagramBudget);
        return o;
    }

    static Configure fromVar (const juce::var& v)
    {
        Configure c;
        c.sampleRate = json::get (v, "sampleRate", c.sampleRate);
        c.blockSize = json::get (v, "blockSize", c.blockSize);
        c.numInputs = json::get (v, "numInputs", c.numInputs);
        c.numOutputs = json::get (v, "numOutputs", c.numOutputs);
        c.inputMask = json::fromHex64 (v["inputMask"], c.inputMask);
        c.outputMask = json::fromHex64 (v["outputMask"], c.outputMask);
        c.telemetryHz = json::get (v, "telemetryHz", c.telemetryHz);
        c.datagramBudget = json::get (v, "datagramBudget", c.datagramBudget);
        return c;
    }
};

//==============================================================================
struct ErrorMessage
{
    enum Code
    {
        unknown          = 1,
        badRequest       = 2,
        unsupported      = 3,
        pluginNotFound   = 4,
        pluginLoadFailed = 5,
        noSuchSlot       = 6,
        notConfigured    = 7
    };

    int code = unknown;
    juce::String text;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("code", code);
        o->setProperty ("text", text);
        return o;
    }

    static ErrorMessage fromVar (const juce::var& v)
    {
        ErrorMessage e;
        e.code = json::get (v, "code", e.code);
        e.text = v["text"].toString();
        return e;
    }
};

//==============================================================================
struct PluginEntry
{
    std::uint32_t uid = 0;
    juce::String name, vendor, version, category, sha256;
    int inputs = 2, outputs = 2;
    int fxmenetAbi = 0;   ///< 0: not FxmeNet-aware (null in the JSON)
    juce::StringArray platforms;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("uid", json::hex32 (uid));
        o->setProperty ("name", name);
        o->setProperty ("vendor", vendor);
        o->setProperty ("version", version);
        o->setProperty ("category", category);
        o->setProperty ("sha256", sha256);
        o->setProperty ("inputs", inputs);
        o->setProperty ("outputs", outputs);
        o->setProperty ("fxmenetAbi", fxmenetAbi > 0 ? juce::var (fxmenetAbi) : juce::var());
        o->setProperty ("platforms", json::stringArray (platforms));
        return o;
    }

    static PluginEntry fromVar (const juce::var& v)
    {
        PluginEntry p;
        p.uid = json::fromHex32 (v["uid"]);
        p.name = v["name"].toString();
        p.vendor = v["vendor"].toString();
        p.version = v["version"].toString();
        p.category = v["category"].toString();
        p.sha256 = v["sha256"].toString();
        p.inputs = json::get (v, "inputs", p.inputs);
        p.outputs = json::get (v, "outputs", p.outputs);
        p.fxmenetAbi = v["fxmenetAbi"].isVoid() ? 0 : (int) v["fxmenetAbi"];
        p.platforms = json::toStringArray (v["platforms"]);
        return p;
    }
};

inline juce::var pluginListToVar (const std::vector<PluginEntry>& plugins)
{
    juce::Array<juce::var> a;
    for (auto& p : plugins)
        a.add (p.toVar());
    auto* o = new juce::DynamicObject();
    o->setProperty ("plugins", a);
    return o;
}

inline std::vector<PluginEntry> pluginListFromVar (const juce::var& v)
{
    std::vector<PluginEntry> out;
    if (auto* a = v["plugins"].getArray())
        for (auto& p : *a)
            out.push_back (PluginEntry::fromVar (p));
    return out;
}

//==============================================================================
struct ParamInfo
{
    int index = 0;
    juce::String paramId, name, label, group;
    float defaultValue = 0.0f;
    float value = 0.0f;   ///< current, normalised (absent from older servers: the default)
    int numSteps = 0;   ///< 0: continuous
    bool automatable = true, isBypass = false;
    juce::StringArray stepTexts;   ///< for stepped parameters, up to 32

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("index", index);
        o->setProperty ("id", paramId);
        o->setProperty ("name", name);
        o->setProperty ("label", label);
        o->setProperty ("group", group);
        o->setProperty ("default", defaultValue);
        o->setProperty ("value", value);
        o->setProperty ("steps", numSteps);
        o->setProperty ("automatable", automatable);
        o->setProperty ("bypass", isBypass);
        if (! stepTexts.isEmpty())
            o->setProperty ("stepTexts", json::stringArray (stepTexts));
        return o;
    }

    static ParamInfo fromVar (const juce::var& v)
    {
        ParamInfo p;
        p.index = json::get (v, "index", 0);
        p.paramId = v["id"].toString();
        p.name = v["name"].toString();
        p.label = v["label"].toString();
        p.group = v["group"].toString();
        p.defaultValue = (float) (double) json::get (v, "default", 0.0);
        p.value = (float) (double) json::get (v, "value", p.defaultValue);
        p.numSteps = json::get (v, "steps", 0);
        p.automatable = (bool) json::get (v, "automatable", true);
        p.isBypass = (bool) json::get (v, "bypass", false);
        p.stepTexts = json::toStringArray (v["stepTexts"]);
        return p;
    }
};

/** One of a plugin's buses (in SLOT_READY), in the plugin's order: its
    channel count is its full width, enabled or not, so that the routing's
    channel numbers do not move when a bus is enabled. */
struct BusInfo
{
    juce::String name;
    bool input = true;
    int channels = 0;
    bool main = false;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", name);
        o->setProperty ("input", input);
        o->setProperty ("channels", channels);
        o->setProperty ("main", main);
        return o;
    }

    static BusInfo fromVar (const juce::var& v)
    {
        BusInfo b;
        b.name = v["name"].toString();
        b.input = (bool) json::get (v, "input", true);
        b.channels = juce::jlimit (0, 1024, (int) json::get (v, "channels", 0));
        b.main = (bool) json::get (v, "main", false);
        return b;
    }
};

struct SlotReady
{
    std::uint32_t slotId = 0;
    int latency = 0;
    int numInputs = 2, numOutputs = 2;   ///< enabled channels, now
    std::vector<BusInfo> buses;          ///< empty from older servers
    std::vector<ParamInfo> params;
    juce::MemoryBlock telemetrySchema;   ///< TelemetryCodec schema; empty when not FxmeNet-aware
    int fxmenetAbi = 0;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("slotId", (int) slotId);
        o->setProperty ("latency", latency);
        o->setProperty ("numInputs", numInputs);
        o->setProperty ("numOutputs", numOutputs);
        juce::Array<juce::var> b;
        for (auto& bus : buses)
            b.add (bus.toVar());
        o->setProperty ("buses", b);
        juce::Array<juce::var> a;
        for (auto& p : params)
            a.add (p.toVar());
        o->setProperty ("params", a);
        o->setProperty ("telemetrySchema", telemetrySchema.toBase64Encoding());
        o->setProperty ("fxmenetAbi", fxmenetAbi > 0 ? juce::var (fxmenetAbi) : juce::var());
        return o;
    }

    static SlotReady fromVar (const juce::var& v)
    {
        SlotReady s;
        s.slotId = (std::uint32_t) (int) v["slotId"];
        s.latency = json::get (v, "latency", 0);
        s.numInputs = json::get (v, "numInputs", s.numInputs);
        s.numOutputs = json::get (v, "numOutputs", s.numOutputs);
        if (auto* b = v["buses"].getArray())
            for (auto& bus : *b)
                s.buses.push_back (BusInfo::fromVar (bus));
        if (auto* a = v["params"].getArray())
            for (auto& p : *a)
                s.params.push_back (ParamInfo::fromVar (p));
        s.telemetrySchema.fromBase64Encoding (v["telemetrySchema"].toString());
        s.fxmenetAbi = v["fxmenetAbi"].isVoid() ? 0 : (int) v["fxmenetAbi"];
        return s;
    }
};

/** A loaded slot as a box of the routing model. Without buses (an older
    server): one main bus each way, of its channel counts. */
inline chain::Box toBox (const SlotReady& s)
{
    chain::Box box;
    box.id = s.slotId;
    if (s.buses.empty())
    {
        if (s.numInputs > 0)
            box.inputs.push_back ({ "Main", s.numInputs, true });
        if (s.numOutputs > 0)
            box.outputs.push_back ({ "Main", s.numOutputs, true });
        return box;
    }
    for (auto& b : s.buses)
        (b.input ? box.inputs : box.outputs).push_back ({ b.name.toStdString(), b.channels, b.main });
    return box;
}

static_assert (chain::inputsBox == routingInputs && chain::outputsBox == routingOutputs);

/** Routing connections to and from the wire. */
inline std::vector<Route> toRoutes (const std::vector<chain::Connection>& connections)
{
    std::vector<Route> out;
    for (auto& c : connections)
        out.push_back ({ c.from.box, (std::uint16_t) c.from.channel, c.to.box, (std::uint16_t) c.to.channel });
    return out;
}

inline std::vector<chain::Connection> fromRoutes (const std::vector<Route>& routes)
{
    std::vector<chain::Connection> out;
    for (auto& r : routes)
        out.push_back ({ { r.fromBox, (int) r.fromChannel }, { r.toBox, (int) r.toChannel } });
    return out;
}

//==============================================================================
/** UTF-8 JSON text of a message, as a frame payload. */
inline std::vector<std::uint8_t> toPayload (const juce::var& v)
{
    const auto text = juce::JSON::toString (v, true);
    const auto* utf8 = text.toRawUTF8();
    return std::vector<std::uint8_t> (utf8, utf8 + text.getNumBytesAsUTF8());
}

/** Parses a JSON payload; a void var when it is not valid JSON. */
inline juce::var fromPayload (const std::vector<std::uint8_t>& payload)
{
    return juce::JSON::parse (juce::String::fromUTF8 ((const char*) payload.data(), (int) payload.size()));
}

} // namespace fxmenet::control
