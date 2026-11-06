# Routing between plugins (proposal)

Status: agreed (2026-11-06), decisions in section 4. It replaces todo item
15 (routing of a slot's extra buses) with a more general design. Steps A
(the routing model) and B (protocol) are written; C to E are to do.

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

- Each plugin gets its own buffer instead of the shared work buffer. For
  each plugin in order: gather its inputs (copy or sum, through the
  compensation delays), process, keep its outputs for the boxes below.
- The graph is swapped in with the chain snapshot, as the chain is today,
  so the audio thread takes no lock.
- Aux buses enabled or disabled as the routing says, preparing again only
  the plugin concerned.
- The latency taken from the longest path.
- An engine test with test processors that only add a delay or a gain,
  checking sums and alignment across parallel paths.

### D. Client engine and state

- The routing lives in the client's model, is saved in the project, and is
  sent on connect and on resume.
- The input and output masks are derived from it.
- Chain edits (insert, remove, move) apply the rules of 2.4.
- The local copies of the plugins (shadows) get the same bus layout as on
  the server.
- Older projects load with the default routing.

### E. The view

It goes in the stream and channels view (decision 3), which is
reorganized around it; it replaces the input and output toggles there.

- The vertical stack of 1, scrollable.
- Dragging from a pin, or from a bus label (all its channels in order).
- Right-click on a connection to delete it; the reset button.
- The total latency shown.

A and C are most of the work. E is the visible part, but mostly
straightforward once A exists.

## 4. Decisions (2026-11-06)

1. Fan-in: several sources into one input are summed.
2. Unconnected FxmeNet outputs are silent.
3. The view goes in the stream and channels view, reorganized around it.
4. The default rule (2.3) and the edit rules (2.4) as written. The default
   rule was then stated from the inputs (2.3) when writing step A, for the
   mono-to-stereo case; the behaviour agreed (today's chain) is unchanged.
