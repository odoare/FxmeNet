# FxmeNet done

Newest first. Numbers are those of `todo.md`.

## 2026-11-02

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
