# Stage B across more than one thread: design notes

Stage B is the draw recording thread `bb:DrawRec` (`vk_draw_pipe.h`, `Rasterizer::RunDrawPacket`).
In heavy fights on the Ryzen 5 5600 / RTX 5060 it is the frame rate limit: 62-94 FPS,
1300-1900 draws per frame, stage B busy 89-94%, ~5.3 us per packet, ~2500 packets per frame
(13.3 ms of a 14.4 ms frame in the 69.6 FPS window of `route\p10.log`). The game's main thread
would allow ~130 FPS. This note records what stage B does per packet, which parts are order
dependent, the approaches considered for running it on more than one thread, the one implemented
(`BB_DRAWREC_WORKERS=1`, a lookahead thread that prefetches for stage B), and why the approach
that would really scale (an ownership split of stage B into two pipelined stages) is not
something I could make safe in this change.

Evidence: `route\p9.log` and `route\p10.log` (played heavy-fight sessions with `BB_SECTIONS=1`,
`BB_SAMPLE_THREADS=load`), `symprofile.py` on `bb-probe-p10.exe`, `docs/parallel_gpu.md` (the
earlier measurements of the texture binding helper and the two-stage pipeline), and the code.

## 1. What stage B does per packet today

Stage A (`shadPS4:GpuCommandProcessor`) decodes PM4, keeps the register file, runs the constant
engine, selects the pipeline (with the prepared draws of the `bb:DrawPrep` workers) and copies
small read-only constants into the constant ring. It writes a packet into the draw pipe ring: the
32-word register blocks written since the previous packet, each stage's user data and flattened
user data, the ring bindings, the vertex stream V#s, the draw parameters. Ordered tasks (EOP/EOS
fences, WriteData, DmaData, the flip) are packets too.

Stage B, per draw packet (`RunDrawPacket` -> `DrawRecord`), with the `stage B us per packet`
numbers of the 69.6 FPS window (5.29 us per packet; a packet is a draw, a dispatch or a task):

| step | what it touches | us/packet |
|---|---|---|
| apply register blocks, install user data snapshots | its own register copy (`pipe_regs`), thread-local snapshots | ~0.2 |
| `PrepareRenderState` | render/depth target descriptions (memoized per slot) | 0.17 |
| `BindBuffers` (per stage) | ring bindings (no lookup), else `ObtainBuffer`: arena table, residency, the memory tracker's page words (CPU/GPU dirty bits), uploads, `gpu_modified_ranges`, barrier tracker | 0.41 (ObtainBuffer in total 0.89, `VRAM track` 0.45) |
| `BindTextures` (per stage) | texture set memo slot (97% hits in fights: 219k hits, 5.7k misses per 5 s); per image: flags, backing, up-to-date check, layout transition; else the full lookup (image description cache, `FindImage`, views, refresh) | 0.99 (set 0.59: check 0.22, apply 0.22; full 0.27; samplers 0.07) |
| `BeginRendering` | render state memo (84% hits), else target views, barriers, scene proxies | 0.28 |
| `ResolveVertexBuffers`, `ResolveIndexBuffer` | `ObtainBuffer` for the merged stream ranges and the index range | 0.52 + 0.11 |
| `EmitVertexBuffers`, `FlushBarriers` | record chunk | 0.13 |
| object motion | bone palette hashes, index range cache (reads guest memory) | 0.26 |
| descriptors, `UpdateDynamicState`, `MarkPass`, `scheduler.BeginRendering`, the draw command | record chunk, dynamic state | ~0.3 |
| `ResetBindings`, `KickRecording` | image binding flags, buffer access tracker, hand-over to `bb:VkRecorder` | 0.14 |
| dispatches (`DispatchRecord`, the copy shader HLE) | `ObtainBuffer` for sources and destinations, copies | 0.61 per packet averaged |
| ordered tasks (fences, WriteData, DMA, flip) | waits for host copies and deferred fences, guest memory stores | 0.46 |

Already parallel: Vulkan command recording (`bb:VkRecorder` threads record the closures stage B
pushes into record chunks; `HandOver`/`KickRecording`), host copies (copy threads and the
ordered copies on the recorders), draw preparation (`bb:DrawScan`, `bb:DrawPrep`: pipeline keys,
sharps, texture description hashes, vertex inputs, all validated by stage A), and since the plan
in `perf-plan.md`, page re-protection (`bb:Protect`, `BB_DEFER_PROTECT`). The CPU profile of
stage B is flat (`symprofile.py`: `BindTextures` 4.8%, the spin in `DrawPipe::Run` 2.5%,
`RunDrawPacket` 2.4%, `DrawRecord` 2.2%, `Image::GetBarriers` 1.7%, `BindBuffers` 1.7%, nothing
else above 1.5%; ~21% in ntdll, mostly the sleeps, protects and waits the applied plan removes).
The earlier `perf` profile with call graphs (`parallel_gpu.md`, "Second pass") said the same:
no function above ~5% self time, the cost is cache misses spread over the texture, buffer and
barrier structures.

## 2. What is order dependent

Everything a draw does on stage B reads or writes state that the previous packets left:

- The register copy: a draw reads the registers at its position; only the changed blocks travel.
- The barrier tracker (`Runtime::IsBufferAccessed`/`AccessBuffer`, `Transit`, image layouts):
  the barrier a draw needs depends on what the previous draws and copies did to the same buffer
  or image, and the transitions it records change what the next draw needs.
- The buffer cache: `ObtainBuffer` turns CPU-dirty pages into uploads recorded before this draw
  and marks pages GPU-modified for written bindings; a later binding of the same range must see
  those marks, and a GPU-modified range must not be re-uploaded from stale guest data. The
  stream path copies guest memory into the stream buffer at this point of the stream: a later
  WriteData/DMA to the same memory waits for that copy (`WaitHostCopiesFor`).
- The texture cache: images are registered, refreshed, invalidated by storage writes, tracked
  and untracked (page watchers) in stream order; the texture set memo and the render state memo
  are valid exactly because nothing ran between the memoized draw and this one that the checks
  do not cover.
- The scheduler's producer side: one thread fills the current record chunk and the ordered
  chunk (`ProducerScope` reports a second one); draws, uploads, barriers, copies and fences
  must land in it in stream order, since `OrderedReady` and the deferred fences assume it.
- CPU side effects of tasks: WriteData and CPU DMA store into guest memory in order with the
  host copies of earlier draws (`WaitHostCopiesFor`) and the deferred fences (`WaitDeferredSignals`);
  later draws read what they wrote (constants, index data).
- The deferred-protect invariant (`page_manager.cpp` ~430): pages an upload copied may stay
  writable until `bb:Protect` or the copy itself applies the protection; every reader of guest
  memory on the GPU side goes through `CopySparseMemory`, which flushes pending protects first.

So a draw's result (what it uploads, which barriers it emits, which memo entries it finds valid)
is a function of the whole prefix of the stream. The only parts without side effects are
computations from the packet's own data: hashes, keys, the addresses of what will be consulted.

## 3. Approaches considered

### A. N workers each running a whole draw from a register snapshot (fork/join)

Each worker would need the caches, the barrier tracker and the scheduler's producer side; none
of them is thread-safe, and the barrier and upload decisions of draw N+1 depend on draw N. The
closest experiment that exists is the texture binding helper (`BB_TEXTURE_HELPER=1`,
`parallel_gpu.md`, "Texture binding on a helper thread"): the helper binds textures on the
memoized path while stage B binds buffers, with a join before any image state changes. 68% of
draws were bound in parallel and the frame rate did not move (85.9 vs 85.8 FPS): the helper's
task took ~6200 cycles where the same work cost ~4600 on the GPU thread and the join waited
~2500 cycles per fork, because the descriptor infos, image states and binding flags it writes
are read by stage B right after and move between cores. Moving ~7 us of cache-bound work to
another core per draw costs about what it saves. Gain ~0, risk high. Rejected.

### B. Pipelining stage B into two owners (B1: decode, buffers, uploads, tasks; B2: textures, targets, barriers, recording)

This is the design `parallel_gpu.md` ("What would scale", item 1) identifies as the one that
scales: each thread owns its data for long stretches instead of handing per-draw results across.
From the table above B2 would take textures (0.99), render state and begin rendering (0.45),
dynamic state, descriptors, the draw command and the kick (~0.45): ~1.9 of 5.3 us; stage B1
would keep ~3.4 us, so stage B's critical path would fall by ~35% (plus whatever the dispatch
path splits), i.e. ~+50% FPS where stage B limits, bounded by the main thread at ~130 FPS.

What it needs, and why I did not do it here:

- Two producers into the scheduler's command stream in stream order while B2 lags B1 by several
  draws: the scheduler would need a second ordered lane (B1's upload copies and barriers must be
  recorded before B2 reaches the draw, after B2's previous draws), or B1 must stop recording
  anything and hand its upload commands to B2 as data. Every `runtime.CopyBuffer`,
  `IsBufferAccessed`, `AccessBuffer` call in the buffer cache moves to B2 or becomes a message.
- The buffer and texture caches call each other: `ObtainBuffer` invalidates images for storage
  writes (`InvalidateMemoryFromGPU`), texel buffers sync from images (`SynchronizeMemoryFromImage`),
  image refresh obtains buffers (`ObtainBufferForImage`, `RefreshObtain`), the copy shader HLE and
  the DMA GPU paths use both and record copies. Each of these becomes a cross-stage drain or a
  protocol.
- `ResetBindings` (buffer access marks of the draw's buffers) and `needs_barrier` are shared
  between the halves.
- The memo validity arguments (texture sets, render state) assume the one-thread order; B2's
  memos would have to be reasoned about against B1's uploads landing in between.
- Ordered tasks: WriteData/DMA stores (B1) are ordered against host copies and deferred fences
  that the recorders run after B2's commands; the fence barrier logic of `perf-plan.md` C2-3
  assumes a single stage B position.

That is a multi-week refactor across `vk_scheduler`, `buffer_cache`, `texture_cache`,
`vk_rasterizer` and `vk_shader_hle`, with failure modes that only show in play (stale uploads,
missing barriers, heap corruption through fence order). I cannot run the game here, and a
change of that size cannot be validated by compiling it. It is the right next step if the
remaining single-thread wins are exhausted; it is not safe within this scope.

### C. Splitting the command stream at frame or submission boundaries

Two stage B instances on alternate frames or submissions would need two copies of every cache
and of the image layout state, or locks around all of it; the stream of one frame depends on
the layouts, uploads and GPU-modified marks the previous frame left. Not viable.

### D. Speculative cache lookups on the draw-preparation workers

The `bb:DrawPrep` workers already compute everything that depends only on the packet (sharps,
hashes, vertex inputs). What remains on stage B is the part that reads mutable cache state and
must be validated against it, and the validation is the same read as the lookup (the texture set
check is 0.22 us of the 0.59 us set path; the lock-free dirty-page check is a bitset scan). A
"candidate" from a worker saves nothing stage B does not redo, and a stale candidate costs a
second pass. Rejected for the texture and buffer lookups; the remaining pure parts (bone
palette hash, the texture set key) are ~0.1 us.

### E. A lookahead thread that prefetches for stage B (implemented)

Stage B is a pointer-chasing consumer whose per-frame working set does not fit its core's
caches: the texture set memo alone is 32768 slots of ~1.1 KiB (~35 MiB, more than the 5600's
32 MiB L3), plus thousands of `Image` slots, the page tracking words of the touched 4 MiB
regions, the arena table, the record chunks and the packets themselves. The first touches of a
draw form short dependent chains (set slot -> entry id -> image slot; region pointer -> page
words) that nothing can overlap because the addresses come from the previous load. A thread that
walks the committed packets a few ahead of stage B can resolve those chains early and prefetch
the lines, so stage B finds them in the shared L3 or in the lookahead core's cache (an on-die
transfer, ~40-100 cycles) instead of memory (~250-300 cycles at DDR4-3200).

It changes no state: the only loads are from the packet ring (committed packets stage A cannot
overwrite), the prepared draws (kept alive until stage B passes the packet), the shader infos and
the memo slot's key and ids; everything else is an address that is prefetched and never read, and
prefetch instructions do not fault. Risk is therefore low and the whole thing sits behind one
toggle, default off. The gain is bounded by the DRAM-miss share of stage B's time, which the
profiles cannot show directly (a flat profile is consistent with it but does not prove it); the
estimate is in section 5. It is honestly the only one of the five that I could make safe without
running the game, and it is the one with the best gain-to-risk ratio for a single toggle. It
does not reach the stated target of stage B well under the main thread's time; B does.

## 4. The lookahead thread (`BB_DRAWREC_WORKERS=1`)

Thread `bb:DrawAhead`, `Rasterizer::LookaheadLoop` (`vk_rasterizer.cpp`), started with the draw
pipe when `BB_DRAWREC_WORKERS` is set to anything but 0. Normal priority (`BB_DRAWREC_IDLE=1`:
idle priority, only spare cores, starved on a busy CPU). It keeps a cursor into the draw pipe
ring:

1. If stage B passed the cursor (it fell behind), it jumps to stage B's position (counted:
   `behind stage B`).
2. If it is more than `BB_DRAWREC_LOOKAHEAD` packets (default 8) ahead of stage B's count, it
   waits (pause spin, yields after 100 us; counted: `waited for it`): lines fetched too early
   would be evicted before use and only pollute the cache.
3. If no packet is committed past the cursor it spins 20 us and then sleeps; `DrawPipe::Commit`
   wakes it with the same announce-then-check protocol as stage B's own sleep
   (`DrawPipe::WaitForCommit`; one extra atomic load on stage A per commit).
4. Otherwise it reads the packet (`DrawPipe::PacketAt`) and runs `PrefetchPacket`.

`PrefetchPacket` handles direct draws with a prepared draw (`PacketKind::Draw` with
`packet.prepared`; 82-98% of draws in the logs): tasks, dispatches, indirect and special draws
bind without prepared sharps and are skipped. It parses the packet exactly as `RunDrawPacket`
does and, per stage with a matching `PreparedStage` (`FindPreparedStage`, now also usable from
another thread):

- Textures (the `BindTexturesFromSet` path): the same key as stage B
  (`XXH3(image_hashes) ^ stage ^ point_native`), the slot's header and hashes; if the slot's key
  matches (one relaxed load), its entries and the first 192 bytes of each image slot
  (`TextureCache::ImageSlotAddress` -> `SlotVector::SlotAddress`: an address computed from the
  storage pointer, never dereferenced); otherwise the two ways of the image description cache
  for each texture (the start of the full lookup).
- Buffers (`BindBuffers` -> `ObtainBuffer`): for each non-special buffer that is not a ring
  binding of this packet, `BufferCache::PrefetchBinding`: the arena table entry and, through
  `MemoryTracker::Prefetch`, the CPU and GPU page bit words of every 4 MiB region of the range
  (what `IsRegionModified` and `ForEachUploadRange` read first; regions are not created).
- The vertex streams (`ResolveVertexBuffers`): the same for each V# in the packet.

Not prefetched: the index buffer and the render targets (they come from the register file, which
the lookahead thread does not replay), the memory tracker's region locks (taken only for written
bindings, and a shared prefetch of a line stage B will write only costs an extra invalidation),
anything behind a pointer read from a memo slot (an image's backing state), guest memory (reads
there could trigger the page tracker's read-fault path).

Safety:

- Ring: `Begin` waits for room below `consumed + Capacity`, so a packet at or past `consumed` is
  never overwritten; the lookahead reads only packets between `Consumed()` and `Published()`
  (acquire loads; `Commit` publishes after the bytes are written).
- Prepared draws: kept alive by `pipe_keepalive` until stage B reached the packet's position and
  stage A ran `RetireSubmission`. If the lookahead falls behind and reads a packet stage B has
  passed, the prepared pointer may be stale; the read is wrapped in `BB_RECOVER_SET` (the
  loader's vectored handler resumes at the recovery point on any thread) and counted
  (`faults recovered`). The ring bytes themselves stay valid for 16 MiB of further packets.
- Memo slots and image ids: read with relaxed atomic loads of the key and the ids only; a torn
  or reused id produces a wrong address to prefetch, which is harmless.
- No locks, no page protection, no guest memory, no scheduler calls, no cache mutation: the
  deferred-protect invariant, the fence ordering and the DMA/WriteData side effects are untouched
  because the thread never participates in them. Stage B's order and results are the same with
  the thread on or off.
- Stop: the thread is a `std::jthread` member declared after `draw_pipe`, so it is joined before
  the ring goes; `WaitForCommit` registers a stop callback on its sleep.
- Live A/B: experiment bit 16 of `BB_TOGGLE_FILE` pauses it (it sleeps in 1 ms steps and
  re-syncs on resume), so `-AbBits 16` compares on/off within one run.

What can still go wrong (not ruled out without running it):

- It takes a hardware thread. On 12 threads with the game's own pools, stage A, stage B, two
  recorders, copy threads and `bb:Protect`, a spinning thread can slow a game thread or stage B
  itself; the spin is short (20 us) and it sleeps between frames, but heavy fights keep it busy.
  If the FPS drops with it on, `BB_DRAWREC_IDLE=1` is the first thing to try, then a smaller
  window.
- Bandwidth and pollution: ~20-60 lines per draw at ~5 us per draw is ~1 GB/s of extra traffic
  if every line missed; prefetches past the L2's outstanding-miss capacity are dropped, which
  only loses the benefit.
- Lines stage B writes right after reading (binding flags, `tick_accessed_last`, set
  generation) are in the lookahead core's cache in shared state: stage B's write costs an
  invalidation. The texture helper measurements show the cost of moving written lines between
  cores; here only reads are shared, which is cheaper, but it is not free.
- The stale-packet recovery relies on the vectored handler being installed and on nothing
  holding a lock across the faulting read (nothing does); a fault in the middle of a prefetch
  loop leaves no state behind.

## 5. Expected gain, from the numbers

Per draw, stage B's first touches that the lookahead covers: 1 texture set slot (3 lines header
and hashes, then 12 lines of entries for a full set) and 2-8 image slots (2-3 lines each) for
each of 2-3 stages with textures; 2-3 lines per buffer range for ~6-10 ranges (the arena entry is
hot). That is 20-60 lines per draw. How many of them miss to DRAM is what the logs cannot show.
If 4-8 of them do (the set slot in a 35 MiB table that exceeds the L3, images not bound since
the last frame, regions not touched for a while), each costs 80-100 ns on this CPU: 0.3-0.8 us
of the 5.3 us per packet, 6-15%. A lookahead that resolves them a few packets ahead removes
most of that latency from stage B's path but adds the shared-line invalidations and the thread's
own footprint, so I expect 0-8% less stage B time per frame, with a real chance of 0 and a small
chance of a loss on a core-starved run. The same window and `-AbBits 16` will tell. If it gives
nothing, the honest reading is that stage B is not DRAM-latency bound in the parts the lookahead
can reach, which is itself useful before investing in approach B.

Measure: `Draw pipe` lines (`BB_FRAME_STATS=1`): `stage B busy`, and the new line

```
  lookahead: N packets/s walked, D draws/s prefetched, L lines/draw, A packets ahead,
             behind stage B B/s, waited for it W/s, busy X%, F faults recovered
```

`A` should sit near the window (a lookahead that keeps up); `behind` near 0; `busy` well under
stage B's. With `BB_SECTIONS=1`, `stage B us per packet`: `tex set`, `tex set check`, `buffers`,
`VRAM track`, `vertex` are the sections that should fall. FPS and `GPU thread us/draw` are the
result. Correctness: screenshots identical on/off (nothing changes what stage B does), no new
`Draw pipe:` reports, `faults recovered` ~0 (a steady count means the thread keeps falling
behind; raise nothing, it only loses the benefit).

## 6. Files changed

- `gpu/shim/bbport_toggles.h`: `BbStats::lookahead_*` counters (CRLF).
- `gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h`: `Published`, `ConsumedPackets`,
  `PacketAt`, `WaitForCommit`; the lookahead's wake flag checked in `Commit` (CRLF).
- `gpu/shadps4/common/slot_vector.h`: `SlotAddress` (CRLF).
- `gpu/shadps4/video_core/texture_cache/texture_cache.h`: `ImageSlotAddress` (LF).
- `gpu/shadps4/video_core/buffer_cache/memory_tracker.h`: `Prefetch` (CRLF).
- `gpu/shadps4/video_core/buffer_cache/buffer_cache.h/.cpp`: `PrefetchBinding` (CRLF).
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h/.cpp`: `LookaheadWanted`,
  `LookaheadWindow`, `LookaheadLoop`, `PrefetchPacket`, `PrefetchDraw`, the static
  `FindPreparedStage`, the thread member, the startup line and the statistics line (CRLF).

Environment: `BB_DRAWREC_WORKERS=1` (on; default off, 0 = today's behaviour; the value is a
switch, there is one lookahead thread), `BB_DRAWREC_LOOKAHEAD=<packets>` (default 8, 1..256),
`BB_DRAWREC_IDLE=1` (idle priority), experiment bit 16 of `BB_TOGGLE_FILE` (pause, for A/B).

## 7. Approach B: build plan (2026-10-21)

Measured before starting (`lantern-20261021_193312.log`, Cathedral Ward, RTX 5060, PC memory
model on): stage B 3.83 us per packet, 84-91% busy at 1300-1600 draws per frame (80-98 FPS).
Texture binding 0.71 (set 0.44: check 0.15, apply 0.20; full 0.16), buffers and `ObtainBuffer`
0.61, vertex 0.32, dispatch 0.44, motion 0.22, begin render 0.20, the rest under 0.15 each.

The texture binding helper (section 3A) showed what does not work: per-draw results handed
between cores cost what they save. Each thread must own its structures for long stretches. Stages,
each shippable on its own behind `BB_STAGEB_SPLIT=<n>` (default 0, today's single thread), each
with a Check mode before the next starts:

1. **One producer.** Everything stage B records (uploads, barriers, draw commands, ordered tasks)
   goes through a single ordered command list owned by the thread that will become B2. B1 appends
   its commands as data (copies, buffer barriers) instead of calling `scheduler.Record`. No second
   thread yet; Check: the recorded command stream (a hash per submission) is identical on and off.
2. **B2 thread with the recording tail.** Descriptors, dynamic state, `MarkPass`, the draw command
   and the kick run on B2 from a per-draw record B1 fills. B1 never reads what B2 writes. Check:
   submission hashes identical; `stage B busy` split between the two.
3. **Image state on B2.** Layouts, `Transit`, binding flags, the render state and begin memos
   move to B2. B1 reports which images a draw samples and targets (ids, ranges) without touching
   their state. Check: image barrier list per draw identical to the single-thread path (a
   recorded-barrier log compared in Check mode).
4. **Texture cache on B2.** Lookups, the texture set memo and refreshes run on B2. A refresh that
   needs buffer data asks B1 through an ordered request (the buffer cache stays B1's); storage
   writes from B1 send invalidations to B2 in stream order. Check: per draw, the image views bound
   match what the single thread would bind (both computed in Check mode, the second discarded).
5. **Ordered tasks.** WriteData/DMA stores and deferred fences keep their single stream position:
   B1 issues them only once B2 has recorded every earlier draw (a position handshake, as the
   draw pipe's `Reached`).

Expected after stage 4: B1 keeps decode, buffers, uploads and dispatches (~2.3 us), B2 textures,
targets and recording (~1.5 us); the critical path falls to ~2.3 us, ~+60% FPS where stage B
limits, until the GPU or the game's main thread does. Stages 1 and 2 alone change little (the
recording tail is ~0.5 us) but build the hand-off everything else needs.

## 8. Runs committed by workers (2026-11-09)

Measured before starting (`last_session.log` of 2026-11-09, RTX 5060, Ryzen 5 5600, 0.9.40
candidate): stage B 98.7% busy at 3.83 us per packet (with `BB_SECTIONS=1`, whose timers cost
about 0.39 us of that, measured offline for 25 scopes). The GPU command thread is not the limit:
its sampled stacks spend 13-30% in `NtWaitForAlertByThreadId`, most of it waiting on stage B. No
function of stage B is above ~3% of its samples.

Approach A (section 3) failed per draw: the texture helper handed each draw's results across
cores. A worker that commits a whole stretch of draws pays the hand-over once per stretch. The
unit is a **run**: direct draws one after another in one render pass, with no ordered task,
dispatch, indirect or special packet between them.

### Step 1: the census (`LANTERN_RUN_CENSUS`, on with `BB_SECTIONS=1`)

Before any worker exists, stage B classifies every direct draw after it ran: clean, or the
first thing it did that changes what other draws share. In order: a render pass began (a real
barrier ends the pass, so barriers land here), a buffer upload, a texture set lookup that was not
a plain memo hit, a render state memo miss, a binding the GPU writes, other (occlusion query,
pipeline still compiling, the display pass, the RT capture, frame capture). Clean draws in a row
form a run; the line

```
  runs for workers: C% of D draws/s clean (X us each, the others Y us), L% in runs of 8 or more
  (R runs/s, N draws each, S% of stage B's time: two workers would take up to T% off it); draws by
  run length ...; runs ended by ...; clean draws streaming constants Z%
```

bounds the gain: `T` is two thirds of the time in runs of 8 or more, before fork and join costs.
If `T` is small, the cuts list says which shared work to make worker-safe first.

### Step 2: what a worker owns

- **Registers.** `AmdGpu::Regs` is 0xD000 words (208 KiB): too much to copy at a fork. Each
  worker keeps its own copy in step by applying every packet's changed blocks, as stage B does
  (32-word blocks, a few per packet), whether or not the packet is its own.
- **Per-draw scratch.** Everything `DrawRecord` fills for one draw moves into a draw context, one
  per thread: push data, descriptor writes, image and buffer infos, vertex and index bindings, the
  ring stages, the packet's V#s and keys, bound images and buffers, `needs_barrier`, the census
  flags. Stage B keeps one too, so the single-thread path runs through the same code.
- **Commands.** A worker records into chunks of its own. The main thread reserves the place of
  each part in the command stream at the fork and splices the parts in order at the join; nothing
  is copied.
- **Counters and notes** (statistics, image binding flags to clear, buffer accesses for the
  barrier tracker, object motion jobs) are kept per worker and applied by the main thread at the
  join, in draw order.

### Step 3: where a worker stops

Before anything the census counts as a cut: a new pass, an upload, a texture lookup that would
create, refresh or revalidate, a render state miss, a GPU-written binding, a query, a pipeline
still compiling. The worker stops there, the main thread commits that draw and the rest of the
part in order, and parts after a stop are thrown away and committed in order too.

### Step 4: the fork

At a clean draw with the render pass open, the main thread looks at the committed packets after
it (`DrawPipe::PacketAt`), up to the first packet that is not a direct draw or changes a target
register block. A run of 8 or more is split into contiguous parts: one for the main thread, one
per worker. Workers spin briefly after a part and then sleep on an address, like the lookahead
thread. `LANTERN_RUN_COMMIT=fork` forks with no work, to price the fork and join alone; `=on`
commits; a compare mode commits each run both ways and counts differences per field, the
second copy thrown away. Off by default, and off on CPUs with fewer than 12 threads until a run
there shows a gain.
