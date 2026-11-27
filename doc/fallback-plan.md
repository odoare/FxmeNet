# Local fallback: design and plan (draft)

Written 2026-11-25, for todo item 14 (user request 2026-11-03): when no
server answers, or on demand, process an instance's chain on the DAW's
computer, with the client's own copies of the plugins.

## Why

- **No server at hand.** A project made at the studio opens at home, on a
  laptop: today every FxmeNet instance plays dry, and the mix is unusable.
- **The server goes away mid-session** (restarted, network cut): today the
  instance plays dry until it reconnects, and a render made meanwhile is
  wrong without warning.
- **Offline work by choice:** editing a project on the train, rendering
  where the server is not reachable.

## What already exists

- **Local copies (shadows).** The plugin scans this computer's VST3
  plugins (`LocalCatalog`) and loads, per slot, a local copy matched by
  uid, version and hash (`Shadow.h`), prepared at the session's rate and
  block size, but only when the slot's editor is first opened, and never
  processed. An FxmeNet-aware plugin is told it is a `mirror`; the NetAbi
  role `local` (an ordinary plugin) already exists.
- **The model.** `ClientEngine` holds every slot's state, parameters,
  bypass and the routing, and rebuilds the server's chain from it after a
  reconnection. Everything a local chain needs is on the client.
- **The timing.** With no server (no link, or bypassed), `fetchBlock`
  plays the block's dry copy, delayed by the stream's latency, so timing
  never jumps (`ClientEngine.h`, Network). The local chain takes the place
  of that dry copy: same blocks, same place, so the latency reported to the
  DAW does not move.
- **Shared chain code.** The routing (`chain::RoutingPlan`) is already in
  Common, JUCE-free. The serial and routed processing, the bypass
  handling and the MIDI guard (`Chain::process`, `MidiGuard`) are in the
  server, as is `JsfxInstance`; the plugin links the JSFX engine already
  (for the display).

## Recommendation

**Three modes per instance**, saved with it: **Server** (silence when
there is no server; today it plays dry), **Auto** (the server when it answers, local
otherwise) and **Local** (always local, no connection attempted). Default
for new instances: Server, so that no plugin is loaded beyond the
editors opened without asking; Auto is one click away (and a
global default in Settings, as the other settings).

**The same chain code on both sides.** The server's processing core (the
serial and routed paths, bypass, the release of notes, per-slot latency)
moves to Common as a `ChainProcessor` over `juce::AudioPluginInstance`s;
the server's `Chain` keeps its message-thread side (adding, removing,
buses, publishing) and the client gets a second user of the core. One
behaviour, tested once.

**The local chain lives in the audio path, not beside it.** In Local, and
in Auto while the server is gone, `fetchBlock` runs block k's dry copy
through the local chain instead of returning it as is. Its MIDI is the
block's own. No second stream, no second latency.

## How it works

### One copy per slot: the editor's

No second copy: the local copy that shows a slot's editor (its shadow) is
the one that processes in local play. It is the same plugin, already
holding the slot's state and parameters, so nothing has to be copied
across at the switch and the editor shows what is heard. What changes:

- **When it is loaded.** Today a shadow is created when its slot's editor
  is first opened. In Auto and Local, every slot gets its shadow when the
  instance loads, so that the switch is instant. Cost: each plugin of the
  chain loaded on this computer even when its editor is never opened
  (memory, load time), which is why Server mode does not do it.
- **Its role.** A mirror never processes. At the switch it is released,
  told `local` (FxmeNet-aware plugins), prepared again, and processed; at
  the switch back, the reverse.
- **Who reaches it.** Shadows are message-thread objects today. The audio
  thread gets them through a published snapshot (as the server's chain
  publishes its slots), so that one removed while playing is never freed
  under it.

A slot with no usable local copy (tier 0: not installed, another
version) is **bridged** (its inputs passed to its outputs, as for a slot
missing on the server) and listed in a warning: "Playing locally without
X, Y". JSFX scripts run locally from their bundle (the slot's state holds
it), with the same engine as the server once `JsfxInstance` moves to
Common.

### Parameters and state

- **Parameters.** The pool and the editors already write into the model
  and, through `setQuietly`, into the copies. In local play the copies are
  the processors: a parameter change reaches them directly (as the DAW's
  automation reaches a plugin), on the audio thread through the same
  parameter objects.
- **State, server to local.** The shadow already has the server's state
  (it takes it when created, and parameter values since); what it may
  miss is a change made on the server side only (none today, apart from
  a plugin changing its own state while it plays). At the switch, each
  copy takes the last state the model has, when it differs (`refreshStates` keeps it current while the server
  answers; a state older than a few seconds is fetched first when
  possible). A server that disappeared without warning leaves the last
  state fetched: the plan says so in the manual (save the project while
  connected).
- **State, local to server.** At the switch back, each copy's state is
  read into the model, and the rebuild sends it (STATE_SET), as after any
  reconnection.

### Switching (Auto)

- **To local:** when the control connection is lost (no answer to pings
  for 2 s, or closed), not on concealed blocks (an ordinary network
  hiccup). Concealment covers the few blocks until the switch.
- **Back to the server:** when the session is rebuilt and its replies
  arrive in time for a few seconds (hysteresis: a flapping network does
  not switch back and forth). A setting chooses between that and asking
  ("Server is back: switch?").
- **A crossfade at each switch,** over about 50 ms, between the side
  leaving and the side arriving, both computed for those blocks; the
  local copies start cold (an empty reverb tail), which a crossfade
  softens but cannot hide.
- **Notes.** The side leaving gets all notes off (`MidiGuard`, on both
  sides once in Common), the side arriving starts silent: a note held
  across the switch is cut, not doubled.
- **Latency.** The local chain's plugin latency should equal the server's
  (same plugins, same block size). When it does not (another build, a
  plugin whose latency depends on the machine), the local output is
  delayed, or advanced by the reported amount if shorter, to match the
  value reported to the DAW; a local chain longer than the reported
  latency is shown as a warning (its output would be late by the
  difference) and the reported latency changes at the next stop.

### Rendering

Offline renders in Local, or in Auto with no server, are processed
locally and exact (no wait for replies). A render that switched side
during its course is marked in the status line, so that a wrong export
does not go unnoticed.

## User interface

- **Settings, Stream part:** the mode (Server / Auto / Local), and the
  global default for new instances.
- **Status line and server row:** "LOCAL" in a warning colour while the
  chain plays here, with the reason (no server, by choice), and the
  bridged plugins.
- **CPU:** the local chain's load in the statistics (time per block
  against the block's duration), since this is the DAW computer's CPU.
- fxmenet-top: nothing (the server does not see local play).

## Phases

Each ends usable; the user tests and commits between them.

State (2026-11-26): phase 1 written. `chain::ChainProcessor` and
`chain::MidiGuard` are in Common/FxmeNet/chain; the server's `Chain` slot
derives from `chain::ProcessorSlot`. Checked: the test suites, and
fxmenet-wavclient through two plugins in series, offline and in real time
(also with random host blocks): bit-identical. Not exercised by those
runs: the routed path (moved as is).

Phase 2 written (2026-11-26). `client::LocalProcessor` (the engine hands
it each complete block, its output played where the reply would be;
silence with no server, dry only when bypassed); the plugin's
`LocalChain` (Plugin/Source/LocalChain.h) on `ChainProcessor`, built from
the shadows (created for every slot in Local, role `local`, state and
values from the model, states polled back into the model at 5 Hz, buses
changed out of the chain first; routed when every slot's buses are known,
`ClientEngine::getLocalRouting`); a slot without a copy (missing here, a
JSFX script) bridged. Settings: Processing, Server or Local, saved with
the project; the status line says LOCAL. Checked: fxmenet-wavclient
`--local` (the engine's local play with the same core, no server) against
the reference: bit-identical, offline, real time and random host blocks.
Not tried yet in a DAW: the plugin's LocalChain, its routed path, the
switch at run time, the Settings layout (one more row). Known limits:
DAW automation reaches the local copies through the model at the 50 Hz
timer (not sample-accurate); slots have no parameter list in Local when
the project was never connected (generic controls empty, editors fine).
Fixed after the first try (2026-11-27): the local copies (and the editor
copies) were given an empty state, since `getSlotInfos` leaves the state
blobs out: they read `getSlotState` now, and a switch first stores the
leaving side's states in the model. In Local, the plugin list shows this
computer's plugins (LocalCatalog::getEntries), so that a chain can be
made without a server and switched to it later.

Phase 3 written (2026-11-27). `JsfxInstance` moved to
Common/FxmeNet/jsfx (namespace `jsfx`; its .cpp built into the server,
both plugins and fxmenet-wavclient). In Local, a JSFX slot's local copy is
a `JsfxInstance` made from the slot's state (the bundle travels in it),
wrapped in a Shadow (`Shadow::fromInstance`), so parameters, states and
the local chain treat it as any plugin; its description (units, lists,
hidden sliders, `jsfxGfx`) is the server's. Display: the mirror path is
kept and fed locally (decided in the phase): the engine routes the
display's requests (reset, subscribe, write) to the local script while
local (`ClientEngine::setLocalJsfx`), and the processor pushes the
script's frames into the display's queue at its timer
(`pumpLocalJsfx`, as the server's `pumpJsfxSync`); JsfxView is unchanged.
Checked: fxmenet-wavclient `--local --jsfx` (delay/delay then
utility/volume) gives the same samples as the same scripts on the server.
Not tried yet in a DAW: the display in Local, the switch while a display
is open.

Phase 4 written (2026-11-27). Engine: while a local processor is set, a
lost or new link no longer restarts the stream (a new link starts at the
current block, `AudioLink::startAt`); `startReturnToServer (blocks)`
processes each block here and sends it too for that many blocks, the output
crossfading from local to server per sample (a block with no reply plays
the local result alone), the last local block releasing its notes; then
`isReturnDone`. Processor: mode Auto (`Processing::automatic`, saved as
"auto", with `autoReturn`); while the server plays, every loaded slot's
copy is made in advance (`syncShadows`); `followAuto` at the timer: lost
when the session is down (not building) for 2 s, or replies stop for 2 s
while blocks are sent; then `goLocal` (the copies take the local role,
`Shadow::setLocal`, the scripts are made); back when the session has been
ready for 3 s, by itself or on "Back to the server" (`startReturn`: the
copies' states to the server, then a 50 ms crossfade; `finishReturn`:
mirrors again, the local scripts removed, the displays restarted from the
server). Checked: fxmenet-wavclient `--return-test` (local play, the
server connected a third of the way in, then the return) with
utility/volume, offline and in real time: the same samples as a run kept
local throughout (no gap, no click). Not done: the latency matching of
"Switching" (local and server chain latencies are taken as equal), the
marking of a render that switched sides (phase 5), the loss direction
tested only by reading (it relies on concealment's fade).

Added 2026-11-27 (user request): "Local copies loaded in advance"
(`preloadCopies`, saved as "autoPreload", on by default). The copies cost
memory, not processing, while the server plays; off, they are loaded only
when the server is lost (`goLocal` publishes the chain before handing it
to the engine: silence while they load, not a chain missing plugins), and
those no editor shows are dropped again after the return.

Phase 5 written (2026-11-28). The statistics line shows "local load" in
place of the round trip while the chain plays here (`LocalChain` measures
each block; the worst of the last second against a block's duration);
an export (offline render) during which Auto switched sides is marked in
the status line until the next export (`noteSwitch`); the manual
(statistics, troubleshooting: an export that switched sides) and the
README. The status line and the warnings for bridged plugins came with
phases 2 and 4. Not done: a global default mode for new instances
(decided: Server, which new instances get), and the latency matching
(local and server chain latencies are taken as equal, which they are
with the same plugins). The fallback is complete otherwise.

1. **The chain core in Common.** `ChainProcessor` (serial, routed, bypass,
   release of notes, latency per slot) and `MidiGuard` move to Common; the
   server's `Chain` uses them. No change in behaviour: the server's tests
   and a run of fxmenet-wavclient give bit-identical output.
2. **Local mode.** The mode setting (Server mode with no server now
   plays silence); in Local, copies for every slot
   (tier 0 bridged, with the warning), the role switch, processing in
   `fetchBlock`, parameters, state into the model when saving. Check in
   REAPER: a project with FxmeFX plugins in Local, no server running,
   sounds as with the server (an export compared with one made through the
   server: the same, apart from plugins with randomness).
3. **JSFX locally.** `JsfxInstance` to Common, built in the plugin; its
   `@gfx` display then reads the local instance directly instead of the
   mirror (or keeps the mirror path, fed locally: to decide in the phase).
4. **Auto mode.** Switching both ways, hysteresis, crossfade, notes,
   latency matching, the state carried across. Check: stop the server
   while playing, start it again; pull the network cable; a held note
   across the switch.
5. **Interface and docs.** The status line, the CPU figure, the warnings,
   the manual's chapter, the README.

## Decisions (user, 2026-11-26)

- **Default mode for new instances:** Server.
- **Back to the server in Auto:** configurable, automatic (after a few
  seconds of good replies) or asked.
- **Server mode with no server:** silence, not the dry signal (a dry
  track may pass unnoticed in a mix). DAW bypass stays dry. Comes with
  phase 2 (the mode setting).
- **JSFX scripts locally:** phase 3, as planned.
