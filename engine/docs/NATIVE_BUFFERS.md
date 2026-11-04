# Native buffers: research and design

How Lantern's buffers and constants become native: driven by GX's objects and calls, the way a
D3D11 driver works, instead of watching guest memory the way an emulator does. Written
2026-11-04 for D's last step and for E. Our own design; nothing here is taken from another project's
code.

## Where the time goes now

From the user's run on 2026-11-04 (`BB_SECTIONS`, about 215,000 draws a second, 94-96 FPS):

- The GPU command thread (stage A) is idle 1.7% (6.8 us a draw); the recording thread (stage B) is
  97% busy (4.4 us a packet). Both are full, so only work removed from both raises FPS.
- Stage A: constants 1.4 us a draw (about 4.6 constant buffers and 2 flattened user data blocks, each
  checked against GPU writes, given ring space and copied), inputs 0.6, packet 0.4.
- Stage B: binds 1.2 us (textures 0.69, buffers 0.40), vertex and index buffers 0.5, `ObtainBuffer`
  0.6 in total (arena, residency, in-place and shadow checks per binding).
- An earlier profile (`parallel-stageb.md`) found no hot function: the cost is cache misses spread
  over the buffer, texture and barrier structures. Removing structures removes misses; tuning them
  does not.

## What the research says

- **D3D11 on Vulkan (DXVK).** A dynamic buffer's discard hands out a new slice of memory owned by
  that buffer; the binding points at the buffer, and the current slice is resolved when the draw is
  recorded. One buffer per object updated once a frame, or one buffer per size class updated every
  draw, both stay cheap. A whole-buffer discard and an `UpdateSubresource` cost about the same.
  Nothing is found by address and no memory is watched: every change arrives as an API call.
- **Change detection beats upload tricks.** The largest measured gain in comparable renderers came
  from not re-sending constants that did not change, more than from how they are sent (push constants,
  dynamic offsets and uniform buffers measured close to each other).
- **Descriptors.** We already use push descriptors, which D3D11-style layers keep as their fast path.
  Descriptor buffers can lower CPU cost but have cases where they are slower and need barriers when
  rebound; not a first step.
- **Upload memory.** Host-visible VRAM (ReBAR) and upload heaps are write-combined: write whole,
  aligned lines sequentially and never read them back on the CPU (reads are tens of times slower).
  Our constant ring is written once and never read on the CPU, which is right.
- **Memory watching** (page protection and exception handlers on Windows, userfaultfd write-protect
  on Linux, which we already use) is the emulator's tool. Our log shows 0-3 write faults a second, so
  it is no longer a cost; the cost is the per-binding lookups that exist because we find buffers by
  address.
- **Large pages** cut TLB misses for big guest memory, but Windows needs a user privilege
  (SeLockMemoryPrivilege) players do not have; Linux transparent huge pages are possible later.
  Not part of this plan.

## What GX does (checked in our eboot)

- A dynamic buffer's update (`UpdateSubresource` `0x2168a20`, buffers with flag 4 at +0x3b) goes to
  `0x26adb90`: new memory from the context's ring allocator `0x26addc0` (64 KiB blocks from the pool
  `0x26a9d50`), the data copied in, and the context's rename table (`+0xb3c0`) now maps the buffer to
  it. `0x26ae840` reads that table. So a dynamic buffer's contents change only through a new
  rename, and the memory it had is reused only once a block comes back from the pool.
- Other buffers are updated through `0x26b3560`, which writes through the command stream; stage A
  sees those writes in order.
- GX's state flushes (`0x26d2230`, `0x26d3370`) write only registers that changed.

## The plan

1. **D, last step: constants by GX object.** Each draw's record already names each constant buffer
   object and its current memory (the rename lookup). A buffer's version changes when GX renames it
   or updates it through `0x26b3560`. Stage A copies a constant buffer into the ring only when its
   object or version is new since its last copy, and the ring keeps that copy alive while a later
   draw still uses it. Unchanged per-frame and per-material constants are then bound with no
   checks, copy or ring space. `=2` compares every reused copy with the bytes in memory and logs
   differences, as the records do.
2. **E: buffers by object.** GX's vertex and index buffers (static, made with their data) and its
   other buffers each get their place on the GPU when GX creates or updates them. The record carries
   the object, and stage B binds that object's buffer directly: no arena, residency, in-place or
   shadow checks per binding. The address path stays for what GX does not own, counted by reason
   until it can go.
3. **E: textures by object** follow the same way (a texture already holds its image); the texture
   set memo's per-image checks become the object's own state.

Each step ships only when it is measurably faster than the old path, with a compare mode first.
