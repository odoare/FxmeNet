# FxmeNet todo

Numbered items keep their number when they move to `done.md`. Phases refer to
[`../plan.md`](../plan.md) section 5.

## Phase 0: spikes

2. Spike 2, remaining: the side channel works on Linux (Phase 1); run the
   loopback harness on Windows and macOS (the `INetPlugin` layout and
   `__stdcall` convention are the part to confirm there).
3. Spike 3: a minimal JUCE plugin hosting a VST3 and embedding its editor,
   in Reaper on Linux (nested XEmbed).
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
13. Later, optional: mDNS publishing (`_fxmenet._tcp`) for system browsers.
8. Phase 4: shadow editors and telemetry; port Compressor and Equalizer.
9. Phase 5: hardening, measurements, CI.
10. Phase 6: plugin distribution.
11. Phase 7: JSFX.
