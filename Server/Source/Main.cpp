/*
  ------------------------------------------------------------------------------
    Main.cpp  (fxmenet-server)

    Parses the options, scans the plugins, starts the server and runs the
    JUCE message loop on the main thread until SIGINT or SIGTERM. The loop
    is not optional: plugins rely on it for timers and asynchronous updates
    (AmbiRR2 pushes its parameters to its engine from a timer), and every
    plugin instance is created, configured and destroyed on it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Server.h"

#include <csignal>
#include <iostream>

namespace
{
    std::atomic<bool> quitRequested { false };

    extern "C" void onSignal (int)   { quitRequested = true; }

    /** Stops the message loop once a signal asked for it (a signal handler
        may only set a flag). */
    class QuitWatcher : private juce::Timer
    {
    public:
        QuitWatcher()   { startTimerHz (10); }

    private:
        void timerCallback() override
        {
            if (quitRequested)
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();
            }
        }
    };
}

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::CharPointer_UTF8 (argv[i]));

    // The child process of an out-of-process plugin scan (PluginCatalog).
    if (args.size() == 2 && args[0] == "--scan-plugin")
    {
        juce::MessageManager::getInstance();
        const int code = fxmenet::server::PluginCatalog::scanOnePlugin (args[1]);
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
        return code;
    }

    fxmenet::server::ServerConfig config;
    juce::String error;
    if (args.contains ("--help") || ! config.parse (args, error))
    {
        if (error.isNotEmpty())
            std::cerr << error << "\n";
        std::cerr << fxmenet::server::ServerConfig::usage() << std::endl;
        return error.isEmpty() ? 0 : 2;
    }

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    juce::MessageManager::getInstance();   // the main thread is the message thread
    int result = 0;
    {
        fxmenet::server::Server server (config);
        if (server.start())
        {
            QuitWatcher watcher;
            juce::MessageManager::getInstance()->runDispatchLoop();
            fxmenet::server::log ("stopping");
        }
        else
        {
            result = 1;
        }
        server.stop();
    }

    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return result;
}
