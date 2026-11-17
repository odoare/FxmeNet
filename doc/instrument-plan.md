# Instruments on the server: design and plan (draft)

Written 2026-11-17, at the user's request: load instruments (synthesizers,
samplers, JSFX synths) on the server, played from the DAW's MIDI. This is
the "MIDI" step announced after the JSFX work (`doc/jsfx-plan.md`).

## What already exists

MIDI is not new to FxmeNet; instruments are what is missing.

- **Wire.** The audio request carries the block's MIDI (protocol.md 5.2:
  `midiBytes`, then events of `u16 sampleOffset, u8 size, bytes`), at most
  `wire::maxMidiBytes` (8192) per block. The reply has the same section
  (5.3).
- **Client.** The plugin is built with `NEEDS_MIDI_INPUT`; `processBlock`
  takes the DAW's events (at most 2048 per callback), the stream splits
  them into N-sample blocks, and `AudioLink::send` packs them.
- **Server.** `AudioWorker` unpacks them into a `juce::MidiBuffer` and the
  chain passes it through the slots in order (routing.md: MIDI is
  chain-wide). The MIDI left after the last slot goes back in the reply;
  the client does not use it (no MIDI output).
- **Filtered out.** The server's catalog skips `isInstrument` plugins
  (`PluginCatalog.cpp`, "FxmeNet hosts effects"), and so does the client's
  local scan (`LocalCatalog.cpp`). `JsfxInstance` reports itself as an
  effect.

So an effect that listens to MIDI (a MIDI-triggered gate, a vocoder) can
already work. An instrument is refused only by the two filters, and by
the DAWs, which will not put an effect where an instrument goes.

## Recommendation: two plugins from one code base

**FxmeNet** (as now) and **FxmeNet Instrument**, built from the same
sources, as two targets of `Plugin/CMakeLists.txt`.

Why two:

- **DAWs place plugins by type, and one binary declares one type.** Logic
  only offers music devices (AU type `aumu`) on instrument tracks, and an
  `aufx` there gets no MIDI. Ableton Live, Cubase, Studio One and Bitwig
  list instruments and audio effects in separate places, from the VST3
  category (`Instrument` or `Fx`) and from whether there is a main input.
  REAPER does not care, which is why the current build can already be
  tried as an instrument there and nowhere else.
- **The buses differ.** An instrument has no main input (an optional
  sidechain at most); an effect needs one. A plugin that has both is
  shown as an effect by most hosts.
- **The lists differ.** In the instrument, the server's instruments come
  first (an instrument, then effects after it in the chain); in the
  effect, instruments are of no use and stay hidden.

Why one code base: everything else is the same (the session, the stream,
the chain, routing, mapping, shadows, telemetry, JSFX). A compile
definition (`FXMENET_INSTRUMENT=1`) changes only the declaration, the
buses and the list. The shared sources are compiled once per target
(JUCE's plugin definitions are per target); the build time of the plugin
doubles, nothing else does.

Rejected alternative: one plugin with a mode switch. Its type is fixed
when the DAW scans it, so the switch could not move it into the
instrument lists; it would only work in REAPER.

## The new plugin

- `juce_add_plugin(FxmeNetInstrument ...)`: `PRODUCT_NAME "FxmeNet
  Instrument"`, `PLUGIN_CODE FnIn`, `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT
  TRUE`, `VST3_CATEGORIES Instrument Network`, its own `BUNDLE_ID`. AU
  type `aumu` follows from `IS_SYNTH`.
- Buses: a stereo main output (up to `wire::maxChannels`, as now), no main
  input. The session still opens with the chain's usual input count; the
  requests carry an empty `inputMask`, so the chain is fed silence and
  a request is a few bytes of header and MIDI (no protocol change).
- The list: instruments first (tagged as now, T0/T1/T2), then effects, then
  JSFX scripts. JSFX: every script is listed (synths are scripts with
  `in_pin:none` and `midirecv`; nothing marks them as instruments).
- An on-screen keyboard in the editor (`juce::MidiKeyboardComponent`), its
  notes merged into the DAW's MIDI: a way to hear an instrument without a
  MIDI track, and a test tool. Optional, small.
- Its state, presets and project format are FxmeNet's; a state saved by
  one variant loads in the other (only the buses differ).
- fxmenet-top shows which variant opened each session (a field in the
  session's hello, informational).

## MIDI must not get lost

Losing audio costs a concealed block; losing MIDI can leave a note
sounding forever (a lost note-off) or a sustain pedal held. Today a lost
request (real time, protocol.md 5.4) is not resent: block k+1 is
processed, k never is, and its MIDI is gone. Measures, in order of
importance:

1. **Redundancy.** Each request also carries the MIDI of the previous R
   blocks (R = D by default, at most 8), each with its block index: a
   "MIDI history" section after the block's own events. The server keeps
   the index of the last block it processed; when it skips blocks, it
   takes their events from the history of the request that arrived and
   plays them at offset 0 of the block it processes:
   - note-offs, sustain and other controller values, pitch bend, program
     changes: always (the state must end up right);
   - note-ons: only if their block is at most one block late, else
     dropped (a note starting 20 ms late sounds wrong; one never starting
     is the lesser evil), along with their note-off.
   Cost: the history is only as large as the MIDI played (a few bytes per
   event, nothing on blocks without MIDI). Protocol: a request flag and
   the section (protocol.md 5.2); the version moves (no compatibility with
   older servers, as agreed).
2. **All notes off at every break.** The client tracks the notes and the
   sustain it has sent (per channel). On a reset (transport jump, stop
   with "Silence in-flight audio"), on bypass, on a session rebuild after
   a reconnection, and when a slot is added, removed or reloaded, it sends
   the matching note-offs and CC 64 = 0, then CC 123 (all notes off) on
   the channels used. The server does the same on its side for a session
   whose client went silent (time-out) and before removing a slot.
3. **No silent overflow.** The 8192 bytes of a block hold about 2000
   short events, more than any keyboard, but a dense controller stream or
   SysEx can fill them. Events that do not fit move to the next block
   instead of being dropped (`getMidiDropped` already counts what is
   lost); SysEx longer than 255 bytes (the `u8 size`) is refused and
   counted, as now.

## Latency

The chain's latency (N x D, plus what the plugins add) is reported to the
DAW, which compensates it on playback: a MIDI track plays in time, as with
a lookahead plugin. Playing live through the instrument is late by that
latency, plus the audio interface's; FxmeNet is not for live playing
(README). To write in the manual: record with the instrument's local
version or a light local synth, play back through FxmeNet; or lower N and
D while recording (N = 64, D = 2 at 48 kHz is under 3 ms on a good
network).

## Editors (shadows) for instruments

Tiers 1 and 2 work as for effects, with three points particular to
instruments:

- **Their own on-screen keyboards** (most synths have one) play the
  mirror, which never processes: no sound. Same nature as plugdata's
  messages (doc/bugs.md). Not fixable in general; for FX-Mechanics
  instruments, the event channel of todo item 17b would carry them. The
  FxmeNet Instrument's own keyboard covers the need.
- **Samplers and their content.** The samples live on the server; the
  state names them by path. A mirror loading that state on the client
  either does not find them or loads the client's copies, which may take
  gigabytes of memory for an editor. A host cannot ask a plugin for an
  editor without its content, nor tell a sampler from a synth. Plan:
  document it (install the same libraries at the same paths, or use the
  generic controls), and check with sfizz and Decent Sampler what happens.
- **Large states.** An instrument with embedded samples can have a state
  of tens of MB, sent whole at each non-parameter change by tier 1
  polling (`Shadow::takeStateChange`). The control frame takes 64 MB;
  above that the change is refused and logged. To measure, then decide.

## Later

- **Several outputs.** Drum machines and multitimbral samplers have
  separate outputs. The instrument would declare extra output buses (8
  stereo pairs, say), and the routing patchbay would map the chain's
  outputs to them. The wire already carries 64 channels.
- **MIDI out to the DAW.** The reply already carries the chain's MIDI;
  an arpeggiator or a MIDI effect on the server could feed the DAW
  (`NEEDS_MIDI_OUTPUT` in the instrument). Hosts differ much in how they
  route a plugin's MIDI output.
- **MIDI routing in the patchbay** (routing.md: chain-wide for now), for
  layering two instruments on different channels.
- **MPE and MIDI 2.0.** MPE is ordinary MIDI 1.0 and passes as is
  (a test with Surge XT); MIDI 2.0 is not in JUCE's plugin API yet.

## Phases

Each ends usable; the user tests and commits between them.

1. **The second plugin.** The CMake target, the definition, the buses, the
   lists (the server and the client stop filtering instruments out; the
   catalog entry says which are instruments, `PluginEntry::isInstrument`,
   protocol.md 4). Check in REAPER, then in a DAW that sorts by type
   (Bitwig, Ableton or Logic): FxmeNet Instrument on an instrument track,
   Surge XT or Dexed on the server, a MIDI clip plays, latency
   compensated, offline render identical.
2. **MIDI safety.** All notes off at every break (client and server), the
   overflow carried to the next block. Check: stop in the middle of held
   notes, jump, bypass, remove the slot, pull the network cable: nothing
   keeps sounding.
3. **MIDI redundancy.** The history section, the server's late events.
   Check with induced loss (fxmenet-wavclient with a drop rate, a MIDI
   file in, notes counted at the output): no stuck note at 5 % loss.
4. **Editor and keyboard.** The on-screen keyboard; the session's variant
   in fxmenet-top; the manual's chapter (playing, recording, samplers,
   latency); the README's tested-plugins list gets instruments.
5. **Later items** as needed (outputs, MIDI out, MIDI routing).

## Test instruments

Free and on the three systems where possible: Surge XT (synth, MPE,
large preset state), Dexed (FM, SysEx), Vital (wavetable, big editor),
sfizz and Decent Sampler (samplers, content on disk), a JSFX synth
(REAPER's stock `synthesis` scripts), and an FX-Mechanics instrument when
there is one (tier 2).

## Open questions for the user

- The second plugin's name: "FxmeNet Instrument", "FxmeNet Inst", or
  "FxmeNet Synth"?
- Should the effect variant keep listing nothing but effects, or also
  instruments (useful in REAPER, where an effect can sit on a MIDI track)?
- Default R for the MIDI history: D (follows the latency budget) or a
  fixed 4?
