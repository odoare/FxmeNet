/*
  ------------------------------------------------------------------------------
    ServerConfig.h

    fxmenet-server's settings, from the command line:

        fxmenet-server [--name NAME] [--control-port N] [--udp-port N]
                       [--discovery-port N] [--no-discovery]
                       [--plugin-path DIR]... [--no-default-paths]

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

namespace fxmenet::server
{

struct ServerConfig
{
    juce::String name = juce::SystemStats::getComputerName();
    int controlPort = 54322;
    int udpPort = 54321;
    int discoveryPort = 54320;
    bool discovery = true;
    juce::StringArray pluginPaths;      ///< searched in addition to the VST3 defaults
    bool useDefaultPaths = true;

    /** Seconds a session survives its control connection dropping, so a
        client can resume it (protocol.md section 8). */
    int orphanGraceSeconds = 10;

    /** Parses the arguments; on error returns false with a message. */
    bool parse (const juce::StringArray& args, juce::String& error)
    {
        for (int i = 0; i < args.size(); ++i)
        {
            const auto& a = args[i];
            const bool hasValue = i + 1 < args.size();

            if (a == "--name" && hasValue)                 name = args[++i];
            else if (a == "--control-port" && hasValue)    controlPort = args[++i].getIntValue();
            else if (a == "--udp-port" && hasValue)        udpPort = args[++i].getIntValue();
            else if (a == "--discovery-port" && hasValue)  discoveryPort = args[++i].getIntValue();
            else if (a == "--no-discovery")                discovery = false;
            else if (a == "--plugin-path" && hasValue)     pluginPaths.add (args[++i]);
            else if (a == "--no-default-paths")            useDefaultPaths = false;
            else
            {
                error = "unknown or incomplete argument: " + a;
                return false;
            }
        }

        if (controlPort <= 0 || controlPort > 65535 || udpPort <= 0 || udpPort > 65535
            || discoveryPort <= 0 || discoveryPort > 65535)
        {
            error = "ports must be between 1 and 65535";
            return false;
        }
        return true;
    }

    static juce::String usage()
    {
        return "usage: fxmenet-server [--name NAME] [--control-port N (54322)] [--udp-port N (54321)]\n"
               "                      [--discovery-port N (54320)] [--no-discovery]\n"
               "                      [--plugin-path DIR]... [--no-default-paths]";
    }
};

} // namespace fxmenet::server
