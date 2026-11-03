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
   - 8a (written): `LocalCatalog` (local scan, cache, tiers) and the tiers
     in the editor. Build `FxmeNetPlugin_VST3`; in Reaper the Stream &
     channels tab shows the scan, then the count; the lists show T1 or T2
     for plugins installed on both sides (T2 for GainPan), T0 for one the
     server alone has (start the server with `--plugin-path` on a folder
     holding a copy of a plugin you do not have in `~/.vst3`); a second
     start of Reaper scans nothing.
   - 8b: shadow instances (mirror role) and their editors, parameter sync
     both ways, `notifyStateChanged` round trips.
   - 8c: telemetry into the shadows (TELEMETRY_ACTIVE when an editor opens).
   - 8d: port FxmeFX Compressor and Equalizer to NetBridge.
9. Phase 5: hardening, measurements, CI.
10. Phase 6: plugin distribution.
11. Phase 7: JSFX.
