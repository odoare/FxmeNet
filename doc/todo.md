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
15. Later, optional: routing of a slot's extra buses (aux or sidechain
    inputs, like TAL Reverb 4's two). Today channels go by number: on the
    server a plugin's extra inputs get the work buffer's channels past the
    main ones, silent unless an earlier, wider plugin wrote there. A
    per-slot routing (silence, a chain channel, or a DAW sidechain sent by
    the client) would make it explicit.
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
