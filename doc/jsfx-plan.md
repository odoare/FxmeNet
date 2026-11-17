# JSFX in the FxmeNet chain: design and plan (draft)

Written 2026-11-12, from a reading of the JSFX sources (the clone of
github.com/odoare/jsfx, now in `~/src/jsfx`, branch `fxmenet-branch`,
LGPL 2.1) and of FxmeNet as released (0.1.0). This is Phase 7 of
`doc/todo.md`. Agreed with the user on 2026-11-12: no commercial build for
now, scripts come from the client only, `nasm` is a build dependency.

State (2026-11-17): phases 1 to 5 are written and were tried in a DAW,
on Linux and Windows (server and client on either side). macOS is built
by the release workflow but not tried. What is left is listed at the end
of phase 5.

## Goal

A JSFX script (Jesusonic, REAPER's scripted effects) becomes a plugin of
the FxmeNet chain, beside the VST3 plugins:

- the client sends the text of the script to the server, which compiles it
  and runs its audio code in the chain;
- a script without `@gfx` shows generic controls on the client (as a VST3
  that is not installed on the client does);
- a script with `@gfx` draws on the client, from the server's real data.
  The client runs the drawing code only; the audio code runs on the server
  only, so the client's CPU stays low.

## What the reading showed

1. **One VM, shared by everything.** A JSFX instance (`SX_Instance`) has one
   EEL2 virtual machine. `@init`, `@slider`, `@block`, `@sample` and `@gfx`
   are code handles compiled into it and share its state:
   - named variables (a table of doubles, `NSEEL_VM_enumallvars`);
   - the memory `x[i]` (blocks of 65536 doubles, allocated on first use);
   - strings, the global memory `gmem[]`, and the sliders (which are named
     variables too).
   In REAPER `@gfx` runs on the UI thread while the audio thread runs
   `@sample`, without a lock: `@gfx` already sees a snapshot that is "about
   now". A mirror that is one frame late is within what scripts expect.
2. **The core is separable from REAPER.** `effectload.cpp`,
   `effectproc.cpp` and `miscfunc.cpp` (loading, compiling, processing, the
   EEL functions) contain no window code. A throwaway build of these three
   files on Linux, without REAPER and with the portable EEL2 back end,
   compiled without error. What remains to link is: EEL2 and LICE (in WDL),
   about 50 Win32 calls that SWELL's headless build provides, and about 25
   REAPER hooks (tempo map, media insertion, pin mapping) that become null
   stubs. The window code (`sfxui.cpp`, `sfx_edit.cpp`: the slider dialog,
   the editor, the watch list) is not needed and is left out.
3. **`@gfx` can already run without a window.** REAPER's embedded UI path
   (`sx_inlineEditor`) calls `setup_frame (NULL, rect, mouseX, mouseY)`,
   runs the `@gfx` code and blits LICE's frame buffer. The client does the
   same into a `juce::Image`.
4. **The memory's block size cannot shrink.** FFTs and the block memory
   functions need a buffer inside one 65536-item block, and scripts rely on
   it. So "which memory does `@gfx` read" is known per block (512 kB), not
   finer, unless the code generator changes.

## The engine: the Cockos sources, as a library

A new static library, `fxjsfx`, built by CMake from the fork:
`effectload.cpp`, `effectproc.cpp`, `miscfunc.cpp`, EEL2, LICE, the headless
part of SWELL (macOS: SWELL's Cocoa GDI for the fonts; Windows: the native
API), plus one new file in the fork, `jsfx_host_embed.cpp`, that replaces
`sfxui.cpp`: the `sx_*` entry points that sat there, the REAPER hooks as
stubs, and the additions below. Changes to the existing files stay small
and inside `#ifdef JSFX_EMBED`, so the fork still merges with Cockos'.

Additions to the JSFX sources (the "functionality to add"):

- **load with the error:** `jsfx_embed_create` loads a script as
  `sx_createInstance` does and, when it does not compile, gives the message
  with its line. (The script still comes from a folder: the host writes the
  bundle it received to a temporary folder, `Effects/` and `Data/`, and
  loads from there. This keeps imports, images and data files working with
  no change to the loader.);
- **gfx without a window:** `setup_frame` and `gfx_runCode` exposed, with
  the mouse and keyboard state passed in, the frame buffer handed out;
- **state access for the mirror:** enumerate the named variables with
  stable indices (the same script compiled on both sides gives the same
  table, checked by a hash of the names), read and write ranges of memory
  without allocating, list the blocks the VM has allocated, enumerate the
  mutable strings;
- **role:** an instance is `audio` (server: `@gfx` is compiled for the
  variable table but never run) or `display` (client: `@init`, `@slider`,
  `@block`, `@sample` are compiled but never run).

Licence: JSFX is LGPL 2.1. Inside the AGPL build this is fine. For the
commercial licence, the LGPL asks that the user can relink: either `fxjsfx`
is a shared library next to the plugin and the server, or the object files
are offered. To decide before a commercial build ships.

Alternative considered: ysfx (a reimplementation for embedding, Apache 2).
It is already a library, but it is not the reference implementation and
not the fork; scripts behave "almost" like in REAPER. Kept as the fallback
if SWELL's fonts turn out to be a problem on one of the three systems.

Build dependency: the fast EEL2 back end needs `nasm` on Linux x86-64
(prebuilt objects exist for Windows and macOS). To add to the CI.

## In the chain: a JSFX is an `AudioPluginInstance`

`Chain` holds `juce::AudioPluginInstance`s. A new class on the server,
`JsfxInstance`, derives from it and wraps an `SX_Instance` in the `audio`
role:

- parameters: one per slider, with its name, range, step and enum values
  (so the 256-entry automation pool, the mapping tab and the generic
  controls work unchanged);
- `processBlock`: `sx_processSamples` (doubles, interleaved; one
  conversion buffer), MIDI in and out through the script's midi hooks, the
  play head given as tempo and position;
- latency: the script's `pdc_delay`; buses from `in_pin` / `out_pin`;
- state: the script bundle (text, imports, resources) and
  `sx_saveState` / `sx_saveSerState`.

Routing, bypass, latency compensation, session statistics and
fxmenet-top's view need no change: to the rest of the server it is a
plugin.

## Where scripts come from

The client is the source: the plugin scans JSFX folders on the DAW's
machine (REAPER's `Effects` folder by default, others added in Settings)
and lists them under the server's plugins, tagged `JS`. Adding one sends
the bundle; the bundle is saved in the DAW project, so a project is
self-contained and reopens on any server. (A script library on the server
can come later; it is a second source for the same message.)

Bundle: the main script, the files it imports (resolved on the client),
and its resources (images for `@gfx`; data files for file sliders, sent to
the server only when small, to decide). The server keeps bundles in memory
per session.

## Protocol additions (version 2 of the control channel)

| Message | Direction | Content |
| --- | --- | --- |
| `SLOT_ADD` | client | as today (phase 2: no message of its own). A script is the uid `JSFX`, the vendor `"JSFX"`, its path as the name, and the bundle inside the state blob (`Common/FxmeNet/jsfx/JsfxBundle.h`) |
| `SLOT_READY` | server | as today, plus `kind: "jsfx"`, `hasGfx` and a range per slider (phase 2); the variable table hash (phase 3). A script that does not compile is an `ERROR` with the message and its line |
| `TELEMETRY_ACTIVE` | client | as for VST3 slots: the display is open. On (also on again) restarts the slot's mirror: its next `JSFX_SYNC` is a reset |
| `JSFX_SUBSCRIBE` (41) | client | slotId, the memory blocks the display touched |
| `JSFX_SYNC` (42) | server | slotId, frame number, [reset: the variables' names], changed variables, changed memory ranges (strings: phase 5) |
| `JSFX_WRITE` (43) | client | a `JSFX_SYNC` without reset: the variables (server indices) and memory ranges `@gfx` changed while the user acted on the display |

Over TCP, not the telemetry datagrams: the sync is a delta, so it must
arrive complete and in order.

Bandwidth is bounded twice: a cap per editor and per frame (default 64 kB,
about 15 Mbit/s at 30 frames a second; a setting), and a total per client
that the open editors share, so that many floating script windows cannot
add up. What does not fit waits for the next frame, oldest first: a script
over its budget gets a slower display, not more bandwidth. Closed editors
send nothing.

## The mirror: which data moves, and how it is found

No static analysis of the script. The two sides find the data at run time:

**Variables (server to client).** While the editor is open, a sync thread
on the server compares the VM's variables with the copy it last sent, 30
times a second, and sends the changed ones (index, value). A few thousand
doubles at most: microseconds, and a few kB per frame.

**Memory (server to client), on demand.** The display VM starts with no
memory. When `@gfx` first touches a block, the VM allocates it (zeroed),
the client notices the new block after the frame and subscribes. The server
sends the block once, then, each frame, the ranges that changed since
(compared in 256-item chunks with a shadow copy). So:

- memory `@gfx` never touches costs nothing;
- a touched block that does not change costs nothing after the first send;
- a touched block that the audio code writes costs what the audio code
  writes: a stereo delay line advances about 3200 items in 33 ms, 26 kB per
  frame, 6 Mbit/s. Acceptable on the wired network FxmeNet asks for, and
  only while that editor is open.

The first frame after a new block is touched draws zeros; the next one is
right.

**Client to server.** After each `@gfx` run the client compares its
variables and touched blocks with what they were before the run. What
`@gfx` changed goes to the server in `JSFX_WRITE`, and the server applies
it (and updates its shadow, so it is not echoed back). This carries the
cases where `@gfx` is the user interface: a mode flag toggled by a click, a
curve edited with the mouse. Variables that only `@gfx` uses (`gfx_*`,
`mouse_*`, the frame buffer state) are excluded by name.

**Sliders.** `@gfx` writing a slider and calling `slider_automate` or
`sliderchange` becomes the existing parameter message (with gesture begin
and end, so the DAW records automation). The other way, the server already
reports parameter changes.

**Conflicts.** A cell written on both sides in the same frame: the last
writer wins, as between REAPER's two threads.

`gmem[]` and strings came later with the same mechanism (phase 5).

Known limit: `@gfx` code that computes from memory it never read before
(a new region each frame) is always one frame late on that region.

## The client's view

- **no `@gfx`:** generic controls, as for tier 0.
- **`@gfx`:** a `JsfxView` component: the visible sliders as generic
  controls on top (as REAPER shows them; hidden ones left out, at most ten
  rows, then they scroll), the `@gfx` area below. A timer
  at the script's rate (30 Hz by default, `gfx_hz` honoured up to 60)
  applies the last `JSFX_SYNC`, runs `@gfx` in the display instance, blits
  the LICE frame buffer to a `juce::Image`, and sends what changed. Mouse,
  wheel and keys feed `mouse_x`, `mouse_cap`, `gfx_getchar`.
  `gfx_w` / `gfx_h` follow the component; `gfx_ext_retina` follows the
  screen scale.
- The display instance exists only while the editor shows that slot. The
  cost on the client is the drawing code and the memory comparison.

## Phases

1. **Engine (written, Linux built and run 2026-11-12).** In the fork:
   `CMakeLists.txt` (the static library `fxjsfx`), `jsfx/jsfx_embed.h`,
   `jsfx/jsfx_host_embed.cpp`, `tools/jsfx_run.cpp`. No existing file of
   the fork is changed. `jsfx_run` loads a script from REAPER's resource
   folder, lists its sliders, runs a sine through it and draws one `@gfx`
   frame to a PPM file. Checked: `utility/volume` at -6 dB halves the level,
   `delay/delay` runs, `analysis/gfxanalyzer` draws the sine's peak with its
   labels (so LICE, the headless SWELL and freetype text work without a
   window). The Windows part of the CMake file was built and tried later
   (2026-11-17); the macOS part is built by the release workflow only.
   WDL's zlib needs `HAVE_UNISTD_H` outside Windows (GCC 14 and later
   refuse its undeclared `read`/`close`). Not done: a comparison with REAPER's render of the same
   script; the display role and the state access for the mirror (they come
   with phase 3).
2. **Server and generic controls (written 2026-11-12).** No new message:
   a script is a `SLOT_ADD` whose state blob carries the bundle, so the
   client's slot model, the project state and the rebuild after a
   reconnection needed no change.
   - Common: `jsfx/JsfxBundle.h` (the bundle and the slot state on the
     wire, path rules, limits; JUCE-free, tested in FxmeNetProtocolTests),
     `jsfx/JsfxScripts.h` (finding scripts in an Effects folder; packing one
     with its imports, its `filename:` files and its file sliders' data).
   - Server: `JsfxInstance` (an `AudioPluginInstance`: sliders as
     parameters with their ranges, pins as channels, MIDI through the
     engine's hook, `pdc_delay` as latency, bundle + sliders + `@serialize`
     as state), created by `Session::handleSlotAdd`. `FXMENET_JSFX` (CMake,
     ON) leaves it out.
   - Plugin: `JsfxLibrary` (the scan, shared by the instances), the scripts
     listed after the server's plugins with the tag `JS` (the filter also
     matches their description), "JSFX SCRIPTS" in the Settings tab (count,
     folders, rescan), slider values shown in the script's units.
   - fxmenet-wavclient: `--jsfx EFFECTS_FOLDER SCRIPT_PATH`.
   - Run on Linux with the wavclient against a local server:
     `utility/volume`, `delay/delay` (echoes build up), `analysis/gfxanalyzer`
     (reported with `@gfx`); a script with a syntax error is refused with
     "@sample:4: syntax error"; the temporary folders are removed.
   - Tried later in a DAW (list, add, sliders, automation, state in the
     project), on Linux and Windows. Hidden sliders came in phase 5. Not
     done: scripts outside the searched folders.
3. **Display, read only (written 2026-11-12).**
   - Fork: `jsfx_embed_set_display` (an instance where no audio code runs),
     `jsfx_embed_enum_vars`, `jsfx_embed_var`, `jsfx_embed_mem_block`,
     `jsfx_embed_mem_blocks_used`, `jsfx_embed_lock`, `jsfx_embed_gfx_request`;
     `jsfx_run --mirror` draws from a display instance fed by copies of an
     audio instance's state. Checked: `analysis/gfxanalyzer` (129 variables,
     1 block) and `analysis/gfxscope` (111 variables, 1 block) draw the same
     as from the audio instance itself.
   - Common: `jsfx/JsfxSync.h`: the codec of the two messages and
     `SyncSource` (the server's copy of what the client has; changed
     variables by their bits, memory compared in chunks of 256 items, a
     byte budget per frame with the blocks served in turn). Tested in
     FxmeNetProtocolTests (a budget of 8 kB brings a whole block across in
     several frames, the copy ends equal).
   - Server: `JsfxInstance::syncReset / syncSubscribe / syncCollect` (the
     engine's lock held while memory is read), `Session::pumpJsfxSync` at
     the telemetry rate, 64 kB per slot and 256 kB per session per tick,
     counted with the telemetry in fxmenet-top.
   - Client: `ClientEngine` queues `JSFX_SYNC` per slot (300 at most, then a
     restart), `JsfxView` (Plugin/Source) shows the display in the
     Parameters tab in place of the plugin editor ("Generic controls"
     switches), sized as the script's `@gfx w h` line asks, at its gfx_hz.
     Scripts with `gfx_ext_retina` draw at the screen's density. The mouse
     position reaches `@gfx` (hover); clicks do not.
   - Limits: a block first touched draws zeros for one frame. (Strings
     left by `@init` and `gmem[]` were missing here; phase 5 brought them.)
4. **Display, interactive (written 2026-11-12).**
   - Fork: `jsfx_embed_gfx_frame` takes `mouse_cap` (buttons and modifiers
     from the host), `jsfx_embed_gfx_wheel`, `jsfx_embed_gfx_key` (the queue
     `gfx_getchar()` reads and the keys held), `jsfx_embed_slider_var`. In
     WDL, `eel_lice.h`: with `EEL_LICE_EMBED_NOWINDOW` (fxjsfx only),
     `gfx_getchar()` reads the queue without a window instead of answering
     -1 ("closed"), and `gfx_getchar(65536)` reports the display focused and
     visible. REAPER's own build of the fork is unchanged.
   - Common: `SyncSource::applyWrite` (into the script, and into the copy
     of what the client has, so nothing comes back).
   - Server: `JSFX_WRITE` (43), `JsfxInstance::syncWrite` under the engine's
     lock.
   - Client: `JsfxView` feeds the mouse (position, buttons, modifiers,
     wheel) and the keys (it takes the keyboard focus on a click). After
     each `@gfx` run:
     - a slider that changed goes as its parameter (`setSlotParameter`, in a
       gesture while a button is held, "last touched" for the mapping), so
       that `@slider` runs on the server and the DAW records it;
     - while the user acts on the display and for 500 ms after, the other
       variables (not `gfx_*`, `mouse_*`) and the memory blocks it reads (8
       at most) are compared with what they were before the run, and what
       changed goes as `JSFX_WRITE`. Outside of that, what `@gfx` changes is
       taken as its own scratch and stays here.
   - Conflicts: a value written on both sides in the same frame takes the
     last writer, as between REAPER's two threads. Loop counters shared by
     `@gfx` and the audio code travel during an interaction, as they race in
     REAPER.
   - Left at the time, all done in phase 5: images named in the code, the
     cursor, `gfx_showmenu`, files dropped on the display.
5. **The rest (essentials written 2026-11-13).**
   - Strings: numbered (0..1023), named (`#name`) and anonymous (`#`)
     strings travel whole when they change, in `JSFX_SYNC` and `JSFX_WRITE`
     (`jsfx_embed_enum_strings` / `jsfx_embed_set_string`, in
     `effectproc.cpp` under `JSFX_EMBED`). Text that `@init` builds in
     strings now reaches the display.
   - `gmem[]`: its blocks are mirrored as blocks numbered from 4096
     (`jsfx_embed_gmem_block`), for a script whose code names `gmem` (the
     buffer is shared by every script of the process, so "allocated" does
     not mean "read by this one"). On the server, the scripts of every
     session share it, as REAPER's do in a project.
   - File sliders: `JSFX_SUPPORT_WAV_DECODING` for `fxjsfx` (REAPER reads
     media with its own API; the fork's own `.wav` reader needed a one-line
     fix in `miscfunc.cpp`): `.wav`, `.txt` and `.raw` files are listed and
     read. A file slider is a list of its files' names in the generic
     controls. Checked with `guitar/amp-model` (12 models, chosen by name,
     the sound changes).
   - Other formats (2026-11-16): the client converts the data files the
     engine cannot read (FLAC, AIFF, Ogg, MP3, floating-point or 32-bit
     .wav; mono or stereo) to 24-bit .wav while packing the script
     (`Common/FxmeNet/jsfx/JsfxAudio.h`, JUCE's readers); the slider shows
     the same names. Images and data files may now take 48 MB together
     (the whole script 56 MB, under the control frame's 64 MB); they are
     saved in the DAW project with each instance of the script.
   - Hidden sliders (2026-11-16): a slider named `-name` is a parameter
     (automatable) left out of the generic controls; `slider_show()` at run
     time is not followed.
   - Display features without a window (2026-11-16): `gfx_showmenu`,
     `gfx_setcursor`
     (Windows cursor ids to JUCE cursors), `gfx_getdropfile` (files dropped
     on the display; their paths are this machine's), images named in the
     code (`gfx_loadimg (0, "knob.png")`: quoted .png/.jpg names found beside
     the script or in Data are sent). Hooks in `eel_lice.h` under
     `EEL_LICE_EMBED_NOWINDOW`; `jsfx_embed_set_gfx_host`, `jsfx_embed_gfx_drop`.
   - Menus without blocking (2026-11-16). A modal menu (JUCE's own loop
     inside the plugin) froze REAPER on Linux, so the menu is replayed
     instead. The first time `@gfx` calls `gfx_showmenu`, it gets 0 and
     the request is kept; after the frame the display opens the menu
     asynchronously and pauses. On a choice, the script is put back as it
     was before that frame (variables, the memory blocks of the snapshot,
     strings) and the frame runs again with the same mouse state, this
     time `gfx_showmenu` returning the choice; what that frame changes
     goes to the server as usual. Nothing chosen: the menu just closes.
     Limit: the replay needs the interaction snapshot, which a click or a
     key provides; a menu opened without input (from a timer in the
     script) is shown but its choice is ignored.
   - Sliders and display together (2026-11-16): as in REAPER's FX
     window, a script with `@gfx` shows its visible sliders above the
     display, in the same pane (`ParameterPanel::jsfxRowsHeight`, at most
     ten rows or half the height, then they scroll); "Generic controls"
     still shows the sliders alone.
   - Variables the audio code keeps setting (2026-11-17). A script such as
     `analysis/gfxanalyzer` sets `update = 1` in `@sample` and clears it
     in `@gfx` after drawing. On the server `@gfx` never runs, so the value
     stays 1, never changes, and is never sent again: once the display had
     cleared it, the spectrum stopped. The display now reads the script's
     `@sample` and `@block` sections (a lexical scan: `name =`, `name +=`,
     ..., not memory) and, before each frame, puts those variables back to
     the server's last value, as the audio code would in REAPER. A value
     the display writes to the server counts as the server's last value.
     Limit: variables assigned only inside functions called from there are
     missed (todo: only when a script needs it). While no audio flows the
     display keeps redrawing its last data (REAPER would stand still).
   - Not done: presets (`.rpl`), a script editor in the plugin.

Each phase ends usable. Phases 1 and 2 carry little risk (the throwaway
build covered the main unknown). The open risks are in 3 and 4: SWELL's
fonts in a plugin on three systems, and scripts with unusual memory use.

## Test scripts

From REAPER's stock set, by what they exercise: `utility/volume` (sliders
only), `delay/delay` (memory, no `@gfx`), `analysis/gfxanalyzer` and
`analysis/gfxspectrograph` (FFT in memory, read by `@gfx`),
`dynamics/general_dynamics` or a compressor with a curve (interactive
`@gfx`), `meters/vumeter` (variables only), a MIDI script
(`midi/midi_transpose`).
