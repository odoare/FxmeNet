I am planning to develop a system `FxmeNet` to host my Juce audio plugins in a separate server.

FxmeNet is a client-server distributed system that offloads real-time Fx-Mechanics plugins (Juce) audio effects processing from a digital audio workstation (DAW) to a remote dedicated DSP server over a local network. The system consists of: 1. FxmeNet Server (fxmenet-server): A high-performance, headless network daemon hosting the engine, executing chains of Fx-Mechanics plugins, streaming audio, MIDI, and necessary control values for the display on the client side. 2. FxmeNet Plugin (FxmeNetPlugin): A VST3/CLAP/AU audio plugin built with JUCE (located at ~/src/JUCE) running in the user’s DAW, that holds plugins. It handles audio I/O streaming, network discovery, graphical parameter editing, parameter automation, plugin chain editing, and graphical monitoring based on the control values sent by the server, as written in the PluginEditor class of the plugin, so that the plugin effect has the same look and feel, wether its audio is computed locally or on the server.

The idea is a fork of a design idea I had before called JesuNet, a JesuSonic JSFX host, not implemented at all for now. The final goal is to include the JesuNet design to include JSFX into FxmeNet, but I prefer to work on the FxmeNet Fx-Mechanics plugins side before.

My idea would be to propose a minimum set of classes to allow the conversion of any VST3/CLAP/AU plugin to FxmeNet to allow the FxmeNet server to be in between the AudioProcessor and the AudioEditor, communicate with the processor on the server side, and with the editor on the client side. That would probably consist of exposing a function to get all the necessary control rate data of the plugin (server side), and a function to set these values in the PluginEditor (for the client to draw vu-meters, etc.). The produced plugin (.so .dll...) would continue to be compatible with standard DAWs, but when put on in the search path of fxmenet-server (server machine), and the client plugin FxmeNetPlugin (client machine, in DAW), the client plugin would allow the user to implement a chain of these plugins, whose audio calculation would run entirely in the server.

I want you to chalenge this idea, and also chalenge the idea of using a VST/CLAP/AU effect without additionnal modifications. Would it be possible?

The base proof of concept would be done with few FxmeFX plugins (eg. eq,comp). The necessary classes for the development of the plugins would be put in the FxmeTools library, as usual, so that other plugins would benefit of this. We should also develop, inside the FxmeNet project a very basic plugin (gain/pan + stereo meter plugin) so that it gives a basic howto.

The code modifications for a Juce plugin using the classic pair PluginProcessor/PluginEditor should be minimal, with the help of the new classes, as simple as possible, with a clear API and well documented.

I think most of the ideas in plan.md in the JesuNet folder can be reused, except the @gfx part, which would now be replaced by control values communications. An we should add a plugin discovery on the server and the client.

Interesting features to consider: A plugin present in the server (a .so/.dll/.dylib file), but not present in the client machine could be downloaded to a configuration directory of the FxmeNet plugin, so that the server could distribute the plugins if it is not found in the search path for plugins on the client side (vst/clap/etc). The same way, a compatible plugin installed on the client machine, but not on the server could be sent to the server, even installed permanently (functionnality optionnally de-activated).

Once this works, we could plan to include jsfx, but I think we could do the same as for the FxmeNet compatible plugins: transfer only the necessary data at graphics rate and compute the graphics on the client side. Here also the server can have plugins in its path, but the client can send the jsfx text script to the server when it is necessary. It is a small amount of data to be sent.

FxmeNet should be multiplatform (Win/Lin/Mac) and a server on one OS can be connected a client on any OS. We should be able to run the server on a headless, non X, linux computer.

Please propose a plan for the development of this tool.
