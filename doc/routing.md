# Routing between plugins (proposal)

Status: agreed (2026-11-06), decisions in section 4. It replaces todo item
15 (routing of a slot's extra buses) with a more general design. All five
steps are written and checked in Reaper (A model, B protocol, C server, D
client, E view), latency compensation included (2026-11-06). Section 5 describes how the server processes a
routed chain, section 6 what the client does.

## 1. The idea

Today the chain is serial. The server runs every plugin, in order, on one
shared work buffer. A plugin processes the first channels of that buffer
(as many as it has), and any channels past its width pass through
untouched. A plugin's extra inputs (sidechain, aux) get whatever is left in
the buffer's higher channels, which is usually silence, so they are
disabled on purpose (see `Common/FxmeNet/catalog/Buses.h`). The input and
output toggle buttons choose which DAW channels are sent and received.

The proposal turns the chain into a small patchbay. The stream and
channels view shows, stacked vertically:

- the INPUTS sent to the server (FxmeNet's input channels);
- one box per plugin, in chain order, with small pins for its inputs (top)
  and outputs (bottom);
- the OUTPUTS received from the server (FxmeNet's output channels).

The user connects any output pin to any input pin below it, by clicking
and dragging from one pin to the other. Connections only go downward, so
there can be no feedback loop. By default the system connects outputs to
inputs in order, where it can. The routing is stored with the project,
and the user can change it. FxmeNet inputs and outputs that are not
connected are not active.

This allows parallel processing (dry and wet paths mixed back together),
sidechains fed from the DAW or from another plugin, and plugins with
several outputs split to different destinations.

## 2. Points to settle

### 2.1 Parallel paths need latency compensation

Once a connection can skip a plugin (INPUTS straight to plugin 3, or
plugin 1 straight to OUTPUTS), two paths with different latencies can meet
at the same input pin. Summed like that they comb-filter: parallel
compression, for instance, would sound wrong.

So the server has to compute, for each plugin, when its input arrives,
delay the shorter paths to match the longest one, and report the longest
path from INPUTS to OUTPUTS as the latency. Today the latency is just the
sum along the chain. This is the main hidden cost of the idea, and it is
required.

### 2.2 Pins belong to buses

Plugins expose buses, not loose channels: a main input and output, often a
sidechain, sometimes aux inputs or several outputs. Each bus has a name and
a number of channels. The boxes should group pins by bus, with the bus
name ("Main", "Sidechain", "Out 3-4").

A bus other than the main ones is enabled only when at least one of its
pins is connected. An enabled bus with nothing on it is exactly the
problem Buses.h works around (Dynette very loud with a silent sidechain,
TAL Reverb 4 sounding different). Connecting or disconnecting such a bus
changes the plugin's bus layout, which means preparing that plugin again,
on the server and in the client's local copy (so that its editor shows
the same layout).

To keep stereo work quick, dragging from a bus label (rather than a pin)
connects all its channels in order to the bus or pins it is dropped on.
Otherwise every stereo link takes two drags, and wide plugins quickly
become a tangle.

### 2.3 The default routing reproduces today's behaviour

"Outputs to inputs in order, where possible" can be read in two ways. With
a plugin that has four outputs, followed by one with two inputs, outputs 3
and 4 would simply go nowhere. Today they pass through the narrower plugin
and reach the next one that has channels 3 and 4, or the outputs.

The default rule, stated from the inputs: each input channel c of a box's
main bus is fed by the nearest box above whose main output bus has a
channel c (INPUTS counting as the top box). This is exactly what the
shared work buffer does today:

- a plugin narrower than the buffer lets the other channels through to
  the next box that has them;
- a mono-to-stereo plugin (one input, two outputs) overwrites channel 2,
  so the box below gets its second output, not the DAW's channel 2 (which
  is then not sent at all);
- a stereo-to-mono plugin lets channel 2 go around it.

(Stated from the outputs, "each output goes to the next box below with
that channel", the mono-to-stereo case would sum the plugin's second output
with the DAW's channel 2, which is not what happens today.)

With that rule an existing project, loaded with the default routing,
sounds as it does today. Only aux and sidechain buses start unconnected
(and so disabled, as today). One difference, an improvement: a channel
that goes around a plugin is now delayed by that plugin's latency (2.1),
where today it arrives early.

Two switches (added 2026-11-06, "ROUTING DEFAULTS" in the stream and
channels tab, saved with the project) change the default rule; connections
set by hand are never affected:

- **Channels go around plugins that lack them** (on by default, the rule
  above). Off: a box is fed only by the box right above it, so channels no
  plugin processes (with 8 DAW channels and stereo plugins, channels 3 to
  8) do not pass around the plugins to the outputs: they are silent.
- **Connect extra outputs** (off by default). On: a plugin's outputs count
  across all its buses, its aux outputs continuing its main ones (2 main
  and 2 extra outputs feed channels 1 to 4 below), and that bus is enabled
  on the server. Extra inputs (sidechains) are still never connected by
  default: enabling a sidechain with nothing meaningful on it changes what
  many plugins do (2.2), while enabling an extra output does not.

### 2.4 The routing has to survive chain edits

The routing is stored as the user leaves it, but the chain keeps changing.
Each input pin is either automatic (fed by the default rule) or manual (fed
by what the user chose, possibly nothing). A pin becomes manual when the
user connects to it or disconnects from it; connecting to an automatic pin
keeps its current source and adds the new one (they are summed). Pins left
automatic simply follow the default rule in the new chain, so an inserted
plugin is spliced into the default lines, and a removed one is bridged,
with nothing to store. For manual pins:

- **A plugin removed:** a source from it is replaced by what fed the same
  channel of its input (as if it were bypassed for good), or dropped when
  it had no such input.
- **A plugin moved:** sources that are no longer above the pin are
  dropped.
- **A plugin whose channel count changed** (reloaded, another version):
  pins and sources that no longer exist are dropped.
- **A manual pin left without any source by an edit** (not by the user)
  goes back to automatic, so the signal keeps flowing. A pin the user
  emptied stays empty.

A "Reset to default routing" button makes every pin automatic again.

### 2.5 Fan-out and fan-in

- One output connected to several inputs: each gets a copy. Free.
- Several outputs connected to one input: summed, as in a DAW. This is how
  parallel paths are mixed back together. (The alternative, one source per
  input, would need a mixer plugin for every parallel path.)

### 2.6 Unconnected inputs and outputs

- An unconnected FxmeNet input is not sent to the server, which saves
  bandwidth. The routing replaces the current input toggle buttons: one
  source of truth rather than two controls that can disagree.
- An unconnected FxmeNet output is silent (proposal), and is not sent by
  the server. This replaces the output toggle buttons.

### 2.7 Out of scope for now

- MIDI stays chain-wide: every plugin receives the MIDI, as now. Routing
  MIDI could come later on the same view.
- No gain on connections (a utility plugin does that).
- A bypassed plugin keeps its pins and passes each input channel to the
  output channel with the same number, as JUCE's bypass does.

### 2.8 Compatibility

An older server cannot route. The client asks for a "routing" capability
in the WELCOME exchange; without it the routing view is hidden and the
chain stays serial, as today. Projects saved before this change load with
the default routing (2.3), so they sound the same.

## 3. Plan

Five commits, each one testable on its own.

### A. The routing model

(Written.) JUCE-free, in `Common/FxmeNet/chain/Routing.h`, with its tests
in `Common/tests/RoutingTests.cpp` (target FxmeNetRoutingTests). It
holds:

- the boxes (INPUTS, one per slot, OUTPUTS), their buses and pins;
- the connections, and their validation (downward only, existing pins);
- the default rule (2.3), and the rules for chain edits (2.4);
- what is derived from the graph: the FxmeNet inputs and outputs in use,
  the aux buses to enable, the delay to add on each connection, and the
  total latency (2.1).

This is where the subtle logic lives, so it gets the most tests, all
without audio.

### B. Protocol

(Written; `doc/protocol.md` sections 4.5 and 4.6.)

- SLOT_READY carries each plugin's buses: name, input or output, channel
  count, main or aux.
- A new ROUTING message carries the whole graph, by slotId. The client sends
  it after any change, and again when a session resumes.
- The "routing" capability (2.8), defined but not advertised yet: the
  server stores the ROUTING it receives and acknowledges it, and will
  advertise the capability in step C, once it applies it.
- `doc/protocol.md` updated.

### C. Server

(Written; details in section 5.)

- Each plugin gets its own buffer instead of the shared work buffer. For
  each plugin in order: gather its inputs (copy or sum, through the
  compensation delays), process, keep its outputs for the boxes below.
- The graph is swapped in with the chain snapshot, as the chain is today,
  so the audio thread takes no lock.
- Aux buses enabled or disabled as the routing says, preparing again only
  the plugin concerned.
- The latency taken from the longest path.
- An engine test with test processors that only add a delay or a gain,
  checking sums and alignment across parallel paths (in
  `Common/tests/RoutingTests.cpp`, with the model's tests).

### D. Client engine and state

(Written; details in section 6.)

- The routing lives in the client's model, is saved in the project, and is
  sent on connect and on resume.
- The input and output masks are derived from it (per block, without a
  reconfiguration: 6.4).
- Chain edits (insert, remove, move) apply the rules of 2.4.
- The local copies of the plugins (shadows) get the same bus layout as on
  the server.
- Older projects load with the default routing.

### E. The view

(Written, `Plugin/Source/RoutingView.cpp`.) In a tab of its own,
"Routing" (since 2026-11-10; it shared "Stream & channels" before): the
routing (scrollable, the whole tab but one row), Reset routing above it,
and the two routing defaults below. Block size, network budget, latency,
the transport options and the local plugins are in the "Settings" tab. The input and output toggle
buttons are gone: the routing decides what is sent and received, and the
masks are no longer saved with the project.

- The stack of section 1: INPUTS (the host's input channels), one box per
  plugin in chain order (its number and name; "bypassed", or "not loaded
  on the server: bypassed", on the right), OUTPUTS (the host's output
  channels).
- Input pins on a box's top edge, output pins on its bottom edge, grouped
  by bus with the bus name. Pins of an aux bus nothing is connected to are
  hollow: that bus is disabled on the server.
- Connections are curves, bright when their input pin was set by hand,
  dim when it follows the default rule. Hovering one shows its two ends.
- Drag from a pin onto a pin (either way round; the output must be above
  the input). Drag from a bus name onto a bus name or a pin: its channels
  connect in order, as far as both have channels. While dragging, the
  pins it would reach are ringed.
- Shift-click a connection to remove it (it turns red while shift is
  held over it), as in Reaper.
- Right-click an input pin: "Back to the default routing" (that pin
  only) or "Disconnect".
- "Reset routing" puts every pin back on the default rule.
- Below the transport options, the two "ROUTING DEFAULTS" switches of 2.3
  (the window's base height grew by 32 px for that row).

A and C are most of the work. E is the visible part, but mostly
straightforward once A exists.

## 4. Decisions (2026-11-06)

1. Fan-in: several sources into one input are summed.
2. Unconnected FxmeNet outputs are silent.
3. The view goes in the stream and channels view, reorganized around it.
   A connection is removed by shift-click (Reaper's habit). No fallback
   for servers without routing: client and server are always updated
   together while FxmeNet is in development.
4. The default rule (2.3) and the edit rules (2.4) as written. The default
   rule was then stated from the inputs (2.3) when writing step A, for the
   mono-to-stereo case; the behaviour agreed (today's chain) is unchanged.

## 5. Server internals

How the server processes a routed chain (step C). The code is in
`Common/FxmeNet/chain/RoutingEngine.h` (JUCE-free, tested with fake
plugins) and `Server/Source/Chain.cpp` (which drives it).

### 5.1 Serial until routed

A session processes its chain in series, as before, until it receives a
`ROUTING`: one 64-channel work buffer, each plugin in place on its first
channels. Older clients never send one, so nothing changes for them. From
the first `ROUTING` on, the chain is routed for the rest of the session
(a resumed session included). The server announces the `"routing"`
capability in `WELCOME`.

### 5.2 What the server keeps

`Chain::setRouting` keeps the connections exactly as received. Each time
the chain changes (a plugin added, removed or moved, a new format) the
connections that fit the chain as it is are picked again
(`fittingConnections`: boxes that exist, pins in range, going downward,
no duplicate), so a `ROUTING` that arrived before a plugin finished loading
takes effect once it has.

For each slot the server also keeps its box (its buses at full width, as
sent in `SLOT_READY`) and the bus layout it last asked the plugin for.

### 5.3 The plan

Everything the audio thread needs is computed on the message thread into
a `RoutingPlan`, published with the chain's snapshot (the same atomic swap
as the serial chain: the audio thread never waits, the message thread frees
an old snapshot only once the audio thread has let go of it). A plan
holds:

- one buffer per box, N samples per channel, allocated when the plan is
  built: INPUTS (a copy of the session's inputs), one per slot (as wide as
  the plugin's enabled channels, the buffer its `processBlock` receives),
  OUTPUTS (the result);
- for each pin, its channel in its box's buffer. Pins count every bus at
  full width; a slot's buffer has only its enabled buses, in order, so a
  pin of a disabled bus (or past the width a plugin actually gave a bus)
  maps to nothing and its connections are ignored;
- for each input channel of each box, its sources ("taps"): a pointer to
  the source's buffer channel, or a delay line and a delay;
- the timing: each slot's latency as it was when the plan was built, and
  the graph's latency.

The plan is rebuilt after any chain edit, any `ROUTING`, any bus layout
change, and any latency change (5.6). Building it allocates; running it
does not.

### 5.4 One block

1. INPUTS: the session's input channels are copied in (absent ones, not in
   the client's input mask, are silent).
2. Each slot, top to bottom: its buffer is cleared, each input channel is
   gathered (the sum of its taps), the plugin processes the buffer in place
   (`processBlock`, or `processBlockBypassed` when bypassed: the plugin's
   own bypass, which passes each input channel to the same output). A muted
   slot (5.7) is not called and its outputs stay silent.
3. OUTPUTS is gathered the same way, and copied back into the worker's
   buffer, which sends it to the client. Channels nothing feeds are silent.

As soon as a box's buffer is final, the delay lines of its output pins
that have any are written. MIDI goes through the plugins in chain order, as
before (each plugin gets what the one above left).

### 5.5 Latency compensation

From the slots' latencies, `computeTiming` gives each box an arrival time:
the latest of (source's arrival + source's latency) over its sources, 0
for a box fed by nothing. Each connection is delayed by the difference
between its box's arrival and its source's, so everything summed at a box
is aligned. The graph's latency is OUTPUTS' arrival; the server reports it
in every audio reply, as it reported the summed latency before, and the
client adjusts its own latency as it already does.

Delays are done by delay lines, one per source pin that has delayed
connections (written once per block, read once per connection, each at its
own delay). A line is sized for twice its largest delay plus a block,
rounded up to a power of two. A new plan takes over the delay lines of the
plan it replaces when they are large enough, so rebuilding a plan (another
plugin added, a latency change) does not empty the delayed paths; a line
that has to grow starts silent. When the client resets the stream (its
reset parity, protocol.md 5.4), the delay lines are emptied along with the
plugins' reset.

### 5.6 Latency changes

A plugin can change its latency while running. After each plugin's
`processBlock` the audio thread compares its latency with the one the plan
was built with; if it differs it sets a flag, and the session's timer
(at the telemetry rate, 30 Hz by default) rebuilds the plan. Between the change and the
rebuild, at most a few blocks are misaligned, then the delays follow.

### 5.7 Switching aux buses

A bus other than the main ones is enabled only while one of its channels is
connected (2.2). When the connections that fit the chain call for a
different layout for some slots:

1. those slots are muted and a plan without them is published; publishing
   returns once the audio thread has let go of the previous snapshot, so
   from then on it no longer calls those plugins;
2. each of them is released, given its new layout (`setBusesLayout`: a
   bus switched on gets the layout it last had, or its default one) and
   prepared again;
3. they are unmuted and a final plan is published.

The other slots keep playing throughout; the slots concerned are silent for
the few blocks this takes, and lose their internal state (a reverb's tail).
A plugin that refuses the layout keeps its own; the layout asked for is
remembered, so it is not asked again until the routing asks for another.
Main buses are left as they are.

In serial mode the server keeps the plugin's default layout, as before. So
the first `ROUTING` disables the aux buses some plugins enable by default
(TAL Reverb 4's second input) until one is connected: by design (2.2), but
audible on those plugins.

## 6. Client internals

What FxmeNetPlugin does (step D), in `Common/FxmeNet/client/ClientEngine.h`
and the plugin's processor.

### 6.1 The model and the server's view

The engine holds the routing model (`chain::Routing`, section 2.3 and 2.4
rules) over:

- every slot whose buses it knows: from `SLOT_READY`, or saved with the
  project (6.3), so that a project's routing holds before the server has
  answered;
- INPUTS and OUTPUTS 64 channels wide, whatever the host gives, so that a
  project loaded before the host is prepared (or on a host with fewer
  channels) keeps its pins.

What the server gets is that model resolved on the chain the server really
has: a copy of the model is given the server's boxes (INPUTS and OUTPUTS
at the host's channel counts, as in `CONFIGURE`, and only the slots loaded
on the server). The edit rules then do the rest: a slot missing on the
server is bridged as if bypassed for good, pins on channels the host does
not have are dropped. The model itself is never changed by that, so the
missing plugin's routing comes back with it.

### 6.2 When the routing is sent

The resolved connections are recomputed after every change that can
affect them (a slot loaded, refused, removed or moved, the chain replaced
by a project load, a routing edit, new host channel counts) and sent as
`ROUTING` when they differ from the last ones sent. Only to a server whose
`WELCOME` lists `"routing"`; each new connection sends it again once the
chain is in place (a resumed session too: harmless, the server keeps the
same). A `ROUTING` that fails or is refused is sent again at the next
change or connection.

Adding a slot changes nothing until its `SLOT_READY`: its buses are not
known before. On a server without the capability nothing is sent and the
chain stays serial, as before.

### 6.3 Saved with the project

In the plugin's state:

- each `SLOT` gets `BUS` children (input, name, channels, main), its
  buses as last known;
- one `ROUTE` element per manual input pin (box id in hex, channel), with
  a `FROM` child per source. Pins left to the rules are not saved: they
  follow the chain.

Projects saved before have neither: their slots get their buses from the
server as they load, and the routing is the default one (the serial
chain).

### 6.4 Inputs and outputs not used

Every audio request carries its own channel mask (protocol.md 5.2), so the
inputs the routing does not use are simply left out of each request: the
configured mask (the input toggles) less the unused ones, set per block on
the audio thread. No `CONFIGURE`, so no interruption when a connection to
INPUTS is made or removed. Likewise a routed server leaves the outputs
nothing feeds out of its replies (protocol.md 4.6), and the client plays
the missing channels as silence, as it already did.

### 6.5 The local copies' buses

The plugins' local copies (shadows, for their own editors) get the same
bus layout as on the server: on each timer tick the processor works out
the buses each slot needs from the server's routing (`wantedBuses`, the
same function the server uses) and, when that changes, the copy is
released, given the layout and prepared again. So an editor that shows its
sidechain sees it enabled exactly when the server's instance has it.

