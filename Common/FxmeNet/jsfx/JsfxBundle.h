/*
  ------------------------------------------------------------------------------
    JsfxBundle.h

    A JSFX script as it travels from the client to the server (doc/jsfx-plan.md):
    the script, the files it imports and its resources (images), each with
    its path under the client's Effects folder (or Data folder), so that the
    server's loader finds them as REAPER's does.

    A JSFX slot is an ordinary slot: SLOT_ADD with the uid jsfxUid, the
    vendor jsfxVendor, the script's path as its name, and a SlotState as its
    state. The bundle rides in the state, so it is saved in the DAW project
    and sent again when the chain is rebuilt, with no message of its own.

    JUCE-free.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/net/TelemetryCodec.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace fxmenet::jsfx
{

constexpr std::uint32_t jsfxUid = 0x4A534658;   ///< 'JSFX': every script, told apart by name
inline constexpr const char* jsfxVendor = "JSFX";

/** Limits a server accepts. */
constexpr std::size_t maxFiles = 256;
constexpr std::size_t maxBundleBytes = 32u << 20;

struct File
{
    std::string path;   ///< relative, '/' separated, under Effects/ (or Data/ when `data`)
    bool data = false;
    std::vector<std::uint8_t> bytes;
};

struct Bundle
{
    std::string main;   ///< the script's path under Effects/ (one of `files`)
    std::vector<File> files;

    bool empty() const noexcept   { return main.empty() || files.empty(); }

    std::size_t totalBytes() const noexcept
    {
        std::size_t n = 0;
        for (auto& f : files)
            n += f.bytes.size();
        return n;
    }
};

/** A JSFX slot's state: its bundle, the sliders (sx_saveState's text) and
    what @serialize wrote. */
struct SlotState
{
    Bundle bundle;
    std::string sliders;
    std::vector<std::uint8_t> serialized;
};

/** A path that stays inside the folder it is joined to: relative, no empty,
    "." or ".." part, no backslash, colon or control character. */
inline bool isSafePath (const std::string& path)
{
    if (path.empty() || path.size() > 512 || path.front() == '/' || path.back() == '/')
        return false;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const auto end = std::min (path.find ('/', start), path.size());
        const auto part = path.substr (start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (const char c : part)
            if (c == '\\' || c == ':' || (unsigned char) c < 32)
                return false;
        start = end + 1;
    }
    return true;
}

constexpr std::uint32_t stateMagic = 0x53464A46;   ///< "FJFS"
constexpr std::uint32_t stateVersion = 1;

inline std::vector<std::uint8_t> encode (const SlotState& s)
{
    std::vector<std::uint8_t> out;
    fxme::net::ByteWriter w (out);
    const auto blob = [&w] (const void* p, std::size_t n)
    {
        w.u32 ((std::uint32_t) n);
        w.bytes (p, n);
    };
    w.u32 (stateMagic);
    w.u32 (stateVersion);
    blob (s.bundle.main.data(), s.bundle.main.size());
    w.u32 ((std::uint32_t) s.bundle.files.size());
    for (auto& f : s.bundle.files)
    {
        blob (f.path.data(), f.path.size());
        w.u8 (f.data ? 1 : 0);
        blob (f.bytes.data(), f.bytes.size());
    }
    blob (s.sliders.data(), s.sliders.size());
    blob (s.serialized.data(), s.serialized.size());
    return out;
}

/** False when the bytes are not a SlotState, or break a limit or a path rule. */
inline bool decode (const std::uint8_t* data, std::size_t size, SlotState& s)
{
    fxme::net::ByteReader r (data, size);
    const auto blob = [&r] (const std::uint8_t*& p, std::size_t& n)
    {
        n = r.u32();
        p = r.take (n);
        return p != nullptr || n == 0;
    };
    const auto text = [&blob] (std::string& out)
    {
        const std::uint8_t* p = nullptr;
        std::size_t n = 0;
        if (! blob (p, n))
            return false;
        out.assign ((const char*) p, n);
        return true;
    };

    if (r.u32() != stateMagic || r.u32() != stateVersion || ! text (s.bundle.main))
        return false;
    const auto count = r.u32();
    if (! r.ok() || count > maxFiles)
        return false;
    s.bundle.files.clear();
    std::size_t total = 0;
    bool mainFound = false;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        File f;
        const std::uint8_t* p = nullptr;
        std::size_t n = 0;
        if (! text (f.path))
            return false;
        f.data = r.u8() != 0;
        if (! blob (p, n) || ! r.ok() || ! isSafePath (f.path))
            return false;
        total += n;
        if (total > maxBundleBytes)
            return false;
        f.bytes.assign (p, p + n);
        mainFound = mainFound || (! f.data && f.path == s.bundle.main);
        s.bundle.files.push_back (std::move (f));
    }
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    if (! text (s.sliders) || ! blob (p, n) || ! r.ok() || ! mainFound)
        return false;
    s.serialized.assign (p, p + n);
    return true;
}

} // namespace fxmenet::jsfx
