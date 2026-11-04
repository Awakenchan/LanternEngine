# Lantern Engine without emulation

The goal: Lantern stops being "shadPS4 with patches" and becomes an engine that runs Bloodborne's
own code on our own system layer and our own renderer. This is the top-level plan. The renderer's
details are in `GX_NATIVE_PLAN.md`, the module-by-module rewrite in `OWN_CODE_PLAN.md`.

## Progress (updated with every step)

Last update: 2026-11-03, after beta 30 (0.9.39-beta.30; betas 25-30 changed no native code): the native switches ship on by default, and the old renderer still does everything phases F and H cover. Keep this section short: what is true now, one line
per step. Details belong in the commit that did the step.

| Phase | State |
|---|---|
| A. Tools (Ghidra, full dump, GX facts) | done 2026-10-29 |
| B. GX trace | done: 0 differences over 5.36 million draws in three areas; **a boss is still to check** |
| C. GX objects | done: every bound object known over 2.2 million draws |
| D. Draws from GX | **in progress, on in beta 24; the record path (`LANTERN_GX_NATIVE`, bindings, render state, vertex, index and resource buffers by object) on by default from beta 31.** The resource bind is Lantern's for 90% of binds (`LANTERN_GX_DRAWS`), 0 differences. `LANTERN_GX_RECORDS` picks a draw's pipeline from a GX record (`=2` on 2026-10-31: 411,449 pipelines, 0 differ). Since 2026-11-03 the record also carries the draw's bindings and render state: `=3` takes them from it (9.1 million draws, every one covered, nothing drawn wrong) and fills each shader's user data without the table walk, but stage A still decodes GX's register packets too, so no FPS gain (pipeline 1.4-1.5 to 1.6-1.7 us a draw); `=2` compares; `=1` stays the default |
| E. Buffers and textures by object | **in progress, on in beta 24.** Textures, depth targets and display buffers come from GX objects and a texture holds its image (`LANTERN_GX_TEXTURES`); 99.99% of lookups map to an object, and 3,631 of 4,377 first lookups in one boot skip the resolver. GX's upload arena stays in place (`LANTERN_GX_BUFFERS`): write faults 5,500 a second to 0 standing still. The old search and resolver still take the rest, so `texture_cache` and `buffer_cache` cannot go yet |
| F. Compute, clears, copies, queries | not started |
| G. Shaders | **in progress, on in beta 24** (`LANTERN_LIFTER`): all 9,847 programs lift, 0 different from the recompiler on the dev cache for every family profile. Install-time builds are not done |
| H. YEBIS, Scaleform, flip pass | not started; Scaleform's renderer is still to find |
| I. The translator goes | not started; needs D-H as the default everywhere |
| J. System libraries | done except GnmDriver (waits for the GX path) and online (the user's call). 302 of 537 imports are ours; 6a-9 ship in beta 24, no dedicated play-through recorded |

**Proof so far for beta 24's defaults:** the user booted it with native on and it worked. No AMD or older
NVIDIA player has confirmed it yet, and no FPS gain has been measured for the whole set: benchmarks are
paused until the native path is built. Safe mode and the launcher's Engine page turn each switch off.

**System library imports** the game and its modules need from us (537, from
`tools/re/import_census.py`; libc and Fios2 are the game's own Sony modules and not counted):

| Served by | Imports |
|---|---|
| **Lantern, `src/sys/`** | **302** (56%) |
| shadPS4, `gpu/shadps4/core/libraries/gnmdriver`: GnmDriver (waits for the GX path) | 50 |
| Online, `gpu/bbnet`: Net, NetCtl, Http, Ssl, Np* (The Hunter's Dream and shadNet: the user's call; offline, `src/sys/net_offline.c` answers) | 165 |
| The game's libc module, or unresolved | 20 |

Phase J log (steps 1-5d played by the user, work; 6a-9 had one unattended boot, not played yet):

| Step | Moved to `src/sys/` | Imports | Checked against the old code |
|---|---|---|---|
| 1 | Rtc | 10 | 2,000,000 random calls |
| 2 | Services, dialogs, PlayGo, DiscMap, Mouse, Voice, AudioIn, AppContent, Sysmodule, `gettimeofday` | 72 | 347-line transcript |
| 3 | Pad, AudioOut, Ajm | 27 | 90,000 pad reads, audio bytes, ~8,500 ATRAC9 jobs |
| 4 | SaveData | 11 | scripted session, final 52 files |
| 5a | Semaphores, rwlocks | 14 | threaded transcript, stress |
| 5b | Mutexes, condition variables | 30 | 256-line transcript, stress, Windows and Linux |
| 5c | Threads, thread-specific data, once, `__error` | 38 | 174-line transcript, Windows and Linux |
| 5d | Clocks, sleep, signals, process calls | 24 | 293-line transcript, Windows and Linux |
| 6a | Files; `src/runtime_file.c` and the switch are gone | 24 | 1,378-line transcript (1,367 on Linux; every path, flag and error, bad and standard fds, bounced reads, listings, mods, fixes, mount limits, fd limit, loader stats, prefetch): identical apart from the line naming the path. 72 of 73 planted bugs caught, the other changes nothing (`/hostapp` and `/app0` are one folder). Close during reads under stress, clean under TSan |
| 6b | Kernel memory; `src/runtime_memory.c` is gone | 16 | 3 seeds of 44,884-line transcripts identical; 2,000 regions: allocate 560 to 2 us, 8 reader threads 18 to 255 million lookups a second, one reader 40 to 50 million, clamp 107 to 60 ns; map and release no slower; 80 planted bugs, none that changes behaviour missed; ThreadSanitizer clean |
| 7 | C start-up; `src/runtime.c` and `src/runtime_services.c` are gone (the rest moved to `src/host.c`, `wait_stats.c`, `blocked.c`) | 9 | unit test, the whole suite on Windows and Linux |
| 8a | Event queue; shadPS4's equeue is gone, and with it the kernel service thread | 3 | 3 seeds of 40,008-line transcripts identical; empty poll about equal (92 ns), trigger and poll 100 to 70 ns; 32 planted bugs caught; ThreadSanitizer clean |
| 8b | VideoOut, state machine in `src/sys/video_out.c`, presenter in `gpu/shim/lantern_video_out.cpp` | 8 | unit test with a fake presenter, 60 planted bugs (the old driver needs the whole renderer, so no old program); ThreadSanitizer clean |
| 9 | AvPlayer, players in `src/sys/av_player.c`, FFmpeg in `gpu/shim/lantern_av_decode.cpp`; shadPS4's avplayer and videodec are gone | 16 | the game's own movie: all 4,905 video frames and 7,675 audio frames identical, plus 2 tail frames the old one lost, CPU equal (115% of a core); stress of 34 starts, 102 million calls; 74 planted bugs; ThreadSanitizer clean |

Known inherited issue, kept for now: when gapless playback ends inside an ATRAC9 superframe, Ajm
decodes the superframe's remaining frames without checking that the input buffer holds them. The
game passes whole superframes, so it does not happen in play; fix it on its own with a test.

## What we are today

The game's CPU code already runs natively: a PS4 is x86-64, so the eboot runs on the PC's CPU as
it is. Decompiling it and compiling it again would buy no speed. What makes Lantern look like a
wrapper is everything under the game:

| Layer | Today | Inherited lines |
|---|---|---|
| GPU: PM4 command processor, register state, texture and buffer caches | shadPS4 `video_core` | ~65k |
| Shaders: GCN to SPIR-V | shadPS4 `shader_recompiler` (our native compiler beside it) | ~40k |
| System libraries (kernel, pthreads, Gnm, Np, Net, audio, pad, saves) | bbport runtime and shadPS4 `core/libraries`, ours in part | ~20k |
| Online pool | shadNet | ~54k |

The game draws through GX, FromSoftware's Direct3D 11-shaped layer. GX turns each call into Gnm
packets; the renderer then reads those packets back and guesses what the draw was. Both sides of
that do work only because of the console: the game encodes, we decode.

## Where we are going

```
Bloodborne's code (native, unchanged)
  ├─ GX device and context calls ──▶ Lantern GX: our objects and Vulkan, at the call
  ├─ YEBIS / Scaleform / flip pass ─▶ the same, through their own context calls
  ├─ GCN shader binaries ───────────▶ our lifter, built to SPIR-V at install
  └─ system library imports ────────▶ Lantern's own library layer
```

No PM4 parsing, no GPU register emulation, no address-and-hash texture guessing, no memory watching
to find what the CPU changed. The PM4 path stays behind a switch until the GX path has covered
everything for a release, then it is deleted.

Why this is the optimized way and not just "our own":

- **Half the CPU work goes.** The game no longer encodes packets for us to decode. Each GX call
  becomes a Vulkan command (or a state change) directly.
- **Objects, not addresses.** GX creates every buffer, texture, view, shader and state object
  through a known creator with a D3D11-style description. Each becomes its Vulkan object at
  creation, so the texture cache's guessing, the write faults and re-protects and the detiling
  on the fly all go.
- **Threads the game already has.** GX's deferred contexts record on the game's worker threads and
  the immediate context replays them. That maps onto Vulkan secondary command buffers recorded on
  the same threads.
- **Pipelines known ahead.** Shader and state objects are known at creation, so every pipeline can
  be built at install or at load, not mid-frame.

## The decompilation is our map, not our code

We do not recompile the game from decompiled C. The decompiler shows us which function does what
and how GX's structures are laid out, so our hooks and our replacement GX are right.

Tools (all ours, in `engine/tools/re/`):

| Tool | What it does |
|---|---|
| `ghidra_elf.py` | turns the player's decrypted eboot into a plain ELF Ghidra loads: applies the 231k pointer relocations, names 687 of the 703 imports from `src/import_names.inc`, lists the 162k functions from the eboot's unwind table |
| `ghidra/LanternSeed.java` | creates those functions and import names before analysis |
| `ghidra/LanternDecompile.java` | decompiles the functions asked for, or exports the function list |
| `ghidra/LanternDump.java` | decompiles every function into one file per 1 MB of code |
| `ghidra.sh` | `import` (convert and analyse, ~50 min), `decompile <addr>...`, `dump` (~45 min) |
| `dumpfn.py` | prints a function from the dump by address, `--depth N` follows its calls |
| `gx_facts.py` | checks every GX fact we rely on against the eboot (must print `0 failed`) |

Setup: Ghidra 12.2 and Temurin JDK 25 in `E:\re-tools` (Ghidra 12.2 needs JDK 25). The project is
`E:\re-tools\projects\Bloodborne`, the dump `E:\re-tools\work\dump` (158,440 functions, 7 the
decompiler could not do). The heap is 6 GB (`GHIDRA_HEADLESS_MAXMEM`); 10 GB ran out of memory on
16 GB of RAM. Ghidra's settings go to `E:\re-tools\settings`, because its plugin framework does not
start from the default settings folder on this machine.

Rules for it:

- **The dump and the Ghidra project never go into git or a release.** They are the game's code.
  Only facts (addresses, offsets, sizes, what a function does) go into our files, each checked by
  a script like `gx_facts.py`.
- Our code is written fresh from those facts, as for the bbhost rewrite.
- Addresses are image offsets (the eboot at 0). Paleblood and bbhost use a 0x400000 base.

## Phases

Each phase is behind a `LANTERN_*` switch with the old path kept, and is checked under Vulkan validation
for every GPU family. Betas ship every native switch on (the user's call, beta 24): `=0`, the launcher's
Engine page or safe mode turns one off. A stable release needs AMD and older NVIDIA players to confirm it.

### A. Tools (done 2026-10-29)

The Ghidra project, the full dump, and 162 GX facts checked. GX's pending state, its 36-command
replay table, its creators and its deferred context are mapped (`GX_NATIVE_PLAN.md`).

### B. GX trace (`LANTERN_GX_TRACE`): small

Hook the 4 input-assembler setters, the 24 stage setters, the output-merger setters, the 8 draws,
Dispatch and Commit. Count each per frame, and at each draw compare the pending state with what
the PM4 path resolves for the same draw. Zero differences in Central Yharnam, Cathedral Ward, the
Hunter's Dream and one boss are the gate to C.

The four view creators and the geometry shader creators are named (`GX_NATIVE_PLAN.md`). Before
coding: find the four state-object creators and the rasterizer and viewport slots in the dump with
`dumpfn.py` and add them to `gx_facts.py`. The trace itself names the other five shader creators'
stages (which creator made what each SetShader receives).

### C. GX objects (`LANTERN_GX_OBJECTS`): medium

Hook the creators and releases. Each GX object gets an entry in our tables with a generation:
buffers and textures with their memory, format and size, views with their resource, shaders with
their GCN binary, state objects with their description. Find Map/Unmap and UpdateSubresource and
every place the game writes a resource's memory without them (those still need watching).

### D. Draws from GX (`LANTERN_GX_DRAWS`): big

At a draw, the pending state plus the object tables give the whole draw. It is recorded as our own
record and the command buffer only keeps a token for ordering. A draw the record cannot cover goes
the PM4 way, and the log counts those by reason until the count is zero.

### E. Buffers and textures by object: big

Immutable buffers live in VRAM from creation, dynamic ones in a host-visible ring, textures as
Vulkan images from creation, filled by GX's upload and copy calls and by the shaders that write
them. This replaces most of `buffer_cache` and `texture_cache` and the memory watching
(`EMULATION_REMOVAL_PLAN.md` steps 4 and 6 become this).

### F. Compute, clears, copies, queries, completions: medium

Every remaining GX call as a token. The labels and fences GX waits on are written when our
submission has run. After this the PM4 walk only orders tokens.

### G. Shaders built at install: big

The lifter takes each shader object's GCN binary to structured SPIR-V (our native compiler is the
start). If, as expected, every shader comes from the game's files (to confirm by logging the shader
creators' binaries against the files), every one is built at install and validated per GPU family. Pipelines are built at load from the shader and state objects.

### H. YEBIS, Scaleform and the flip pass: medium

YEBIS has its own D3D11-shaped context (vtable `0x530a380`, 133 methods): the same approach through
its methods. Scaleform's renderer and the Gnmx flip pass are found in the dump and moved the same way.

### I. The translator goes: deletion

With D–H the default everywhere, `video_core`'s PM4 processor, register state, SRT walker,
constant-engine emulation, GPU-write tracking and address-keyed caches are deleted, and so is
shadPS4's `shader_recompiler`. That is about 100k inherited lines.

### J. Our own system libraries: started 2026-10-30

`tools/re/import_census.py` lists the imports by library and which file serves each (`--list` names
the functions). Our libraries live in `src/sys/`, one file per Sony library, each a table keyed by
NID (`SYS_LIBRARY`); `runtime_resolve` asks `sys_resolve` first. Each is checked against the old one
before the old one goes: a scratch program runs both on the same script or random input, planted
bugs must show, and a unit test stays in `build.sh --test`.

What is left: GnmDriver shrinks as the GX path lands (after I only what GX calls at start), and
`gpu/shadps4/common` goes with `video_core` and `shader_recompiler`, its only users.

Every step needs an in-game check by the user before the next one depends on it.

## Lighting effects: what the native path must carry over

The optional lighting effects (`gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer_effects.cpp`, every
one off by default) work under `LANTERN_GX_DRAWS`, `LANTERN_GX_TEXTURES`, `LANTERN_GX_BUFFERS`,
`LANTERN_GX_RECORDS`, `LANTERN_READ_FILTER` and `LANTERN_LIFTER`, because they read what `DrawRecord` has
already resolved, whichever way it came. Phases F, H and I remove that function. Each hook names what the
native path has to give it. Pixel shader hashes stay valid while shaders come from the game's files.

| Hook, on pixel shader | Takes from the draw | Native path must keep |
|---|---|---|
| Shader choice at module creation: SSAO `cbbca5dc`, SSR `6c2323c2`, motion blur `29e06868`; SPIR-V patches on the SSR composite `3fe9e964`, water `e3021062`, POM | program hash, first binding, resource counts, and for patches the exact words | a place between lifting and module creation; the patches match words, so a changed recompiler or lifter profile leaves them off (phase G: move them to the lifter's IR) |
| G-buffer pass (5+ color targets) | depth, albedo and normal targets, the camera constants | texture objects for those targets; `CameraMotion` from GX's constant buffer |
| Shadow resolve `f55643de`, then blur `11ffdfb5` | target, constants, uniforms, shadow atlas view and layout; the blur's source address | the resolve as a GX draw with its bound objects; passes inserted after it and before the next draw |
| Scene before transparents, first blended draw | scene color and depth | a "transparents start" point (phase H has the scene order) |
| Light grid `370174f7` and the other tile shaders | point shadows: both light targets, constants, lights, the tile from the game's quad. Lamp fog: the lights and camera read from guest memory, once per light | the tile's lights and quad as GX objects |
| Directional table `d92266b9`, sky dome `dda49e89`, HDR post `ccbf44a6`, scene target `32e47788` | lighting table, material, sky vertices, scene color | points in YEBIS's flip pass (phase H): volumetrics composite before its first post copy |
| SSR draw and composite | constants, normals, scene color; swaps the composite's reflection image (slot 6) | binding override on a native draw's image slot |
| Push constants `motion_param` (byte 120), `buf_offsets` (byte 80) | strength or frame number; each binding's offset remainder as one byte, read packed | same layout for replaced shaders, or a push block of their own |

Passes stay compute passes on our own images, created on first use; they need `Runtime::Transit`, the
scheduler's `EndRendering` and `SceneTargets` when the scene is reduced. Eboot writes for shadow sizes
(`graphics.cpp`) use checked addresses and do not depend on the renderer.

## Order and what comes next

**Left:** 7 of 10 phases are open (D, E, F, G, H, I, J's GnmDriver). Since 2026-11-04 the steps are built back to
back, each behind a switch with the old path kept, and tested in batches: full native first, then tuning (the user's call). D has 1 step left (d), which waits on stage B. A gain comes only when a step
removes old work: the record so far was added on top of the PM4 decode.

1. **D:** (a) done 2026-11-04: the fill as a compiled list per shader took stage A from 5.8-7.3 to 4.1-5.1 us a
   draw (record 2.0-2.3 to 0.54-0.65 us), the user felt the FPS gain; (b) done, run 2026-11-04 (16.9 million draws, 0 register, target or index differences): render targets and the
   depth target as GX's writers set them, the vertex V# table from GX's bind, the index buffer compared, and `=3`
   carries only the registers that differ, built only when the render state's hash changes; (c) re-scoped 2026-11-04:
   GX's flushes (`0x26d2230`, `0x26d3370`) already compare each state with the last value written and write only what
   changed, so skipping their packets saves little. Instead the pipeline memo now also keeps the draws that get motion
   vectors or position capture (fixed at startup): with `object_motion=1` they were 27% of draws and always paid the
   key (1.2 us); the memo holds 262,144 records. Run 2026-11-04: 82% of pipelines from the record, 1.6% from the key,
   key 1.21 to 0.2 us and stage A 6.6 to 4.5-5.0 us a draw, 0 differences over 4.3 million draws, about 215,000 draws/s.
   Stage A now waits on stage B (stage B busy 97%, stage A waited 2%), so (d), the constants (1.4 us in stage A), waits
   too: the next gain is in stage B, whose bind (textures 0.69, ObtainBuffer 0.60, buffers 0.40 us a packet) is **E**.
   Correction the same day: stage A is full as well (the GPU command thread idle 1.7%), so both need to shrink. (d) built
   2026-11-04, not yet run (`NATIVE_BUFFERS.md`): each constant buffer address has a version that GX's two update paths
   (`0x26adb90` dynamic, `0x26b3560` default, both only from UpdateSubresource) bump; the record carries it; stage A binds
   an unchanged buffer's earlier ring copy (held until the draw has run) with no checks or copy. First run: 43% of
   195 million constant buffers reused, 0 of 1.3 million checks differ, but constants stayed at 1.56 us (a GPU-capped
   scene) because every copy was hashed; now one copy in 64 is hashed and checked at its first reuse, `=2` checks
   every reuse. Second run: 40% reused, 0 differ, but constants 1.84-2.05 us against 1.36-1.44 without it: the
   version and copy lookups cost more than the copy, so it is off by default (`LANTERN_GX_CONSTANTS=1` turns it on)
   until stage A's per-binding work is gone; E first.
2. **E:** Lantern's own buffers and textures, owned by GX objects, replace shadPS4's `buffer_cache` and `texture_cache`
   (`NATIVE_BUFFERS.md`). The run on 2026-11-04 counted stage B's buffer bindings per packet: VRAM 1.27 read and 0.10
   written, stream copies 0.46, in place 0.03, uploads 0. (1) built, not yet run: each GX buffer has a version that its
   creation, release and both UpdateSubresource paths bump; the record names every vertex stream's buffer (from GX's
   slot tables at `+0x25b8`/`+0x2638`) and the index buffer, and stage B binds them through Lantern's bind table by
   object and version instead of `ObtainBuffer` (`LANTERN_GX_BINDS=0` turns it off; `=2` checks every binding, `=3`
   one in 64). (2) built, not yet run: shader resource buffers through the same table, keyed by their GX view's buffer
   object (the record lists each read-only buffer view's address and key; the GX buffer vtable `0x535f3a0` confirms the
   resource is a buffer). Run 2026-11-04 (with (2)): 60% of vertex, index and resource bindings by object, stage B's
   VRAM path 1.27 to 0.62 bindings a packet, ObtainBuffer 0.60 to 0.45 us, vertex 0.40 to 0.28 us; 17 of 214,188 checks
   differed, all large vertex ranges around loading stalls (data written into a buffer after GX made it), so the table
   now also starts over after any CPU write the memory tracker sees. (3) textures by object (a texture already holds its
   image); (4) Lantern's own GPU memory for GX's buffers and textures, so the address path keeps only memory no GX call
   describes. Then what still goes to the resolver, by reason, and the boss check left from **B**.
3. **F:** compute, clears, copies, queries and completions. After this the PM4 walk only orders tokens.
4. **G:** every shader built at install, once all are confirmed to come from the game's files.
5. **H**, then **I** in one release once the betas are clean. **J** finishes with GnmDriver at that point.

Done means: no shadPS4 `video_core` or `shader_recompiler`, no PM4 parsing in a normal frame,
every system library function the game calls implemented by us, and the credits list only the
libraries and SDKs we link.
