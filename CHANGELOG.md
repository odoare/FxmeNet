# Changelog

All notable changes to FxmeNet (the FxmeNet and FxmeNet Instrument plugins,
`fxmenet-server` and `fxmenet-top`). The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

To release: rename `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD`, start a new empty
`[Unreleased]` above it, commit, then tag `vX.Y.Z`. The release workflow
publishes that section as the release notes, and stops before building if the
tag has no section here (`.github/changelog-notes.sh`).

## [Unreleased]

## [0.4.0] - 2026-11-27

Client and server must be updated together: this version adds protocol
messages (`SLOT_LAYOUT`, `SLOT_EVENTS`) that a 0.3.0 server does not know.

### Added

- **Playing without the server.** A new *Processing* setting (Settings tab):
  - *Server*: as before.
  - *Local*: the chain plays on the DAW's computer with the plugins installed
    there, and FxmeNet does not connect.
  - *Auto*: the chain plays on the server, and locally while the server is
    lost (the session down for two seconds, or the server silent while audio
    is sent). It goes back to the server, with a crossfade of about 50 ms,
    once the server has stayed back for three seconds. That happens by itself
    or on *Back to the server*.
  - The latency the DAW sees does not change, so tracks keep their timing.
  - Each plugin plays through its local copy, the one that shows its editor.
    A plugin missing on this computer is passed through and counted in the
    status line.
  - JSFX scripts play locally too, with their display.
  - *Local copies loaded in advance* (on by default) keeps every copy
    loaded while the server plays, so the switch to local is immediate.
  - The statistics line shows the local load while the chain plays here.
  - The *Processing* setting is saved with the project.
- **Channel counts.** Plugins that accept several channel counts get *in* and
  *out* lists in their Parameters tab, and the same choice when you
  right-click their box in the Routing tab. Ambisonic plugins (IEM) take their
  order from it: 4 channels for order 1, 16 for order 3, up to 64. The choice
  is saved with the project, applied on the server and to the local copies,
  and the routing pins follow it. New protocol message `SLOT_LAYOUT`; the
  widths a plugin accepts are listed in `SLOT_READY`.
- **Events from a plugin's editor to the server.** FxmeNet-aware plugins
  built with FxmeTools' ABI 2 can send what their editor triggers that is
  neither a parameter nor state (a click on ModalDish's plate, its MIDI
  learn, its pickup meter) to their instance on the server. New protocol
  message `SLOT_EVENTS`. Plugins built against ABI 1 are unaffected.
- `FxmeNetLoopback` carries these events from the mirror to the source.
- `fxmenet-wavclient`: `--local` (the engine's local play, no server),
  `--return-test` (local play, then back to the server midway), `--layouts`
  and `--relayout` (main bus layouts).
- The manual covers playing without the server, channel counts and
  ambisonics, and exports made across a switch.

## [0.3.0] - 2026-11-25

## [0.2.1] - 2026-11-18

## [0.2.0] - 2026-11-17

## [0.1.0] - 2026-11-12

Releases up to 0.3.0 predate this file: their notes are on the
[releases page](https://github.com/odoare/FxmeNet/releases).

[Unreleased]: https://github.com/odoare/FxmeNet/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/odoare/FxmeNet/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/odoare/FxmeNet/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/odoare/FxmeNet/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/odoare/FxmeNet/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/odoare/FxmeNet/releases/tag/v0.1.0
