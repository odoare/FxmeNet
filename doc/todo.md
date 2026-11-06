# FxmeNet todo

Numbered items keep their number when they move to `done.md`. Phases refer to
[`../plan.md`](../plan.md) section 5.

## Phase 0: spikes

2. Spike 2, remaining: the side channel works on Linux (Phase 1); run the
   loopback harness on Windows and macOS (the `INetPlugin` layout and
   `__stdcall` convention are the part to confirm there).
12. Not FxmeNet's code, to report to AmbiRR2: when `isNonRealtime()`, wait for
    the image-source recompute instead of processing with stale taps, so
    offline bounces are deterministic.

## Phase 1 onwards

6. Phase 2, remaining: 6g, the wavclient runs between two machines (offline
   and `--realtime`, also `--engine`); needs a second machine.
14. Later, optional (user request 2026-11-03): local fallback. When no
    server answers (or on demand, an option per instance), process the chain
    locally with the client's own copies of the plugins. Builds on Phase 4,
    which already scans the client's plugins and loads local copies (as
    mirrors) matched by uid, version and hash: fallback switches them to the
    local role. Points to settle: keep the reported latency unchanged (the
    local output delayed by the same amount, so the DAW's compensation does
    not jump), move each slot's state across both ways (the client already
    holds it), a crossfade at the switch, what to do with plugins missing
    locally (dry, with a warning), and whether to switch back automatically
    when the server returns.
15. Routing between the chain's plugins, with sidechains and parallel
    paths (user request 2026-11-06): the design, decisions and plan are in
    [`routing.md`](routing.md). Steps:
    - A (written): the routing model, `Common/FxmeNet/chain/Routing.h`.
      Build and run FxmeNetRoutingTests.
    - B (written): protocol (buses in SLOT_READY, ROUTING message,
      capability; protocol.md 4.5 and 4.6). Build and run
      FxmeNetProtocolTests and FxmeNetControlTests; rebuild the server and
      the plugin (nothing changes in use yet).
    - C (written): server (a buffer per plugin, sums, latency
      compensation, aux buses enabled when connected; routing.md section
      5). Build and run FxmeNetRoutingTests; rebuild the server. Nothing
      changes in use until D: the client sends no ROUTING yet, so chains
      stay serial (the server now lists "routing" in WELCOME).
    - D: client engine and state (masks derived from the routing).
    - E: the view, in a reorganized stream and channels view.
16. (written) Tier 1 plugins: send non-parameter state changes to the
    server. Before, `syncShadows` sent a shadow's state only when its
    state-change counter moved, and only FxmeNet-aware (tier 2) plugins have
    one, so whatever a tier 1 editor changed outside its parameters stayed
    on the client. Seen with Mango (2026-11-05): its lanes and blocks are
    state, the shadow showed the pattern, the server's instance had none
    and passed the audio through. Done (Shadow.h, `takeStateReference` and
    `takeStateChange`): for tier 1 shadows, `syncShadows` reads the state
    at 5 Hz while the editor is open, and once when it closes, and sends it
    (`engine.setSlotState`) when it differs from the reference, which is
    taken when the server's state is applied and moves with each change.
    Parameters are part of that state but travel on their own, so when
    parameters moved too, the state is read again with the reference's
    parameter values put back for a moment: if that gives the reference,
    only parameters changed and nothing is sent (otherwise each knob move,
    or the DAW's automation, would reload the whole state on the server).
    Cost: one state read per open tier 1 editor and poll, two when
    parameters moved; a plugin with a huge state should become tier 2.
    To check: build the plugin; in Reaper, Mango in FxmeNet, draw a
    pattern in its editor: the server's instance plays it (with the editor
    still open, within a fifth of a second); save, reload the project: the
    pattern is there. Turning a knob in a tier 1 editor while it plays
    stays smooth (a state reload per move would be heard as resets).
17. ModalDish as a tier 2 plugin (seen 2026-11-05: in FxmeNet the editor
    works, a plate can be drawn and computed, but nothing sounds, neither
    clicks, MIDI nor input audio). Causes: the shape, the boundaries and the
    computed modes are state, built in the client's mirror and never sent,
    so the server's instance has no modes (item 16, but polling does not
    suit a state holding megabytes of mode cache); and saving the DAW
    project fetches the server's empty state, so the design is lost. Mouse
    strikes (`requestStrike`, an atomic mailbox read by the audio thread)
    stay in the mirror, which never processes. Plan, three commits:
    - a. In ModalDish: a `NetBridge`, and `net.notifyStateChanged()` when a
      computation completes and when the shape or the boundaries change; the
      tier 2 path then sends the state with its modal cache, and the server
      restores the modes from it without recomputing. This alone brings
      MIDI and input audio back.
    - b. An event channel from client to server: in FxmeTools (`NetAbi.h`,
      ABI 2, backward compatible) a call where the mirror queues a small
      event and one where the server delivers it; in FxmeNet a control
      message carrying them to the slot (protocol.md to update). Reusable by
      any FX-Mechanics plugin whose editor triggers something.
    - c. In ModalDish: `requestStrike` queues an event in a mirror, the
      server's instance posts it into its strike mailbox. Optionally,
      telemetry for its hit display and meters.
    Cheaper alternative to b and c: strikes as hidden parameters (x, y,
    velocity, a counter), no protocol change, but four more parameters
    shown by DAWs.
18. (written) Telemetry frames dropped by the client's UDP reassembler.
    Seen 2026-11-05 with the Equalizer in FxmeNet, Reaper on its PulseAudio
    driver with large buffers: the spectrum updated slowly (fine with JACK,
    even at 128). Likely cause: the client's message limit followed the
    session block N (`64 + maxMidiBytes + 64 x N x 4`, about 41 kB at
    N = 128), while a telemetry frame carries up to 8192 samples per tap
    (two taps, about 64 kB) when bursty audio fills the server's taps. Done:
    the limit is now also at least `AudioLink::maxTelemetryMessage` (16 taps
    of 8192 samples, about 530 kB; four reassembly slots, about 2 MB per
    link); the reassembler counts a message over its limit once, as
    `tooLarge` (it was one `malformed` per fragment); the client counts
    reassembly losses (`udpDropped`, `udpTooLarge`) and shows them in the
    statistics line ("UDP dropped N (M too large)"). To check(done on 2026-11-06):
    build and run FxmeNetProtocolTests; in Reaper on PulseAudio with a large buffer, the
    Equalizer's spectrum in FxmeNet now moves as with JACK, and "UDP
    dropped" stays at or near 0 (it also counts genuinely lost datagrams).
19. A slot's plugin editor in its own window, as DAWs do (user request
    2026-11-05). Today `ParameterPanel::refresh` (Panels.cpp) puts the
    shadow's editor in a viewport. Plan:
    - a. Basic (about half a day): a "Window" button beside "Generic
      controls" / "Plugin editor" opens the editor in a
      `juce::DocumentWindow` that follows the editor's size; one window per
      slot, several open at once (`setEditorShown (slotId, ...)` is already
      per slot, so each gets its telemetry). A processor has one active
      editor, so a slot's editor is in the panel or in its window, never
      both: the panel then says so, with a button to bring it back, and
      keeps "Map last touched". The windows belong to FxmeNet's editor and
      close with it; `beforeShadowRemoved` closes a slot's window too (it
      only reaches the panel today). Always on top by default: on Linux a
      window opened from a plugin is not tied to the DAW's window and can
      fall behind it. Check text entry in such a window
      (`fxme::TextEntryFocusFixer`).
    - b. Later: which windows were open and where, saved in FxmeNet's
      state and reopened with the project.
    - c. Later, optional: windows that outlive FxmeNet's editor (owned by
      the processor, as Reaper's floating FX windows), with the lifetime
      cases that brings (slot removed, session lost, processor deleted).
13. Later, optional: mDNS publishing (`_fxmenet._tcp`) for system browsers.
8. Phase 4: shadow editors and telemetry; port Compressor and Equalizer.
   - 8c (written): telemetry into the shadows. Build the plugin; in Reaper
     open GainPan's editor in FxmeNet: its four meters move with the audio
     processed on the server; another slot selected,
     "Generic controls", or the window closed: the server stops sending
     (its log's telemetry frame count stops growing).
   - 8d (written, in FxmeFX): Compressor ("gr" meter), Equalizer ("in" and
     "out" spectrum taps), Freeze ("display" tap; its private display FIFO
     replaced by an fxme::SpectrumTap, `isEngaged()` read from the
     parameter so a mirror shows it) and Cab (bridge only: its IRs are
     chosen by parameters and plotted from the mirror's own load). FxmeFX's
     FxmeTools pin bumped to 94e4e4d. Build the four VST3s in FxmeFX and the
     server (rescans them: ABI now 1), then in FxmeNet in Reaper: each shows
     T2; the Compressor's GR meter, the EQ's spectra and Freeze's display
     move with the server's audio; Freeze's FROZEN state follows its switch;
     Cab's IR plot follows the IR choice. Also each plugin alone in Reaper,
     to check nothing changed locally (Freeze's display especially).
9. Phase 5: hardening, measurements, CI.
10. Phase 6: plugin distribution.
11. Phase 7: JSFX.
