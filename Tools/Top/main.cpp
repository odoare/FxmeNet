/*
  ------------------------------------------------------------------------------
    main.cpp  (fxmenet-top)

    What an fxmenet-server is doing, at a glance (like top), and its plugin
    folders:

      fxmenet-top [HOST[:PORT]]   full screen, refreshed every second (this
                                  machine's server by default)
      fxmenet-top --once [HOST]   one frame without colours, then exits
      fxmenet-top --find          the servers on the local network, then exits

    Three views: 1 sessions (each client, its chain, its figures), 2 plugins
    (the folders searched, the scan, the plugins found), 3 the server's log.
    It connects to the server's control port as a monitor (protocol.md
    section 4.7): no session, nothing touches the audio. Changing the
    folders is allowed from the server's own machine (or a server started
    with --remote-admin); watching from anywhere.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Terminal.h"

#include <juce_core/juce_core.h>

#include <FxmeNet/control/ControlConnection.h>
#include <FxmeNet/control/ControlMessages.h>
#include <FxmeNet/control/Discovery.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using fxmenet::top::Terminal;
namespace control = fxmenet::control;

namespace
{
    std::atomic<bool> stop { false };

    extern "C" void onSignal (int)   { stop = true; }

    using Clock = std::chrono::steady_clock;

    //==========================================================================
    // A line of the frame, clipped to the terminal width. Width is counted in
    // code points: the characters used here are all one column wide.
    struct Style
    {
        const char* bold = "\x1b[1m";
        const char* dim = "\x1b[2m";
        const char* title = "\x1b[1;36m";
        const char* accent = "\x1b[1;33m";
        const char* warn = "\x1b[1;31m";
        const char* good = "\x1b[32m";
        const char* inverse = "\x1b[7m";
        const char* reset = "\x1b[0m";

        static Style none() { return { "", "", "", "", "", "", "", "" }; }
    };

    class Line
    {
    public:
        Line (int maxWidth, const Style& s) : max (maxWidth), st (s) {}

        Line& put (const std::string& text, const char* style = nullptr)
        {
            if (style && *style)
                out += style;
            for (std::size_t i = 0; i < text.size() && used < max;)
            {
                const auto c = static_cast<unsigned char> (text[i]);
                const std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                out.append (text, i, n);
                i += n;
                ++used;
            }
            if (style && *style)
                out += st.reset;
            return *this;
        }

        Line& put (const juce::String& text, const char* style = nullptr)   { return put (text.toStdString(), style); }
        Line& put (const char* text, const char* style = nullptr)           { return put (std::string (text), style); }
        Line& pad (int column) { return put (std::string (static_cast<std::size_t> (std::max (0, column - used)), ' ')); }
        int width() const noexcept { return used; }
        std::string str() const { return out + "\x1b[K"; }

    private:
        std::string out;
        int used = 0;
        int max;
        const Style& st;
    };

    std::string fixed (double v, int decimals)
    {
        char buf[64];
        std::snprintf (buf, sizeof buf, "%.*f", decimals, v);
        return buf;
    }

    std::string bar (double fraction, int width)
    {
        fraction = std::clamp (fraction, 0.0, 1.0);
        const int full = static_cast<int> (std::lround (fraction * width));
        std::string s;
        for (int i = 0; i < width; ++i)
            s += i < full ? "█" : "░";
        return s;
    }

    std::string bitRate (double bytesPerSecond)
    {
        const double bits = bytesPerSecond * 8.0;
        if (bits >= 1.0e6)
            return fixed (bits / 1.0e6, 1) + " Mb/s";
        return fixed (bits / 1.0e3, 0) + " kb/s";
    }

    std::string byteSize (double bytes)
    {
        if (bytes >= 1.0e9)
            return fixed (bytes / 1.0e9, 2) + " GB";
        return fixed (bytes / 1.0e6, 0) + " MB";
    }

    std::string duration (juce::int64 seconds)
    {
        const auto d = seconds / 86400, h = seconds / 3600 % 24, m = seconds / 60 % 60, s = seconds % 60;
        if (d > 0)
            return std::to_string (d) + "d" + std::to_string (h) + "h";
        if (h > 0)
            return std::to_string (h) + "h" + (m < 10 ? "0" : "") + std::to_string (m);
        if (m > 0)
            return std::to_string (m) + "m" + (s < 10 ? "0" : "") + std::to_string (s);
        return std::to_string (s) + "s";
    }

    const juce::Array<juce::var>& arrayOf (const juce::var& v)
    {
        static const juce::Array<juce::var> empty;
        const auto* a = v.getArray();
        return a != nullptr ? *a : empty;
    }

    /** Every word of `filter` is in `text` (case ignored). */
    bool matches (const juce::String& text, const juce::String& filter)
    {
        for (auto& w : juce::StringArray::fromTokens (filter, " ", ""))
            if (! text.containsIgnoreCase (w))
                return false;
        return true;
    }

    //==========================================================================
    /** The connection to the server, on a thread of its own: connects (and
        reconnects), asks for the status every second, sends the commands. */
    class Link : private juce::Thread
    {
    public:
        struct View
        {
            bool connected = false, admin = false, everReceived = false;
            juce::String state;                 ///< what the connection is doing
            juce::var status;                   ///< the last MONITOR_STATUS
            juce::Array<juce::var> plugins;     ///< the catalog, sent when it changes
            std::vector<juce::String> log;      ///< every line received
            juce::String message;               ///< the last command's outcome
            Clock::time_point messageAt;
        };

        Link (juce::String h, int p) : juce::Thread ("fxmenet-top link"), host (std::move (h)), port (p)
        {
            view.state = "connecting to " + where();
            startThread();
        }

        ~Link() override
        {
            signalThreadShouldExit();
            stopThread (4000);
        }

        juce::String where() const   { return host + ":" + juce::String (port); }

        View snapshot() const
        {
            const std::lock_guard<std::mutex> l (lock);
            return view;
        }

        void setPaths (const juce::StringArray& saved, bool useDefaults)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("paths", control::json::stringArray (saved));
            o->setProperty ("useDefaultPaths", useDefaults);
            queue (control::Type::monitorPaths, o, "folders saved, rescanning");
        }

        void rescan (bool full)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("full", full);
            queue (control::Type::monitorRescan, o, full ? "full rescan started" : "rescan started");
        }

        void say (const juce::String& text)
        {
            const std::lock_guard<std::mutex> l (lock);
            view.message = text;
            view.messageAt = Clock::now();
        }

    private:
        struct Command
        {
            control::Type type;
            juce::var payload;
            juce::String done;
        };

        void queue (control::Type type, juce::var payload, const juce::String& done)
        {
            const std::lock_guard<std::mutex> l (lock);
            commands.push_back ({ type, std::move (payload), done });
        }

        void setState (const juce::String& s, bool connected = false, bool admin = false)
        {
            const std::lock_guard<std::mutex> l (lock);
            view.state = s;
            view.connected = connected;
            view.admin = admin;
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                session();
                if (! threadShouldExit())
                    wait (1000);
            }
        }

        /** One connection, until it drops. */
        void session()
        {
            auto socket = std::make_unique<juce::StreamingSocket>();
            if (! socket->connect (host, port, 2000))
            {
                setState ("no server at " + where() + ", retrying");
                return;
            }
            control::ControlConnection connection (std::move (socket));

            control::Hello hello;
            hello.clientName = "fxmenet-top";
            hello.os = juce::SystemStats::getOperatingSystemName();
            hello.fxmenetVersion = FXMENET_VERSION;
            hello.role = control::roleMonitor;
            connection.sendJson (control::Type::hello, 1, hello.toVar());

            control::Frame f;
            if (connection.read (f, 5000) != control::ControlConnection::ReadResult::frame
                || f.type != control::Type::welcome)
            {
                setState ("no answer from " + where() + ", retrying");
                return;
            }
            const auto welcome = control::Welcome::fromVar (control::fromPayload (f.payload));
            if (welcome.error.isNotEmpty() || ! welcome.capabilities.contains (control::roleMonitor))
            {
                setState (where() + " refused: " + (welcome.error.isNotEmpty() ? welcome.error
                                                                                : juce::String ("no monitor support (another version?)")));
                connection.close();
                return;
            }
            setState ("connected to " + where(), true, welcome.capabilities.contains (control::capabilityAdmin));

            std::uint32_t nextRequest = 2, statusRequest = 0;
            std::map<std::uint32_t, juce::String> pending;   // command request -> what to say when done
            std::uint64_t logNext = 0;
            int revision = -1;
            auto nextStatus = Clock::now();

            while (! threadShouldExit())
            {
                std::vector<Command> toSend;
                {
                    const std::lock_guard<std::mutex> l (lock);
                    toSend.swap (commands);
                }
                for (auto& c : toSend)
                {
                    pending[nextRequest] = c.done;
                    connection.sendJson (c.type, nextRequest++, c.payload);
                    nextStatus = Clock::now();   // show the scan at once
                }

                if (Clock::now() >= nextStatus && statusRequest == 0)
                {
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("logSince", (juce::int64) logNext);
                    o->setProperty ("catalogRevision", revision);
                    statusRequest = nextRequest++;
                    connection.sendJson (control::Type::monitorStatus, statusRequest, juce::var (o));
                    nextStatus = Clock::now() + std::chrono::seconds (1);
                }

                const auto r = connection.read (f, 50);
                if (r == control::ControlConnection::ReadResult::closed)
                {
                    setState ("lost " + where() + ", reconnecting");
                    return;
                }
                if (r != control::ControlConnection::ReadResult::frame)
                    continue;

                if (f.type == control::Type::monitorStatus && f.requestId == statusRequest)
                {
                    statusRequest = 0;
                    const auto status = control::fromPayload (f.payload);
                    const auto& catalog = status["catalog"];
                    const std::lock_guard<std::mutex> l (lock);
                    view.status = status;
                    view.everReceived = true;
                    if (catalog.hasProperty ("plugins"))
                    {
                        view.plugins = arrayOf (catalog["plugins"]);
                        revision = (int) catalog["revision"];
                    }
                    for (auto& line : arrayOf (status["log"]["lines"]))
                        view.log.push_back (line.toString());
                    if (view.log.size() > 5000)
                        view.log.erase (view.log.begin(), view.log.begin() + (std::ptrdiff_t) (view.log.size() - 5000));
                    logNext = (std::uint64_t) (juce::int64) status["log"]["next"];
                }
                else if (const auto it = pending.find (f.requestId); it != pending.end())
                {
                    say (f.type == control::Type::error ? control::fromPayload (f.payload)["text"].toString() : it->second);
                    pending.erase (it);
                }
            }
            connection.sendEmpty (control::Type::bye);
            connection.close();
        }

        const juce::String host;
        const int port;
        mutable std::mutex lock;
        View view;
        std::vector<Command> commands;
    };

    //==========================================================================
    class Top
    {
    public:
        explicit Top (Link& l) : link (l) {}

        enum class Page { sessions, plugins, log };

        //--- keys --------------------------------------------------------------
        /** False to quit. */
        bool key (int k)
        {
            if (input != Input::none)
            {
                typing (k);
                return true;
            }

            switch (k)
            {
                case 'q': case 'Q':                 return false;
                case '1':                           page = Page::sessions; break;
                case '2':                           page = Page::plugins; break;
                case '3':                           page = Page::log; break;
                case Terminal::Right:               page = page == Page::sessions ? Page::plugins : Page::log; break;
                case Terminal::Left:                page = page == Page::log ? Page::plugins : Page::sessions; break;
                case Terminal::Up:                  move (-1); break;
                case Terminal::Down:                move (1); break;
                case Terminal::PageUp:              move (-std::max (1, listRows - 1)); break;
                case Terminal::PageDown:            move (std::max (1, listRows - 1)); break;
                case Terminal::Home:                move (-1000000); break;
                case Terminal::End:                 move (1000000); break;
                case Terminal::Tab:                 if (page == Page::plugins) foldersFocused = ! foldersFocused; break;
                case '/':
                    if (page != Page::sessions)
                        startInput (Input::filter, filterOf (page));
                    break;
                case Terminal::Escape:              if (page != Page::sessions) filterOf (page).clear(); break;
                default:
                    if (page == Page::plugins)
                        folderCommand (k);
                    break;
            }
            return true;
        }

        //--- the frame ---------------------------------------------------------
        std::string frame (int columns, int rows, const Style& st)
        {
            v = link.snapshot();
            noteCounters();

            std::vector<std::string> lines;
            header (lines, columns, st);

            const bool once = rows <= 0;
            const int body = once ? 1000000 : std::max (3, rows - (int) lines.size() - 1);
            std::vector<std::string> content;
            if (! v.everReceived)
            {
                Line l (columns, st);
                l.put (v.state, v.connected ? st.dim : st.warn);
                content.push_back (l.str());
            }
            else if (once)
            {
                sessions (content, columns, body, st);
                content.push_back ({});
                plugins (content, columns, body, st);
                content.push_back ({});
                logView (content, columns, 30, st);
            }
            else if (page == Page::sessions)  sessions (content, columns, body, st);
            else if (page == Page::plugins)   plugins (content, columns, body, st);
            else                              logView (content, columns, body, st);

            if (! once)
                content.resize ((std::size_t) body, "\x1b[K");
            lines.insert (lines.end(), content.begin(), content.end());
            if (! once)
                lines.push_back (footer (columns, st));

            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i)
                out += lines[i] + (i + 1 < lines.size() ? "\r\n" : "");
            return out;
        }

    private:
        enum class Input { none, filter, addFolder };

        //--- header and footer -------------------------------------------------
        void header (std::vector<std::string>& lines, int w, const Style& st)
        {
            const auto& s = v.status;
            const auto& srv = s["server"];

            Line l (w, st);
            l.put ("fxmenet-top", st.title);
            if (v.everReceived)
                l.put ("  " + srv["name"].toString(), st.bold)
                 .put ("  " + srv["version"].toString() + ", " + srv["os"].toString() + ", up "
                       + duration ((juce::int64) srv["seconds"]), st.dim);
            l.put ("  " + link.where(), st.dim);
            if (! v.connected)
                l.put ("  " + v.state, st.warn);
            else
                l.put (v.admin ? "  admin" : "  read only", v.admin ? st.good : st.dim);
            lines.push_back (l.str());

            if (! v.everReceived)
            {
                lines.push_back ({});
                return;
            }

            // CPU: the machine, each core, the server process; memory; network.
            const auto& load = s["load"];
            Line c (w, st);
            const double total = (double) load["total"];
            c.put ("CPU ", st.title);
            if (total >= 0.0)
                c.put (bar (total / 100.0, 12), total > 85.0 ? st.warn : st.good).put (" " + fixed (total, 0) + "%");
            const auto& cores = arrayOf (load["cores"]);
            if (! cores.isEmpty())
            {
                c.put ("  cores", st.dim);
                for (auto& x : cores)
                {
                    const double p = (double) x;
                    c.put (" " + fixed (p, 0), p > 85.0 ? st.warn : nullptr);
                }
            }
            c.put ("  server ", st.dim).put (fixed ((double) load["process"], 0) + "%", st.bold);
            if ((juce::int64) load["resident"] > 0)
                c.put ("  ").put ("MEM ", st.title).put (byteSize ((double) (juce::int64) load["resident"]));
            const auto& net = s["network"];
            c.put ("  ").put ("NET ", st.title).put ("in " + bitRate ((double) net["inBytesPerSecond"]))
             .put ("  out " + bitRate ((double) net["outBytesPerSecond"]));
            lines.push_back (c.str());

            Line t (w, st);
            const auto tab = [&] (Page p, const std::string& name)
            {
                t.put (" " + name + " ", page == p ? st.inverse : st.dim).put (" ");
            };
            const auto nSessions = arrayOf (s["sessions"]).size();
            tab (Page::sessions, "1 Sessions (" + std::to_string (nSessions) + ")");
            tab (Page::plugins, "2 Plugins (" + std::to_string ((int) s["catalog"]["count"]) + ")");
            tab (Page::log, "3 Log");
            if ((bool) s["catalog"]["scanning"])
                t.put ("  scanning plugins", st.accent);
            lines.push_back (t.str());
        }

        std::string footer (int w, const Style& st)
        {
            Line f (w, st);
            if (input != Input::none)
            {
                f.put (input == Input::filter ? "Filter: " : "Add folder (absolute path on the server): ", st.accent)
                 .put (typed).put ("█").put ("   Enter", st.bold).put (" confirm  ", st.dim).put ("Esc", st.bold).put (" cancel", st.dim);
                return f.str();
            }
            if (v.message.isNotEmpty() && Clock::now() - v.messageAt < std::chrono::seconds (6))
            {
                f.put (v.message, st.accent);
                return f.str();
            }

            f.put ("q", st.bold).put (" quit  ", st.dim).put ("1 2 3 ←→", st.bold).put (" view  ", st.dim)
             .put ("↑↓ PgUp PgDn", st.bold).put (" move  ", st.dim);
            if (page == Page::plugins)
                f.put ("Tab", st.bold).put (" folders/plugins  ", st.dim)
                 .put ("a", st.bold).put (" add  ", st.dim).put ("d", st.bold).put (" remove  ", st.dim)
                 .put ("x", st.bold).put (" default folders  ", st.dim).put ("r R", st.bold).put (" rescan, full  ", st.dim);
            if (page != Page::sessions)
                f.put ("/", st.bold).put (" filter  ", st.dim).put ("Esc", st.bold).put (" clear", st.dim);
            return f.str();
        }

        //--- sessions ----------------------------------------------------------
        void sessions (std::vector<std::string>& lines, int w, int rows, const Style& st)
        {
            const auto& list = arrayOf (v.status["sessions"]);
            if (list.isEmpty())
            {
                Line l (w, st);
                l.put ("No session: no FxmeNet plugin is connected to this server.", st.dim);
                lines.push_back (l.str());
                return;
            }

            Line h (w, st);
            h.put ("  ID  CLIENT", st.title).pad (30).put ("FROM", st.title).pad (47).put ("FORMAT", st.title).pad (67)
             .put ("CHAIN", st.title).pad (74).put ("LOAD, MAX 1 S", st.title).pad (95).put ("IN", st.title).pad (106)
             .put ("OUT", st.title).pad (117).put ("LATE  LOST  STALE", st.title);
            lines.push_back (h.str());

            selected = std::clamp (selected, 0, list.size() - 1);
            for (int i = 0; i < list.size(); ++i)
            {
                const auto& s = list.getReference (i);
                const bool on = i == selected;
                Line l (w, st);
                l.put (on ? "› " : "  ", st.accent)
                 .put (std::to_string ((int) s["id"])).pad (6)
                 .put (s["client"].toString() + " " + s["os"].toString(), on ? st.accent : st.bold).pad (29).put (" ")
                 .put (s["peer"].toString()).pad (47);

                if ((bool) s["orphan"])
                    l.put ("disconnected, kept for a resume", st.warn);
                else if (! (bool) s["configured"])
                    l.put ("connected, not configured", st.dim);
                else
                {
                    const double rate = (double) s["sampleRate"];
                    const int block = (int) s["blockSize"];
                    const bool wholeKilohertz = std::fmod (rate, 1000.0) < 0.5;
                    l.put (fixed (rate / 1000.0, wholeKilohertz ? 0 : 1) + " kHz "
                           + std::to_string (block)).pad (67)
                     .put (std::to_string (arrayOf (s["slots"]).size())).pad (74);
                    loadBar (l, s, 12, st);
                    l.pad (95).put (bitRate ((double) s["inBytesPerSecond"])).pad (106)
                     .put (bitRate ((double) s["outBytesPerSecond"])).pad (117);
                    counter (l, s, "overOneBlock", 6, st);
                    counter (l, s, "lost", 6, st);
                    counter (l, s, "stale", 6, st);
                }
                lines.push_back (l.str());
            }

            lines.push_back ({});
            sessionDetail (lines, w, rows - (int) lines.size(), list.getReference (selected), st);
            listRows = std::max (1, list.size());
        }

        /** The worst processing time of the last second, against the block's duration. */
        void loadBar (Line& l, const juce::var& s, int width, const Style& st)
        {
            const double blockMicros = 1.0e6 * (int) s["blockSize"] / std::max (1.0, (double) s["sampleRate"]);
            const double fraction = (int) s["processMaxMicros"] / blockMicros;
            l.put (bar (fraction, width), fraction > 0.8 ? st.warn : fraction > 0.5 ? st.accent : st.good)
             .put (" " + fixed (100.0 * fraction, 0) + "%");
        }

        /** A counter, highlighted for 10 s after it moved. */
        void counter (Line& l, const juce::var& s, const char* name, int width, const Style& st)
        {
            const auto value = (juce::int64) s["counters"][name];
            const auto key = std::to_string ((int) s["id"]) + name;
            const auto it = changes.find (key);
            const bool recent = it != changes.end() && Clock::now() - it->second.at < std::chrono::seconds (10)
                             && it->second.seenTwice;
            const auto text = std::to_string (value);
            l.put (text, recent ? st.warn : value > 0 ? nullptr : st.dim)
             .put (std::string ((std::size_t) std::max (1, width - (int) text.size()), ' '));
        }

        void sessionDetail (std::vector<std::string>& lines, int w, int rows, const juce::var& s, const Style& st)
        {
            Line t (w, st);
            t.put ("SESSION " + std::to_string ((int) s["id"]), st.title)
             .put ("  " + s["client"].toString() + " on " + s["os"].toString() + " from " + s["peer"].toString()
                   + ", open " + duration ((juce::int64) s["seconds"]), st.dim);
            lines.push_back (t.str());

            if ((bool) s["configured"])
            {
                const double blockMicros = 1.0e6 * (int) s["blockSize"] / std::max (1.0, (double) s["sampleRate"]);
                Line f (w, st);
                f.put ("  " + fixed ((double) s["sampleRate"], 0) + " Hz, blocks of " + std::to_string ((int) s["blockSize"])
                       + " (" + fixed (blockMicros / 1000.0, 1) + " ms), " + std::to_string ((int) s["inputs"]) + " in, "
                       + std::to_string ((int) s["outputs"]) + " out, ")
                 .put ((bool) s["realtime"] ? "real-time audio thread" : "ordinary audio thread (no real-time priority)",
                       (bool) s["realtime"] ? st.good : st.warn)
                 .put (", " + fixed ((double) s["blocksPerSecond"], 0) + " blocks/s, chain latency "
                       + std::to_string ((int) s["chainLatency"]));
                lines.push_back (f.str());

                Line p (w, st);
                p.put ("  process ", st.dim).put (std::to_string ((int) s["processMicros"]) + " us")
                 .put ("  max 1 s ", st.dim).put (std::to_string ((int) s["processMaxMicros"]) + " us", st.bold)
                 .put ("  peak ", st.dim).put (std::to_string ((int) s["processPeakMicros"]) + " us")
                 .put ("  reply after max 1 s ", st.dim).put (std::to_string ((int) s["turnaroundMaxMicros"]) + " us")
                 .put ("  (block " + fixed (blockMicros, 0) + " us)", st.dim);
                lines.push_back (p.str());

                Line c (w, st);
                c.put ("  ");
                static const std::pair<const char*, const char*> shownCounters[] {
                    { "processed", "blocks" }, { "overOneBlock", "late > 1 block" }, { "overTwoBlocks", "> 2 blocks" },
                    { "lost", "lost" }, { "stale", "stale" }, { "fromCache", "from cache" }, { "queueFull", "queue full" },
                    { "sendErrors", "send errors" }, { "resets", "resets" }
                };
                for (const auto& [name, label] : shownCounters)
                {
                    c.put (std::string (label) + " ", st.dim);
                    counter (c, s, name, 0, st);
                    c.put (" ");
                }
                lines.push_back (c.str());
            }

            lines.push_back ({});
            Line ct (w, st);
            const auto& slots = arrayOf (s["slots"]);
            ct.put ("CHAIN", st.title).put ("  " + std::to_string (slots.size()) + " plugins", st.dim);
            lines.push_back (ct.str());
            int n = 1;
            for (auto& slot : slots)
            {
                Line l (w, st);
                const bool bypassed = (bool) slot["bypassed"];
                l.put ("  " + std::to_string (n++)).pad (6)
                 .put (slot["name"].toString(), bypassed ? st.dim : st.bold).pad (34)
                 .put (slot["version"].toString(), st.dim).pad (44)
                 .put (slot["vendor"].toString(), st.dim).pad (66)
                 .put ((bool) slot["fxmenet"] ? "FxmeNet" : "", st.good).pad (75);
                if ((int) slot["latency"] > 0)
                    l.put ("latency " + std::to_string ((int) slot["latency"]) + "  ");
                if (bypassed)
                    l.put ("bypassed", st.accent);
                lines.push_back (l.str());
            }

            // The rest of the screen: the latest log lines.
            const int left = rows - (int) slots.size() - 7;
            if (left > 2 && ! v.log.empty())
            {
                lines.push_back ({});
                Line lt (w, st);
                lt.put ("LOG", st.title);
                lines.push_back (lt.str());
                const auto count = std::min ((std::size_t) left - 2, v.log.size());
                for (auto i = v.log.size() - count; i < v.log.size(); ++i)
                    lines.push_back (logLine (v.log[i], w, st));
            }
        }

        //--- plugins -----------------------------------------------------------
        struct Folder
        {
            juce::String path, kind;
            bool exists = true, active = true;
        };

        std::vector<Folder> folders() const
        {
            std::vector<Folder> out;
            const auto& c = v.status["catalog"];
            const bool defaults = (bool) c["useDefaultPaths"];
            for (auto& f : arrayOf (c["defaultPaths"]))
                out.push_back ({ f["path"].toString(), "default", (bool) f["exists"], defaults });
            for (auto& f : arrayOf (c["savedPaths"]))
                out.push_back ({ f["path"].toString(), "saved", (bool) f["exists"], true });
            for (auto& f : arrayOf (c["commandLinePaths"]))
                out.push_back ({ f["path"].toString(), "this run", (bool) f["exists"], true });
            return out;
        }

        void plugins (std::vector<std::string>& lines, int w, int rows, const Style& st)
        {
            const auto& c = v.status["catalog"];
            const auto list = folders();

            Line t (w, st);
            t.put ("FOLDERS", foldersFocused ? st.accent : st.title)
             .put ((bool) c["useDefaultPaths"] ? "  default folders on" : "  default folders off", st.dim);
            if (! v.admin)
                t.put ("  read only: changes are made on the server's machine", st.dim);
            lines.push_back (t.str());

            folder = std::clamp (folder, 0, std::max (0, (int) list.size() - 1));
            for (int i = 0; i < (int) list.size(); ++i)
            {
                const auto& f = list[(std::size_t) i];
                const bool on = foldersFocused && i == folder;
                Line l (w, st);
                l.put (on ? "› " : "  ", st.accent)
                 .put (f.kind, st.dim).pad (12)
                 .put (f.path, ! f.active ? st.dim : on ? st.accent : st.bold);
                if (! f.active)
                    l.put ("  off", st.dim);
                else if (! f.exists)
                    l.put ("  missing", st.warn);
                lines.push_back (l.str());
            }
            if (list.empty())
            {
                Line l (w, st);
                l.put ("  no folder: press a to add one", st.dim);
                lines.push_back (l.str());
            }

            Line s (w, st);
            if ((bool) c["scanning"])
            {
                const int done = (int) c["done"], total = (int) c["total"];
                s.put ("SCAN ", st.title).put (bar (total > 0 ? (double) done / total : 0.0, 20), st.accent)
                 .put (" " + std::to_string (done) + "/" + std::to_string (total) + "  ")
                 .put (c["file"].toString(), st.dim);
            }
            else
            {
                s.put ("SCAN ", st.title).put (c["summary"].toString(), st.dim);
            }
            lines.push_back ({});
            lines.push_back (s.str());
            lines.push_back ({});

            // The plugins, by name, filtered.
            std::vector<const juce::var*> shown;
            int aware = 0;
            for (auto& p : v.plugins)
            {
                aware += (bool) p["fxmenet"] ? 1 : 0;
                if (matches (p["name"].toString() + " " + p["vendor"].toString(), pluginFilter))
                    shown.push_back (&p);
            }
            std::sort (shown.begin(), shown.end(), [] (const juce::var* a, const juce::var* b)
            {
                return (*a)["name"].toString().compareNatural ((*b)["name"].toString()) < 0;
            });

            Line pt (w, st);
            pt.put ("PLUGINS", foldersFocused ? st.title : st.accent)
             .put ("  " + std::to_string (v.plugins.size()) + ", " + std::to_string (aware) + " FxmeNet-aware", st.dim);
            if (pluginFilter.isNotEmpty())
                pt.put ("   filter: ", st.dim).put (pluginFilter, st.accent).put ("  " + std::to_string (shown.size()) + " shown", st.dim);
            lines.push_back (pt.str());

            const int room = std::max (1, rows - (int) lines.size());
            listRows = foldersFocused ? std::max (1, (int) list.size()) : room;
            pluginTop = std::clamp (pluginTop, 0, std::max (0, (int) shown.size() - room));
            for (int i = pluginTop; i < (int) shown.size() && (int) lines.size() < rows; ++i)
            {
                const auto& p = *shown[(std::size_t) i];
                Line l (w, st);
                l.put ("  ").put (p["name"].toString(), st.bold).pad (36)
                 .put (p["version"].toString(), st.dim).pad (48)
                 .put (p["vendor"].toString()).pad (72)
                 .put (std::to_string ((int) p["inputs"]) + "/" + std::to_string ((int) p["outputs"]), st.dim).pad (80)
                 .put ((bool) p["fxmenet"] ? "FxmeNet" : "", st.good).pad (90)
                 .put (p["category"].toString(), st.dim);
                lines.push_back (l.str());
            }
        }

        void folderCommand (int k)
        {
            if (k != 'a' && k != 'd' && k != Terminal::Delete && k != 'x' && k != 'r' && k != 'R')
                return;
            if (! v.connected)
                return;
            if (! v.admin)
            {
                link.say ("read only: change the folders on the server's machine, or start the server with --remote-admin");
                return;
            }

            const auto& c = v.status["catalog"];
            juce::StringArray saved;
            for (auto& f : arrayOf (c["savedPaths"]))
                saved.add (f["path"].toString());
            const bool defaults = (bool) c["useDefaultPaths"];

            if (k == 'a')
                startInput (Input::addFolder, {});
            else if (k == 'x')
                link.setPaths (saved, ! defaults);
            else if (k == 'r' || k == 'R')
                link.rescan (k == 'R');
            else
            {
                const auto list = folders();
                if (! foldersFocused || ! juce::isPositiveAndBelow (folder, (int) list.size()))
                    return;
                const auto& f = list[(std::size_t) folder];
                if (f.kind == "default")
                    link.say ("a default folder: x turns them all off");
                else if (f.kind == "this run")
                    link.say ("given with --plugin-path: remove it from the server's command line");
                else
                {
                    saved.removeString (f.path);
                    link.setPaths (saved, defaults);
                }
            }
        }

        //--- log ---------------------------------------------------------------
        std::string logLine (const juce::String& text, int w, const Style& st) const
        {
            Line l (w, st);
            const bool bad = text.containsIgnoreCase ("error") || text.containsIgnoreCase ("warning")
                          || text.containsIgnoreCase ("skipped");
            l.put (text, bad ? st.warn : nullptr);
            return l.str();
        }

        void logView (std::vector<std::string>& lines, int w, int rows, const Style& st)
        {
            std::vector<const juce::String*> shown;
            for (auto& line : v.log)
                if (matches (line, logFilter))
                    shown.push_back (&line);

            Line t (w, st);
            t.put ("LOG", st.title).put ("  " + std::to_string (v.log.size()) + " lines", st.dim);
            if (logFilter.isNotEmpty())
                t.put ("   filter: ", st.dim).put (logFilter, st.accent).put ("  " + std::to_string (shown.size()) + " shown", st.dim);
            if (logBack > 0)
                t.put ("   scrolled back, End follows", st.accent);
            lines.push_back (t.str());

            const int room = std::max (1, rows - 1);
            listRows = room;
            logBack = std::clamp (logBack, 0, std::max (0, (int) shown.size() - room));
            const int end = (int) shown.size() - logBack;
            for (int i = std::max (0, end - room); i < end; ++i)
                lines.push_back (logLine (*shown[(std::size_t) i], w, st));
        }

        //--- selection and typing ----------------------------------------------
        void move (int delta)
        {
            if (page == Page::sessions)
                selected = std::max (0, selected + delta);   // clamped when drawn
            else if (page == Page::plugins)
            {
                if (foldersFocused)
                    folder = std::max (0, folder + delta);
                else
                    pluginTop = std::max (0, pluginTop + delta);
            }
            else
                logBack = std::max (0, logBack - delta);   // up: further back
        }

        juce::String& filterOf (Page p)   { return p == Page::log ? logFilter : pluginFilter; }

        void startInput (Input kind, const juce::String& initial)
        {
            input = kind;
            typed = initial.toStdString();
        }

        void typing (int k)
        {
            if (k == Terminal::Escape)
                input = Input::none;
            else if (k == Terminal::Enter)
            {
                const auto value = juce::String::fromUTF8 (typed.c_str()).trim();
                if (input == Input::filter)
                    filterOf (page) = value;
                else if (value.isNotEmpty())
                {
                    const auto& c = v.status["catalog"];
                    juce::StringArray saved;
                    for (auto& f : arrayOf (c["savedPaths"]))
                        saved.add (f["path"].toString());
                    saved.add (value);
                    link.setPaths (saved, (bool) c["useDefaultPaths"]);
                }
                input = Input::none;
            }
            else if (k == Terminal::Backspace)
            {
                // Drop the last code point: its continuation bytes, then its lead.
                while (! typed.empty() && (static_cast<unsigned char> (typed.back()) & 0xC0) == 0x80)
                    typed.pop_back();
                if (! typed.empty())
                    typed.pop_back();
            }
            else if (k >= 0x20 && k < 0x110000 && k != 0x7F)
                typed += fxmenet::top::utf8 (k);
        }

        //--- counters that moved -------------------------------------------------
        struct Change
        {
            juce::int64 value = 0;
            Clock::time_point at;
            bool seenTwice = false;   ///< not the first value seen (that one is history)
        };

        void noteCounters()
        {
            for (auto& s : arrayOf (v.status["sessions"]))
                for (auto* name : { "overOneBlock", "overTwoBlocks", "lost", "stale", "fromCache", "queueFull", "sendErrors", "resets" })
                {
                    const auto value = (juce::int64) s["counters"][name];
                    const auto key = std::to_string ((int) s["id"]) + name;
                    auto it = changes.find (key);
                    if (it == changes.end())
                        changes[key] = { value, Clock::now() - std::chrono::hours (1), false };
                    else if (it->second.value != value)
                        it->second = { value, Clock::now(), true };
                }
        }

        Link& link;
        Link::View v;
        Page page = Page::sessions;
        int selected = 0, folder = 0, pluginTop = 0, logBack = 0, listRows = 10;
        bool foldersFocused = true;
        juce::String pluginFilter, logFilter;
        Input input = Input::none;
        std::string typed;
        std::map<std::string, Change> changes;
    };

    //==========================================================================
    void splitAddress (const juce::String& address, juce::String& host, int& port)
    {
        host = address;
        // host:port, but not an IPv6 address (several colons) unless in brackets.
        if (address.startsWith ("["))
        {
            host = address.fromFirstOccurrenceOf ("[", false, false).upToFirstOccurrenceOf ("]", false, false);
            const auto rest = address.fromFirstOccurrenceOf ("]", false, false);
            if (rest.startsWith (":"))
                port = rest.substring (1).getIntValue();
        }
        else if (address.indexOfChar (':') >= 0 && address.indexOfChar (':') == address.lastIndexOfChar (':'))
        {
            host = address.upToFirstOccurrenceOf (":", false, false);
            port = address.fromFirstOccurrenceOf (":", false, false).getIntValue();
        }
    }

    int find()
    {
        const auto servers = fxmenet::discovery::discover (1500);
        if (servers.empty())
        {
            std::printf ("no FxmeNet server answered on the local network\n");
            return 1;
        }
        for (auto& a : servers)
            std::printf ("%s  %s:%d  %s  version %s, %d sessions\n", a.name.toRawUTF8(), a.host.toRawUTF8(),
                         a.controlPort, a.os.toRawUTF8(), a.version.toRawUTF8(), a.sessions);
        return 0;
    }
}

//==============================================================================
int main (int argc, char* argv[])
{
    bool once = false;
    juce::String host = "127.0.0.1";
    int port = 54322;

    for (int i = 1; i < argc; ++i)
    {
        const auto arg = juce::String (juce::CharPointer_UTF8 (argv[i]));
        if (arg == "--once")
            once = true;
        else if (arg == "--find")
            return find();
        else if (! arg.startsWith ("-"))
            splitAddress (arg, host, port);
        else
        {
            std::fprintf (stderr, "usage: fxmenet-top [HOST[:PORT]] [--once]   (default 127.0.0.1:54322)\n"
                                  "       fxmenet-top --find                  (the servers on the local network)\n");
            return 2;
        }
    }

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    Link link (host, port);
    Top top (link);

    if (once)
    {
        // The first status, or what went wrong, within 5 s.
        const auto until = Clock::now() + std::chrono::seconds (5);
        while (Clock::now() < until && ! link.snapshot().everReceived && ! stop)
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        const auto none = Style::none();
        auto frame = top.frame (160, 0, none);
        for (std::size_t p; (p = frame.find ("\x1b[K")) != std::string::npos;)
            frame.erase (p, 3);
        for (std::size_t p; (p = frame.find ('\r')) != std::string::npos;)
            frame.erase (p, 1);
        std::printf ("%s\n", frame.c_str());
        return link.snapshot().everReceived ? 0 : 1;
    }

    const Style style = std::getenv ("NO_COLOR") == nullptr ? Style {} : Style::none();
    Terminal term;
    auto nextFrame = Clock::now();
    while (! stop)
    {
        bool redraw = false;
        while (const int k = term.key())
        {
            redraw = true;
            if (! top.key (k))
                stop = true;
        }
        if (stop)
            break;

        if (redraw || Clock::now() >= nextFrame)
        {
            int columns = 0, rows = 0;
            Terminal::size (columns, rows);
            term.draw (top.frame (columns, rows, style));
            nextFrame = Clock::now() + std::chrono::milliseconds (250);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return 0;
}
