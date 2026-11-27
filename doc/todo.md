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
14. (written 2026-11-28, phases 1 to 5 of doc/fallback-plan.md) Plan written 2026-11-25: doc/fallback-plan.md. Later, low priority, optional (user request 2026-11-03): local fallback. When no
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
19. Low priority. A slot's plugin editor in its own window, as DAWs do (user request
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
20. Mostly settled (2026-11-17): the automatic mapping already skips
    non-automatable parameters (PluginProcessor.cpp), and the user saw
    no ZAM meter among the mapped or shown parameters. Left to check: how
    the generic controls show a read-only parameter, if one shows at all.
    Original note, later, low priority (seen 2026-11-06 with the Zam plugins): plugins
    whose meters are output parameters. DPF plugins (Zam, and others built
    on DPF) declare meters as `kParameterIsOutput` (ZamComp: "Gain
    Reduction", "Output Level"), read-only in VST3 and reported from
    processing. They already work in FxmeNet through the parameter path
    (the server's ParamWatcher sends them as PARAM_CHANGED at the
    session's timer rate, the shadow takes them with setQuietly). To check
    and, if needed, fix: whether such parameters show in the generic
    controls as if they could be moved, and whether the automatic mapping
    gives them pool entries (DAW automation lanes that move by
    themselves, and lanes taken for nothing). Likely fix: skip read-only
    or non-automatable parameters when auto-mapping (SLOT_READY's
    `automatable` already travels; a `readOnly` flag may be needed), and
    show them as read-outs in the generic view.
21. (written 2026-11-17) Network buffers. `transport/SocketBuffers.h`
    (`growSocketBuffers`): the server asks for 4 MB send and receive
    buffers on its shared UDP socket (forced past the system's cap when
    it may: root or CAP_NET_ADMIN), logs the sizes it got ("UDP buffers:
    send ..., receive ...") and, on Linux, when a size is below 4 MB,
    the sysctl to raise and the file to keep it in; the client's audio
    socket asks for 256 KB send, 1 MB receive (not logged). macOS: the
    size is halved until accepted (kern.ipc.maxsockbuf). To check: the
    server's log at start (with the default 208 KB limits it shows both
    hints); a 28-track run with the limits raised gives no send errors.
    Original note: network buffers (seen in the stress tests of 2026-11-07, see
    `doc/Tests/stress-tests.md`). The server sends every session's replies
    from one UDP socket with the system's default send buffer (about
    208 KB). Under load the buffer fills and the kernel drops replies:
    about 0.3 % of them at 28 tracks (`UdpSndbufErrors` on the server, and
    the `send errors` count in the activity line), each one a concealed
    block on the client. To do:
    - the server asks for a large send and receive buffer on its socket
      (`SO_SNDBUF`, `SO_RCVBUF`, a few MB) and logs the sizes it got;
    - Linux caps them at `net.core.wmem_max` / `rmem_max`: when the size
      got is smaller, the log says which sysctl to raise (or document
      `/etc/sysctl.d/90-fxmenet.conf` in the server's setup notes);
    - the same on the client's sockets, if its receive side ever shows
      drops;
    - check with a 28-track run that the send errors are gone.
22. (written) The server plugins in FxmeNet's editor sorted T2, T1, T0,
    then by name, with a filter field above the list (user request
    2026-11-10, before opening the project on GitHub). To check: the
    order once the local scan is done (and by name only before), typing
    a few letters selects the first match and Return adds it, Escape
    empties the field, the selection stays on its plugin when the list
    changes, Add and double-click add the right plugin when filtered.
23. (written) fxmenet-top (`Tools/Top`), the server in a terminal: sessions
    with their chain and figures, CPU (machine, cores, server), memory,
    network, the log, and the plugin folders (add, remove, default
    folders on and off, rescans) kept by the server from one run to the
    next (user request 2026-11-10, same reason). Protocol in
    protocol.md 4.7. To check:
    - against a server under load: the figures agree with htop and the
      activity line; sessions coming and going; the server restarted
      (fxmenet-top reconnects);
    - folders: add one, remove it, defaults off and on, a rescan while
      a session plays (no glitch), a full rescan; the folders survive a
      server restart; a client sees the new plugins after reconnecting;
    - from another machine: read only, unless `--remote-admin`;
    - on Windows and macOS (the server and fxmenet-top): the CPU and
      memory figures, the keys, the box characters;
    - stopping the server during a full rescan (it must quit at once).
    Later, if wanted: tell connected clients that the catalog changed
    (an unsolicited `PLUGIN_LIST`), so their list follows a rescan.
24. (written) The "Stream & channels" tab split in two (user request
    2026-11-10): "Routing" (the routing view, Reset routing, the two
    defaults) and "Settings" (N, D and latency, transport, local plugins).
    To check: both tabs at the default window size and when the window
    grows for a large plugin editor.
25. (draft) The user manual, `doc/manual` (LaTeX, PDF and HTML for
    fx-mechanics.com; user request 2026-11-10). 13 chapters written from
    the code and the notes; every gap is a visible `[TODO]`, listed in
    `doc/manual/README.md`: screenshots, the final compatible-plugin mark
    (the two logos are drafts: opaque JPEG, a 64 px icon) with the
    conditions of use, installation and packaging, the server as a
    service, and checking each statement against the running software.
    Keep it in step when the plugin or the server change.
26. Only when a script needs it (user, 2026-11-17): JSFX variables that
    the audio code assigns inside functions. `JsfxView` puts back, before
    each frame, the server's value of the variables assigned in `@sample`
    and `@block` (jsfx-plan.md, phase 5), found by a lexical scan that
    misses assignments inside functions called from there. Extend the scan
    to the bodies of the functions those sections call (and what they
    call), if a script's display stays still because of it.
27. (written 2026-11-27: phases 1 and 2, then channel counts only) Plan written 2026-11-27: doc/bus-width-plan.md. A per-slot choice of
    the main buses' channel count (IEM ambisonic plugins follow it: AmbiRR2's
    16 outputs into the IEM BinauralDecoder, stuck at 4 inputs). After the
    local fallback.
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
11. Phase 7: JSFX scripts as plugins of the chain (user request
    2026-11-12). Design, phases and state in `doc/jsfx-plan.md`. Phases 1
    (the engine, `fxjsfx`, in the fork `~/src/JesuNet/jsfx`) and 2 (scripts
    sent in `SLOT_ADD`'s state, processed on the server, generic controls)
    were tried in a DAW by the user (2026-11-12): they work.
    Phase 3 (the `@gfx` display, read-only) was tried by the user: all
    checks pass (2026-11-12).
    Phase 4 (the display interactive) is written. To check in a DAW, with
    scripts whose interface is their `@gfx` (REAPER's stock set: an EQ or a
    compressor with a curve to drag; `analysis/gfxanalyzer` reacts to clicks
    on its settings line; anything using `gfx_getchar` for keys):
    - clicking and dragging in the display acts as in REAPER;
    - a slider moved from the display: the generic controls follow, the
      sound changes, and the DAW records it once mapped ("Map last
      touched");
    - the wheel and modifiers (shift, control, alt) act as in REAPER;
    - keys reach the script once it was clicked (the DAW may keep some);
    - a state the display sets (a mode, a curve) survives closing and
      reopening it, and saving and reopening the project;
    - the outgoing traffic of the client while dragging (fxmenet-top shows
      the server's side; the client's is in its stats line).
    Before the next release: commit and push the fork, then update its pin
    in `cmake/Dependencies.cmake`; build `fxjsfx` on macOS and Windows.
    Phase 4 was tried by the user: it works (2026-11-12). Phase 5's
    essentials are written (2026-11-13). To check in a DAW:
    - a script whose display shows text built in `@init` or `@block`
      (`#name` strings, `sprintf`): the text shows;
    - a script using `gmem[]` with a display that reads it;
    - `guitar/amp-model`: "Model" is a list of the models, each changes the
      sound; saving and reopening the project keeps the model.
    Not done: presets, a script editor, the manual.
