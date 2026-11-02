# FxmeNet done

Newest first. Numbers are those of `todo.md`.

## 2026-11-02

- 6c step 1: `fxmenet-server` builds and runs (scan, listening, clean stop).
  Binary named `fxmenet-server` with `OUTPUT_NAME` (juce_add_console_app
  ignores `PRODUCT_NAME` for the file).
- 6d (written, not yet built): `Tools/WavClient`, the end-to-end test.

- 6b: control channel tests pass (committed).
- 6c step 1 (written, not yet built): `fxmenet-server` in `Server/`:
  catalog scan, control handshake with resume, sessions, chain with
  lock-free snapshot swaps, per-session audio thread with ordering, offline
  hold, reply cache, `UdpPeer` sends. Checked: syntax of every source with
  strict warnings, configure. Protocol completed: acknowledgements (an empty
  frame of the request's type) and `STATE_GET`'s payload.

- 6a: protocol and transport tests pass (committed).
- 6b (written, not yet built): the control channel in
  `Common/FxmeNet/control/` (frames and binary payloads JUCE-free, JSON
  messages, `ControlConnection`), frame tests added to
  `FxmeNetProtocolTests`, new `FxmeNetControlTests` (JSON round trips, TCP
  exchange with a 1 MB state). Protocol amended: plugins are identified by
  JUCE's VST3 `uniqueId` (a 32-bit hash of the normalised class id) plus name
  and vendor, because JUCE's hosting API does not expose the 16-byte class
  id.

- Protocol reviewed, no changes. 6 (part, written, not yet built):
  `Common/` with the wire format (`Wire.h`), fragmentation
  (`Fragmentation.h`), `AudioTransport` and `UdpTransport`, and two test
  programs: `FxmeNetProtocolTests` (JUCE-free: round trips, malformed input,
  fragments in order, reversed, duplicated, interleaved, lost) and
  `FxmeNetTransportTests` (200 requests of 16 channels x 256 samples through
  loopback UDP to an echo thread, sample-exact; a wrong-token datagram
  ignored). Checked: syntax with strict warnings, configure.

- Protocol draft, `doc/protocol.md`: UDP audio (decided) with fragmentation,
  sequence-numbered blocks, a server reply cache and offline retries; TCP
  control with JSON and binary frames; latency in every audio reply;
  transport and MIDI in the request; channel masks; telemetry; discovery;
  reconnection with resume. Transport-neutral by rule, so a TCP audio
  transport stays about a day's work if tests call for it (section 11).

- 4: AudioGridder read (streamer, processor, audio worker, sandbox, message
  format). Findings in `doc/audiogridder-notes.md`: ideas adopted (latency in
  every reply, bypass delayed by the latency, sandbox with a direct port),
  practices avoided (blocking and allocating on the audio thread, raw-struct
  wire format), and the case for UDP with sequence-numbered retries. Its
  code is not reused.

- 5 (Phase 1 done): GainPan and the loopback harness built and run on
  Linux. The mirror's meters follow the source's and freeze with telemetry
  off, parameters follow both ways, schema `inL inR outL outR`, frames of
  104 bytes (8-byte header, 4 meter channels of 24). This also settles
  spike 2 on Linux: the VST3 side channel works. FxmeTools pin bumped to
  94e4e4d.

- 5a: `FxmeCoreNetTelemetryTests` pass (FxmeTools d8b0cff, "Additions for
  FxmeNet").
- 5b, 5c (written, not yet built): `Examples/GainPan` (the howto plugin, full
  shell, NetBridge with four meters) and `Tools/Loopback` (the harness). In
  FxmeTools: `fxme::net::queryNetPlugin()` and `UnknownCompatible` in
  `NetAbi.h`, so a host needs no VST3 SDK headers; `doc/fxmenet-howto.md`.
  Checked: syntax of GainPan (with a stand-in JuceHeader) and the harness
  with `-Wshadow -Wconversion -Wsign-conversion -Wfloat-equal`, clean; CMake
  configure.

- 5 (part, written, not yet built): FxmeTools telemetry. Core:
  `net/TelemetryCodec.h`, `net/TelemetryRegistry.h`, `net/NetAbi.h`, the
  `VuMeter` / `SpectrumTap` / `WaveformTap` additions, tests
  (`CoreNetTelemetryTests.cpp`). Module: `net/NetBridge.h`. Checked: syntax
  with `-Wconversion -Wsign-conversion -Wfloat-equal` (clean apart from three
  existing `VuMeter.h` lines), the core JUCE guard, and `NetBridge.h` against
  both JUCE 9.0.4 and 8.0.12. Logged in FxmeTools `doc/api-changes.md`.

- 1 (done): spike 1 with the settle step. AmbiRR2 bit-identical once the
  message loop runs (0 ms: 0.014 off; 1000 and 3000 ms: identical). In a
  `debian:stable-slim` container with only fontconfig and freetype (no X11,
  xcb or GL libraries), all four plugins load, process and reload
  bit-identical. The server needs no X11. Also found: JUCE's plugin
  identifier string hashes the file path, so it cannot match plugins across
  machines.

- Repository scaffold: root CMake (macOS settings before `project()`),
  `cmake/FxmeDeps.cmake` copied from FxmeFX, `cmake/Dependencies.cmake`
  pinning JUCE 9.0.4 and FxmeTools 1a767df, doc triplet. Configure checked:
  both dependencies resolve to the sibling clones, at their pins.
- 1 (first run): `Spikes/HeadlessHost` built and run on Compressor,
  Equalizer, SpectralDede and AmbiRR2 with no display. All load, process and
  round-trip their state; three bit-identical, AmbiRR2 not (its 30 Hz
  parameter timer never fired: the spike did not run the message loop).
  Results and consequences in `plan.md`, Phase 0. Then: settle step added
  (`settleMs`, message loop run after `prepareToPlay`), `-Wfloat-equal`
  warning fixed, binary name corrected in the docs.
- Plan written and reviewed (`plan.md`): tiers 0/1/2, shadow instances,
  telemetry, the case against just using AudioGridder, porting costs, channel
  masks. Decisions confirmed: VST3 only, client-authoritative state, 256
  parameters with learn mapping, own repo with sibling clones.
