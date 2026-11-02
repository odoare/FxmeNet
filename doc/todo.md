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

6. Phase 2, remaining:
   - 6b. Build and run `FxmeNetProtocolTests` (now with control frames) and
     `FxmeNetControlTests` (written).
   - 6c. `fxmenet-server`: control server, session routing on one UDP
     socket, reply cache, chain engine (VST3 slots, lock-free swaps),
     message loop, telemetry pump, discovery responder.
   - 6d. Command-line test client: a WAV through a chain on the server,
     compared with local processing (bit-identical).
7. Phase 3: FxmeNetPlugin tier 0, parameter pool with learn mapping, channel
   enable/disable UI.
8. Phase 4: shadow editors and telemetry; port Compressor and Equalizer.
9. Phase 5: hardening, measurements, CI.
10. Phase 6: plugin distribution.
11. Phase 7: JSFX.
