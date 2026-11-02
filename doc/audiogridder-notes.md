# AudioGridder: what FxmeNet takes from it

Read on 2026-11-02 from a shallow clone of github.com/apohl79/audiogridder
(MIT, last push 2024-07-31). Files read: `Plugin/Source/AudioStreamer.hpp`,
`Plugin/Source/PluginProcessor.cpp` (processBlock, latency, automation),
`Server/Source/AudioWorker.cpp`, `Server/Source/Sandbox.cpp`,
`Common/Source/Message.hpp`, `Common/Source/Defaults.hpp`. Conclusion: take
its ideas, not its code. Its real-time discipline and wire format are below
the house standard, so lifting code would cost more to fix than to write.

## How it works, in short

- **Transport:** TCP for audio (`StreamingSocket`), one connection per plugin
  instance, request/response: the client sends a block (audio, MIDI, the
  play head position), the server processes its chain and sends the block
  back with the chain's latency.
- **Buffering:** a client I/O thread between two queues. The read queue is
  pre-filled with `NUM_OF_BUFFERS` silent blocks (default 2), which is the
  added latency: `chain latency + NUM_OF_BUFFERS x block size` (+ a manual
  offset), reported with `setLatencySamples`. With 0 buffers it runs
  synchronously, the audio thread waiting for the reply.
- **When late:** by default the audio thread blocks waiting for data (up to
  1 s). "Live mode" waits at most one block, then outputs silence.
- **Offline rendering:** no special handling. It works because the default
  mode blocks: a faster-than-real-time render simply waits for the server.
- **Server:** one thread per client connection, reads a block, processes the
  chain, replies. No clock of its own.
- **Automation:** a fixed pool of host parameters (16 by default,
  configurable), each slot pointing at (plugin, parameter); the mapping is
  saved in the state, `updateHostDisplay()` renames the slots.
- **Channels:** an "active channels" map per instance; inactive channels are
  not sent, and the server maps them back to the chain's full channel count.
- **Bypass:** the dry signal is delayed by the reported latency (a ring
  buffer), so bypassing does not shift the audio in time.
- **Sandboxing:** the server re-launches its own binary as a child process,
  per client chain or per plugin. The master tells the client the child's
  port and the client connects to the child directly, so audio never goes
  through the master.
- **Wire format:** native C structs (`int`, `bool`, `Uuid`) written raw, no
  magic, no version, no defined endianness or packing.

## Adopted (confirmed our design or added to it)

1. **Request/response without a server clock.** Already our design.
2. **Latency in every audio reply.** The reply header carries the chain's
   current latency, so a separate `LATENCY_CHANGED` message is unnecessary
   and cannot be lost or reordered. The client applies a change on the
   message thread (`setLatencySamples`).
3. **The play head with every block.** Already planned (transport).
4. **Pre-filled silent blocks as the latency budget**, `D` blocks, default 2,
   user-selectable, reported to the DAW.
5. **Bypass delays the dry signal by the reported latency.** New for us: the
   client's `processBlockBypassed` must do it, or bypass shifts the timing.
6. **Automation pool with saved mapping and `updateHostDisplay()`.** Already
   decided (256 + learn); AudioGridder shows it is workable in practice.
7. **Active channels.** Already planned (masks).
8. **Sandboxing as child processes with a direct port**, modes none / per
   client / per plugin. Fits Phase 5's "worker process per session"; the
   direct-port handoff is the detail to keep.

## Done differently, on purpose

1. **Never block the audio thread in real time.** Late block: output silence
   (or a short fade), count it, show it. Block only when `isNonRealtime()`,
   where waiting is exactly what an offline render needs. AudioGridder blocks
   by default and only offers dropping as an option.
2. **No allocation, locking or logging on the audio thread.** AudioGridder
   resizes buffers, adds MIDI events (allocates) and logs from the audio
   path, and locks a mutex shared with chain edits around each block on the
   server. FxmeNet: preallocated buffers, SPSC rings, chain edits swapped in
   lock-free (the old chain freed off the audio thread).
3. **An explicit, versioned, little-endian wire format** (as
   `TelemetryCodec.h`), because client and server may run different OSes and
   different builds.
4. **UDP rather than TCP for audio** (to be confirmed in `protocol.md`). On a
   wired LAN loss is rare, but when one TCP segment is lost the whole stream
   stalls for at least the retransmission timeout (200 ms minimum on Linux),
   then delivers a backlog: tens of blocks lost instead of one. With UDP a
   lost block costs one concealed block. Reliability for offline renders comes
   from sequence-numbered requests: the client re-sends a request that got no
   reply, and the server keeps its last replies by sequence number, so a
   duplicate is answered from the cache rather than processed twice.
5. **Float only.** AudioGridder carries double precision; no FX-Mechanics
   plugin processes doubles.
6. **No screen capture.** The point of FxmeNet (tier 2 telemetry).

## Not relevant to FxmeNet

Instruments and MIDI-only transfer modes (keeping the stream alive while
notes ring), the tray app, screen capture and its FFmpeg encoder, VST2.
