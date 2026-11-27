# Choosing a plugin's channel count: design and plan (draft)

Written 2026-11-27, at the user's request. To do after the local fallback
(doc/fallback-plan.md).

## The problem

Some plugins set what they do from the width of their main buses. The IEM
ambisonic plugins are the common case: their order follows the bus size (4
channels: first order, 16: third order, up to 64). A DAW lets the user
choose a track's or a plugin's channel count; FxmeNet hosts the plugins
itself and never does: every plugin keeps the main input and output widths
it declares by default, on the server, in the client's local copies and in
local play (`catalog::useDefaultBuses`, Common/FxmeNet/catalog/Buses.h).

Seen 2026-11-27: AmbiRR2 has 16 outputs (third order) to connect to the
IEM BinauralDecoder, which shows 4 inputs (its default, first order). The
routing cannot help: pins that do not exist cannot be connected.

## Design

**A per-slot setting, Channels: in and out,** for the main input and
output buses. Each lists only the widths the plugin accepts. Saved with the
project, sent to the server, applied to the local copies.

**Which widths a plugin accepts.** When a plugin is loaded, the server
tries each main input width from 1 to 64 with the output as it is, and
each output width with the input as it is
(`AudioProcessor::checkBusesLayoutSupported`, discrete layouts, plus mono
and stereo for 1 and 2). The widths accepted go in SLOT_READY
(`inputWidths`, `outputWidths`). It is a test of layouts, not of
processing: cheap (128 calls, no prepare), done once per load. A plugin
whose accepted inputs depend on its outputs (the IEM plugins mostly do not)
may refuse a pair the lists allow: see "Refused".

**Changing it.** A new control message, SLOT_LAYOUT (27): `{ slotId,
inputs, outputs }`, 0 keeping a side as it is. The server takes the slot
out of the audio path for a moment (as `Chain::update` already mutes a slot
whose aux buses change: a plan without it is published first), sets the
layout, prepares the instance again, describes it again, and replies with
a new SLOT_READY (its buses, its parameters: a plugin may change those with
its order, its latency). The client updates the slot's model, its box for
the routing, and the generic controls.

**At a rebuild** (reconnection, a project opened), SLOT_ADD carries the
widths chosen, applied before `prepareToPlay` as the default buses are now.

**Refused.** A layout the plugin refuses leaves it as it was; the reply
says so, and the Parameters tab shows "X refused 16 inputs: it keeps 4".

**The routing.** New pins appear with the new box. Connections to pins
that disappear after narrowing a slot are kept and wait, as the routing
already does for what does not fit (rule of doc/routing.md).

**The local copies** (the editor's, and local play's: doc/fallback-plan.md)
get the same layout, so that the editor shows the right order (the IEM
plugins draw their order) and local play processes the same channels. In
local play without a server, the accepted widths are tested on the local
copy, as the server does.

**The aux buses** (sidechains) are unchanged: they stay off unless the
routing connects them (Buses.h), at their default width.

**The FxmeNet instance's own channels** (the DAW track's, 1 to 64) are
already free; nothing changes there. A 16-channel FxmeNet instance on a
16-channel track carries third-order ambisonics end to end.

## Interface

In the Parameters tab, above the controls, for the selected slot:
"Channels  in [4 v]  out [2 v]", the lists showing the accepted widths
(the current one selected). Hidden for a plugin that accepts one width
only. Alternative considered: on each box of the Routing tab (a menu on
its title); can be added later, the message is the same.

## Protocol (protocol.md to update)

- SLOT_READY: `inputWidths`, `outputWidths` (arrays of channel counts),
  empty for a JSFX script (its pins are fixed by the script).
- SLOT_ADD: optional `mainInputs`, `mainOutputs` (0: the default).
- SLOT_LAYOUT (27), JSON `{ slotId, inputs, outputs }`; reply SLOT_READY,
  or ERROR (`layoutRefused`, with the widths kept).

No compatibility with older servers (development phase, as agreed).

## Revision (2026-11-27): layouts, not widths

Asked by the user after phase 2: the labels ("16 (order 3)") and the list
(stereo and the ambisonic sizes) were interpretations of a channel count,
true for the IEM plugins, wrong for others (a 16-channel 9.1.6 plugin
would have been called order 3; a 7.1.4 one, refusing 12 discrete
channels, would have offered nothing). Generic instead: what the plugin
accepts decides.

- **Candidates:** every layout JUCE knows, for 1 to 64 channels:
  `AudioChannelSet::channelSetsWithNumberOfChannels` (discrete, the named
  surround layouts up to 9.1.6, ambisonics for square counts) and the
  named layouts it leaves out (5.0.2, 5.0.4). Tested at load as before
  (not prepared), each side with the other as it is.
- **Reported:** SLOT_READY's `inputLayouts` / `outputLayouts`, each
  accepted layout as `{ key, name, channels, discrete }`, and
  `inputLayout` / `outputLayout`, the current ones. A key identifies the
  layout exactly: `amb:<order>`, `dis:<channels>`, or the named layout's
  speaker string (JUCE's `getSpeakerArrangementAsString`, e.g. `L R C Lfe
  Ls Rs`). Names: JUCE's description for named layouts ("5.1 Surround",
  "7.1.4 Surround"), "Ambisonics, order 3 (16 ch)", "16 channels".
- **Chosen and sent:** a layout key per side, empty for the plugin's
  default: SLOT_ADD and SLOT_LAYOUT carry two strings; the model and the
  project keep the keys. Two layouts of the same size (9.1.6 and order-3
  ambisonics) are told apart.
- **Offered (client):** the named and ambisonic layouts the plugin
  accepts; its discrete counts only when it accepts no named layout on
  that side (a generic multichannel plugin); and always its current one.
  Shown when there is more than one.

This replaces phase 1's widths (`inputWidths`, `mainInputs`, ...) and
phase 2's filter and labels (Widths.h becomes Layouts.h); the rest of
phases 1 and 2 stands.

Written 2026-11-27. `catalog::candidateLayouts`, `acceptedMainLayouts`,
`layoutKey` / `layoutFromKey` / `layoutName`, `setMainLayouts` (Buses.h),
`chain::describeLayouts` (Describe.h, server and copies);
`control::LayoutInfo`; SLOT_ADD, SLOT_LAYOUT and the model carry keys
(`SlotModel::inputLayout` / `outputLayout`, saved as such);
Plugin/Source/Layouts.h (offered, labels). Checked with fxmenet-wavclient
(`--layouts`, `--relayout`, keys): the IEM BinauralDecoder reports its
default as `amb:1`, accepts every named layout JUCE has (it counts
channels) and the ambisonic orders 0 to 7, but one discrete count only
(so `dis:16` is refused, as it should, while `amb:3` gives 16 inputs);
a 5.1 input applied by SLOT_LAYOUT; FxmeCompressor accepts Stereo only
(no choice shown), audio still bit-identical.

Then (user, same day): the lists show channel counts only, as numbers
("the user knows what the plugin expects"): every count the plugin
accepts in some layout. A count is applied as the plugin's current layout
when it has that count, else as the first accepted layout of that count
(JUCE's order: discrete, named, ambisonic). The keys stay on the wire and
in the project (exact, generic); only the offered list and its labels
changed (Layouts.h).

## Phases

State (2026-11-27): phase 1 written. `catalog::channelSetsFor`,
`acceptedMainWidths`, `setMainWidths` (Buses.h, shared with the client's
copies); SLOT_ADD's widths, SLOT_READY's `inputWidths` / `outputWidths`,
SLOT_LAYOUT (27) and `layoutRefused` (9); `Chain::setMainWidths` (the slot
muted, which in series now leaves it out of the published chain). Checked
with fxmenet-wavclient `--widths` / `--relayout` and the IEM
BinauralDecoder: 4 inputs by default, 16 when asked in SLOT_ADD, 25 or 7
after a SLOT_LAYOUT; it accepts 1 to 9, 16, 25, 36, 49 and 64; FxmeFX
plugins accept 2 only.

Phase 2 written (2026-11-27), with the user's answers: the setting in
both places (Parameters tab and the routing boxes), only the useful widths
(2 and the ambisonic sizes, plus the current one: Plugin/Source/Widths.h).
`SlotModel::mainInputs` / `mainOutputs` (saved with the project, sent in
SLOT_ADD at every rebuild), `ClientEngine::setMainWidths` (SLOT_LAYOUT; the
answer updates the description, the box and the routing; a refusal goes
to `layoutError`). The copies (`Shadow`) get the widths before being
prepared and find the widths they accept as the server does; a change
reaches a mirror at once, and a local copy after leaving the local chain
(its description follows when there is no server). Checked with
fxmenet-wavclient `--engine --widths 16 0 --relayout 36 0` and the IEM
BinauralDecoder: 16 inputs from the model, 36 after the change, the
routing box with 36 inputs. Not tried yet in a DAW: the two selectors,
AmbiRR2 into the decoder, Local.

Each ends usable; the user tests and commits between them.

1. **Server and protocol.** The widths tested at load, in SLOT_READY;
   SLOT_LAYOUT; the widths in SLOT_ADD. Tests: the messages in
   FxmeNetProtocolTests; fxmenet-wavclient (or a small tool) asking an
   IEM plugin for 16 inputs and getting a 16-input bus back.
2. **Client.** The setting in the slot model, saved with the project and
   sent at a rebuild; SLOT_LAYOUT on a change; the model, box and generic
   controls from the reply; the local copies (editor and local play) with
   the same layout; the widths tested locally in local play. Check in
   REAPER: AmbiRR2 (16 outputs) into the IEM BinauralDecoder set to 16
   inputs, on a stereo track; the decoder's editor shows third order; the
   same in Local.
3. **Docs.** protocol.md, the manual (Parameters tab, routing chapter: an
   ambisonic chain), the README (the IEM plugins in the tested list once
   tried).

## Open questions for the user

- The setting in the Parameters tab (proposed), or on the routing boxes,
  or both?
  > Both
- Width lists: every accepted width (an IEM plugin accepts most from 1 to
  64), or only the "useful" ones (squares for ambisonics: 1, 4, 9, 16, 25,
  36, 49, 64, plus 2)? Proposed: every accepted width, as DAWs do.
  > Only the useful ones
  
