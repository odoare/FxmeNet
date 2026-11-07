# FxmeNet stress tests

First stress tests, 2026-11-06. Raw notes in `tests.txt`.

## 1. Setup

| Role   | Machine                                                                 |
|--------|-------------------------------------------------------------------------|
| Client | Windows 11, Intel i7-6700K (4 cores, 8 threads, 4.0 GHz), 32 GB, Reaper, RME HDSP (ASIO) |
| Server | Ubuntu 24.04 LTS, AMD Ryzen 5 PRO 2400G (4 cores, 8 threads)            |

All tests at 44.1 kHz, stereo tracks, one FxmeNet instance per track. "Full
chain" means all 17 FxmeFX plugins loaded in each FxmeNet instance. FxmeNet
settings are written block × budget (for example 1024×2: blocks of 1024
samples, a budget of 2 blocks). The ASIO buffer is set separately in
Reaper.

## 2. Results

| # | Tracks | Chain | ASIO buffer | FxmeNet | Server CPU | Client CPU | Network | Result |
|---|--------|-------|-------------|---------|------------|------------|---------|--------|
| 1a | 24 remote | full | 128 | 1024×2 | 55 % per core | < 20 % | ~71 Mbit/s each way | a concealed block every 5 to 10 s, occasional clicks |
| 1b | 28 remote | full | 128 | 1024×2 | | | | 1 or 2 concealed blocks per second, more clicks |
| 2 | 32 remote | one FxmeEqualizer (6 bands) | 128 | 128×2 | one thread at 18.5 %, the others 1.5 to 1.9 % | ~20 % | ~95 Mbit/s each way | no problem |
| 3 | 24 local | full | 128 | (local) | | 75 % per core | | reference: local processing only |
| 4 | 12 local + 12 remote | full | 128 | 128×2 | | | | nothing audible, a concealed block every ~2 s |
| 5 | 24 remote | full | 256 | 256×2 | 66 % (one core at 72 %) | 25 % | ~75 Mbit/s each way | ~10 concealed blocks in 5 min on one track: the most robust case |

**The clicks.** In tests 1a and 1b (more often in 1b), the output
sometimes has a sudden short peak, like a dirac. A few lost blocks could be
tolerated, but these clicks can't: they are the main problem found.

## 3. Analysis

### 3.1 The losses at 1024×2 are not a CPU deadline problem

- At 1024×2 the server has about 2 blocks (46 ms) to reply. With the
  server at 55 % average load, the processing alone can't miss that often.
  - Estimate: 24 sessions × 17 plugins on 8 hardware threads at 55 % is
    about 4 ms per session per block. If all 24 sessions start together,
    the last one finishes after about 13 ms, well inside 46 ms.
- Test 5 (256×2, a deadline of 12 ms) loses far fewer blocks than test 1a,
  at a higher CPU load. If deadlines were the problem, it would be the
  other way round.
- So something else changes with block size. The main candidate is the
  size of the network bursts (3.3).

### 3.2 The offload works

24 tracks with the full chain take 75 % of the client CPU locally (test 3),
and 20 to 25 % through FxmeNet (tests 1a and 5), with the work moved to a
machine whose CPU is no faster.

### 3.3 Network: bursts, not bandwidth

- 24 stereo tracks of 32-bit float at 44.1 kHz carry about 68 Mbit/s of
  audio, which matches the ~71 Mbit/s measured once headers are added.
  Test 2 at ~95 Mbit/s each way could not run on a 100 Mbit link, so the
  link is gigabit (to confirm with `ethtool <interface>` on the server).
  Average bandwidth is not the limit.
- **Bursts.** All the FxmeNet instances run in the same host callbacks
  and were started together, so they fill their blocks at the same moment
  and send them together:

  | FxmeNet | Bytes per block (stereo) | Datagrams per block (1400 bytes) | Burst for 24 tracks | Every |
  |---------|--------------------------|----------------------------------|---------------------|-------|
  | 128×2   | 1 KB                     | 1                                | 24 datagrams, 25 KB  | 2.9 ms |
  | 256×2   | 2 KB                     | 2                                | 48 datagrams, 50 KB  | 5.8 ms |
  | 1024×2  | 8 KB                     | 6                                | 144 datagrams, 200 KB | 23 ms |

- **The server receives every session on one UDP socket**, with the
  system's default receive buffer (`net.core.rmem_default`, 208 KB on a
  standard Ubuntu). The kernel counts more than the payload for each
  datagram (typically 2 to 4 KB for a 1.4 KB datagram), so a 144-datagram
  burst needs 300 to 600 KB. If the reading thread doesn't empty the
  socket fast enough, the kernel drops datagrams.
- **The reading thread has a lower priority than the audio threads.** The
  server's UDP thread runs at ordinary high priority, while the 24 session
  threads are real-time. While the sessions process a burst, they can delay
  the UDP thread, and the socket fills up.
- A block of 1024 is 6 fragments, and losing any one of them loses the
  whole block. So the larger the block, the more likely each block is to
  be lost.
- This fits all the results: 1024×2 loses blocks, 1b (28 tracks, a bigger
  burst) loses more, and 256×2 and 128×2 with light chains are clean.

### 3.4 Where the clicks may come from

A plain concealed block should not sound like a dirac. The client fades
the output out over 64 samples and back in over 64
(`Common/FxmeNet/client/BlockStream.h`), so a missing reply sounds like a
short, soft dropout. There are two cases:

1. **A request is lost on its way to the server** (a dropped datagram, see
   3.3). The server counts it as `lost`, or as `stale` if it arrives after
   it was skipped. In real time the server doesn't wait: it goes straight to
   the next block (`Server/Source/AudioWorker.cpp`). The plugins then see a
   jump in their input (1024 samples at 1024×2), and 17 plugins (filters,
   compressors, reverbs) turn that step into a broadband click.
   - That click is inside a reply that arrives on time, so the client plays
     it at full level.
   - The client's fade-in doesn't cover it: the chain's latency puts the
     click later than the block boundary where the fade is applied.
   - **This is the likely source of the clicks**, and it fits 3.3: losses
     on the way in.
2. **A reply is lost or late on its way back**, while the server's output
   stays continuous. Only the client's 64-sample ramps cut in, which should
   sound soft.

### 3.5 Other things that may add jitter

These matter more at small blocks (128×2, test 4), where the deadline is
short:

- **Server thread priority.** If the system refuses real-time priority,
  the audio thread falls back to an ordinary thread without saying so
  (`AudioWorker::start`).
- **Server CPU frequency scaling and kernel preemption.** Both add wakeup
  jitter on Ryzen with Ubuntu's generic kernel.
- **Reaper's anticipative FX processing.** Reaper may render tracks ahead,
  in bursts. The client asks for a reply after a fixed number of samples,
  not a fixed time, so a burst shortens the time the server has to answer.
- **Interrupt moderation on the client's network adapter.** It delays
  received packets.

## 4. Test plan

Each step is run first on test 1a (24 tracks, full chain, ASIO 128,
FxmeNet 1024×2), which fails often enough to show a difference, then on
test 5 (256×2) and on test 4 (128×2).

### Step 1: find where blocks are lost

1. Before the test, on the server, note the UDP counters:
   `nstat -az | grep -E 'UdpRcvbufErrors|UdpInErrors|UdpSndbufErrors'`
   (or `netstat -su`).
2. Start the server in a terminal and keep its per-session activity lines:
   `lost`, `stale`, `process … us (max …)`.
3. In the client, turn on the event log (written to
   `fxmenet-client-events.log`) and note the concealed counts.
4. Record Reaper's output for a few minutes.
5. After the test, read the UDP counters again and compare:
   - `UdpRcvbufErrors` rising: the server's socket overflows (3.3). Go to
     step 2.
   - `lost` or `stale` rising with the clicks: case 1 of 3.4 (losses on the
     way in).
   - Only the client's concealed count rising, `lost` at 0: losses or
     delays on the way back. The `max` process time shows whether the
     server itself is sometimes slow.
6. Zoom in on one click in the recording: a burst of noise (case 1), or a
   hole with ramps (case 2).

### Step 2: larger socket buffers (no code)

On the server:

```
sudo sysctl -w net.core.rmem_default=4194304 net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_default=4194304 net.core.wmem_max=8388608
```

Restart the server (the default applies to sockets created afterwards) and
repeat step 1. If losses and clicks go away at 1024×2, 3.3 is confirmed.
These settings are lost at reboot unless written to
`/etc/sysctl.d/90-fxmenet.conf`.

### Step 3: server tuning (no code)

Repeat the step 1 measurement after each change, to see which one helps.

1. **Real-time priority.** Check with
   `ps -eLo tid,cls,rtprio,pcpu,comm | grep -i fxme`: the audio threads
   should show `FF` or `RR` and rtprio 8. If not, create
   `/etc/security/limits.d/audio.conf` with:
   ```
   @audio - rtprio 95
   @audio - memlock unlimited
   ```
   add the user to the `audio` group, and log in again.
2. **CPU governor:** `sudo cpupower frequency-set -g performance`.
3. **Kernel preemption:** boot with `preempt=full`, or install
   `linux-lowlatency`.

### Step 4: client tuning (no code)

1. Turn off anticipative FX for the FxmeNet tracks (track performance
   options: prevent anticipative FX), and compare the concealed counts.
2. On the network adapter, turn interrupt moderation off or to low; use
   the High performance power plan.

### Step 5: code changes, depending on steps 1 and 2

- **If step 2 helps:** the server asks for a large receive and send buffer
  on its socket (`SO_RCVBUF`, `SO_SNDBUF`, a few MB) and logs the size it
  got. The client does the same on its sockets. The UDP thread becomes a
  real-time thread above the audio threads, so it always empties the
  socket.
- **If case 1 of 3.4:** when a block is missing in real time, the server
  processes a made-up block in its place (the last input faded to silence)
  instead of jumping. The plugins never see a step and their time stays
  continuous; the client conceals that block as before. This removes the
  clicks whatever the cause of the loss.
- **In any case:** the server logs a warning when it can't get real-time
  priority.
- **Possibly:** spread the sends of the instances over time, so that 24
  instances don't send in the same instant. To consider only if the above
  is not enough.

### Step 6: measure again

Rerun tests 1a, 1b, 4 and 5 after the changes and add the results to this
document, to find the new limits at 128×2, 256×2 and 1024×2.
