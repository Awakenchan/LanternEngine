# Native rendering from GX

Lantern Engine's renderer today reads the game's PM4 command buffers and works out each draw from
the GPU registers and the descriptors in guest memory (the shadPS4-derived path). This plan moves
it up a level: the game draws through GX, FromSoftware's graphics layer, which is shaped like
Direct3D 11. Each draw, dispatch, clear and copy is taken from GX's own objects at the call, and the
command buffer only keeps the place of each in the stream.

Every address below is an image offset (the eboot loaded at 0, as in all of Lantern's code). Every
fact is checked against the 1.09 eboot by `tools/re/gx_facts.py`, which must print `0 failed`
before any of them is used.

## What GX is in the eboot

| What | Where | How we know |
|---|---|---|
| Draw | `0x21696c0` | writes Gnm's drawIndexAuto, flushes the output merger and the command buffer |
| DrawIndexed | `0x2169780` | writes drawIndexOffset |
| DrawInstanced | `0x2169840` | drawIndexAuto with an instance count |
| DrawIndexedInstanced | `0x2169900` | drawIndexOffset with an instance count |
| DrawInstancedIndirect | `0x21699d0` | drawIndirect, then the post-draw |
| DrawIndexedInstancedIndirect | `0x2169ab0` | drawIndexIndirect, then the post-draw |
| Dispatch, DispatchIndirect | `0x21695c0`, `0x2169640` | dispatch and dispatchIndirect |
| Commit (state only) | `0x2169ba0` | the output-merger flush and the post-draw with no draw packet |
| Output-merger flush | `0x26d2230` | called by every direct draw and the commit |
| Post-draw | `0x26cbd50` | called after the indirect draws and the commit |
| Command buffer flush | `0x26af700` | the end of every GX operation |
| FinishCommandList | `0x26d5850` → `0x216aca0` | a deferred context's work as a command list |
| ExecuteCommandList, ExecuteCommandLists | `0x21679a0` → `0x216a7d0`, `0x21679e0` → `0x216a980` | the immediate context runs the lists in order |
| Segment submit | `0x26b0b00` | a command buffer's segment to the GPU |
| Close, submit and flip | `0x26afb30` → `0x26b0ce0` → `0x26b0e30` | the frame's last segment with the flip |
| Present | `0x26d5dd0` → `0x26d6e50` | sync interval clamped to 4, the swap chain at context +0x33d0 |
| CreateBuffer | `0x2165c10` | a 0x58-byte holder per buffer |
| Upload heap pool | `0x26a9b50`, made by the device at `0x216b0f0` | 0x30-byte nodes in 0xff0-byte blocks: every renamed constant buffer, Map rename and ring allocation |
| Occlusion query writer | `0x1077660` | |
| Pipeline drain | `0x26b09e0` | a label, an end-of-pipe event and a wait |
| Resource-table reclaim | `0x26aa860` | |
| Flush wait | `0x11d7030` | |
| Constant engine writers | `0x11fd190`–`0x11fd3e0` | |
| Gnm packet writers | `0x1072ed0`–`0x108b610` | `gnm_names.txt` |
| YEBIS's D3D11-shaped context | vtable `0x530a380` | 133 methods: Direct3D 11's 115 in order, then YEBIS's own; slot 50, ClearRenderTargetView, is `0x2a41320` |
| YEBIS's Gnm draw index, draw auto, dispatch | `0x11fbc40`, `0x11fbb50`, `0x11fbd10` | |
| Post-processing entry points | `0xbb68f0` (bind framebuffer), `0xbb6980` (apply effects) | |
| The flip pass (Gnmx) | `0x21d5f80` | its constant update engine at `0x1092e80` and `0x1093150` |
| The frame timer | `0x2034770` | the one the frame-rate patches already change |

GX's context and device methods are not found through a vtable in the image: they are called
directly.

## GX's command lists and pending state (2026-10-29)

Found with the Ghidra project (`tools/re/ghidra.sh`, see `NATIVE_ENGINE_PLAN.md`) and checked by
`gx_facts.py` (162 checks, `0 failed`).

A deferred context records its calls as GX's own records, and the immediate context replays them
through a table of 36 handlers at `0x52b9fb8`. Each handler reads one record, calls the same context
method the game calls directly, keeps the value in the pending state and returns the record's size.
`0x26ab6b0` is handler 28 (Dispatch), not a dispatcher. So hooking the context methods covers
deferred work too: replay goes through them.

The pending state is the second argument of every context method (the first is the context):

| Handler | Method | Pending state |
|---|---|---|
| 0 | IASetInputLayout `0x2168c90` | `+0x0` |
| 1 | IASetPrimitiveTopology `0x2168ca0` | `+0x8` |
| 2 | IASetIndexBuffer `0x2168cb0` | `+0x10` buffer, `+0x18` format, `+0x1c` offset |
| 3 | IASetVertexBuffers `0x2168cc0` | `+0x20` buffers, `+0x120` strides, `+0x1a0` offsets (32 slots) |
| 4–27 | six shader stages, four setters each | per stage: `+0` 16 samplers, `+0x80` 128 shader resources, `+0x480` 14 constant buffers, `+0x4f0` the shader |
| 28–35 | Dispatch, Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, both indirect draws, Commit | |

The stages' blocks start at `+0x598`, `+0xa90`, `+0xf88`, `+0x1480`, `+0x1978` and `+0x1e70`, in
replay order. The trace proves them against the registers: `+0x598` is the vertex stage (the LS
program when hull is set, the VS program otherwise), `+0xa90` hull, `+0xf88` domain (the VS program),
`+0x1978` pixel, with the program's address at `+8` of the description at shader `+0x100`. `+0x1480`
(geometry) and `+0x1e70` (compute) have not drawn yet in a checked run.

A GX draw may first write draws of its own: the output-merger flush resolves a render target that is
being unbound through `0x26b5a70`, a rectangle with GX's state, not the game's. 87,692 of them in one
run; a native path has to keep them.

Trace result (2026-10-30, Hunter's Dream, Cathedral Ward, Central Yharnam, 5.36 million draws): 0
differences in count, index address, index type, render targets, depth, viewport and shader programs.

| Stage block | SetSamplers | SetShaderResources | SetConstantBuffers | SetShader |
|---|---|---|---|---|
| `+0x598` | `0x2168ce0` | `0x2168d00` | `0x2168d90` | `0x2168e00` |
| `+0xa90` | `0x2168e10` | `0x2168e30` | `0x2168ec0` | `0x2168f30` |
| `+0xf88` | `0x2168f40` | `0x2168f60` | `0x2168ff0` | `0x2169060` |
| `+0x1480` | `0x2169070` | `0x2169090` | `0x2169120` | `0x2169190` |
| `+0x1978` | `0x21692c0` | `0x21692e0` | `0x2169370` | `0x21693e0` |
| `+0x1e70` (compute) | `0x2169440` | `0x2169460` | `0x21694f0` | `0x21695b0` |

On the immediate context the pending state is the block at `context+0xb730`; on a deferred context
it is at `+0x28` of its recorder. The game writes it inline beside each call, and most setters only
return: that is why no setter body writes these slots.

| Pending state | What | Setter |
|---|---|---|
| `+0x220` | stream-output targets (4) | `0x21691a0` |
| `+0x240` | rasterizer state | RSSetState `0x2169290` (only returns) |
| `+0x248`, count `+0x4c8` | 16 viewports, 24 bytes each | RSSetViewports `0x21692a0` (the count goes to `0x26d3350`) |
| `+0x3c8`, count `+0x4cc` | 16 scissor rectangles, 16 bytes each | RSSetScissorRects `0x21692b0` (the count goes to `0x26d3360`) |
| `+0x4d0`, `+0x568`, `+0x580` | blend state, blend factor, sample mask | OMSetBlendState `0x21693f0` (only returns) |
| `+0x4d8`, `+0x584` | depth-stencil state, stencil reference | OMSetDepthStencilState `0x2169400` (only returns) |
| `+0x4e0`, `+0x520`, `+0x585` | 8 render targets, the depth view, a byte the same call sets | OMSetRenderTargets `0x2169420` |
| `+0x528`, `+0x578`, `+0x57c` | 8 unordered-access views, their start and count | OMSetRenderTargetsAndUAVs `0x2169430` |

A state object holds Gnm's register values, made once by its creator's converter, so the flushes copy
them without translating: the rasterizer's flush is `0x26d3370`, the output merger's `0x26d2230`.

| Creator | Object | Converter |
|---|---|---|
| CreateBlendState `0x21658e0` | 0x128 bytes | `0x2178050` |
| CreateDepthStencilState `0x21659a0` | 0x38 bytes | `0x21782b0` |
| CreateRasterizerState `0x2165a80` | 0x24 bytes | `0x2178550` |
| CreateSamplerState `0x2165b40` | 0x40 bytes | `0x2178750` |

Most state objects do not come from these creators: `0x21aeca0` builds the game's preset library by
calling the converters itself, and the game keeps those pointers. A state object is plain immutable
bytes, so Lantern learns it from its first bind instead of hooking its making.

The shader creators, proven by the stage block each one's shaders are bound to: vertex `0x2166d00`,
hull `0x2166e10`, domain `0x2166f20`, geometry `0x2167030` (and `0x2167140` with stream output, not
hooked: eleven arguments), pixel `0x21672d0`; `0x21673e0` is left for compute.

Objects with a table entry (`LANTERN_GX_OBJECTS`): buffers, textures and views from their creators,
each released through its vtable's first two slots; shaders from their creators (they have no
release: a reused address gets a new generation); state objects from their first bind (the bytes are
compared at each bind, and changed bytes get a new generation).

Every draw runs the same flushes before its packet: the output merger (`context+0xb558`), the
shader stages (`0x26bba30` on `context+0xb518`, with an "indexed" flag), `0x26d3370` on
`context+0xb528`, the command flush, and the resource bind `0x26bc2e0` on `context+0x50`; then the
Gnm writers on the command buffer at `context+0x10`, then the post-draw.

## GX's device creators

All take a Direct3D 11-shaped description. CreateBuffer's is D3D11's (byte width, usage 0–3,
bind flags, CPU access, misc flags, structure stride); usage 1 (immutable) needs the initial data.

| Creator | Address | Object |
|---|---|---|
| CreateBuffer | `0x2165c10` | 0x58 bytes, vtable `0x535f3a0` |
| CreateTexture2D | `0x2165f90` | description: width, height, mips, array size, format, samples, usage, bind, CPU, misc |
| CreateTexture3D | `0x2166300` | width, height, depth, mips, format, usage, bind, CPU, misc |
| CreateDepthStencilView | `0x2166610` | 0x60 bytes, vtable `0x535f420`; needs bind flag 0x40 |
| CreateRenderTargetView | `0x21667a0` | 0x50 bytes, vtable `0x535f440`; bind flag 0x20 |
| CreateShaderResourceView | `0x21669a0` | 0x50 bytes, vtable `0x535f460`; bind flag 0x8 |
| CreateUnorderedAccessView | `0x2166b60` | 0x50 bytes, vtable `0x535f480`; bind flag 0x80 |
| CreateGeometryShader | `0x2167030` | 0x150 bytes; its check `0x2173220` wants the `Shdr` magic at `+0x24` and type 3 at `+0x2c` |
| CreateGeometryShaderWithStreamOutput | `0x2167140` | 0x150 bytes, through the same check |
| Other shaders (five) | `0x2166d00` (0x128), `0x2166e10`, `0x2166f20`, `0x21672d0`, `0x21673e0` (0x120) | the binary is PSSL (`Shdr` at `+0x24`) |
| CreateDeferredContext | `0x21676c0` | a 0xdae0-byte context |

A view's resource keeps its dimension in a byte at `+0x38`, checked against the view's dimension.

## How GX binds a shader's resources

`0x26bc320` (7,500 lines, one body per stage combination) walks the shader object: `+0`, `+8`, `+0x10`
are slot masks, `+0x18`, `+0x58`, `+0x98` map a slot to an entry of the table at `+0xd8`, `+0xe0` is
the input table and `+0xe8` its length. An input is a word: kind (low byte), user-data register
(next byte), slot (next byte). Kinds 0-1 are resource views, 2-3 access views, 4 a sampler, 5 a
constant buffer; 8-11 point at whole tables, copied into a ring (`0x26addc0` on `context+0xb3e0`) for
each draw. A constant buffer's descriptor is built on the spot from its current address,
`0x26ae840(context+0xb3c0, buffer+0x18)`, and its size at `buffer+0x2c`.

Checked at run time on 3 million resources: images 1,101,830 of 1,101,830 are a view's descriptor at
`view+0x10`, byte for byte; samplers 544,314 of 544,425 are the state object's first 16 bytes (the
rest are the shader's own anisotropy change); buffers 1,425,016 of 1,437,446 are a constant buffer's
current address, and the rest are one 48-byte block of GX's in another ring.

A shader-resource bind of Lantern's own (`gx_draws.cpp`, `LANTERN_GX_DRAWS=1`) replaces `0x26bc320`
for draws with no hull or geometry shader, 90.4% of binds; the rest still run the game's function, and
the state both keep is the same, so they can alternate at any draw. Its register writes go into one
packet per bind, a NOP tagged `0xbb5e0000` holding offset-and-length runs of user data, in place of a
hinted SET_SH_REG for each input. The shader and fetch registers are still the game's Gnm writers.
Checked by the trace at the registers: about 1.67 million inputs, 0 differ. PM4 packets per draw go
from 3.2 to 2.8 and from 3.7 to 3.1 (the only numbers so far that do not depend on what else the
machine is doing); no clean FPS comparison yet.

Where the GPU command thread's time goes, sampled at 117 FPS, 1,440 draws a frame, per draw: stage A
5 us (PM4 walk 1.7, pipeline 0.9, constants 1.25, inputs 0.3, packet 0.4), stage B 2.85 us a packet.
Flattening the user data costs almost nothing, so rewriting it from GX's slots gains nothing. What
costs is the PM4 walk, the packet with its register blocks, copying constants and the read tracking.
In real play (73 FPS, 1,544 draws a frame) stage A is 96% busy and stage B 99%.

## GX textures against the address-keyed image cache

`LANTERN_GX_OBJECTS=1` matches every image lookup to the GX texture at its address. Over the Hunter's
Dream, Cathedral Ward and Central Yharnam (2026-10-30): 168,728 of 168,728 lookups at a texture's
address have its size, and none fall inside a texture. Of 2,041 textures looked up, 1,923 have one
image. The rest have the same size, format and byte count under several image ids, so the cache made
the same image again; the one exception is a 1x1 cube whose image grows with its estimated size. 21 of
2,155 images served more than one texture: a texture's memory reused. So a texture object can own its
image, and all of those cases go away with it.

78,641 lookups are in no GX resource, at least 256 distinct images:

| What | Examples |
|---|---|
| Display buffers | three at 1920x1080, 8,355,840 bytes, formats 37 and 43 |
| Post-processing chain | 1920x1080 and 960x540 format 97, 784x662 down to 7x6 format 122 |
| Interface and other | 256x256, 256x1, 64x64 at addresses not aligned to a page |

YEBIS's device is a Direct3D 11-shaped table at `0x5308810` (the object's vtable after construction in
`0x2a3f5e0`): CreateBuffer `0x2a39310` → `0x11ecb70`, CreateTexture2D `0x2a39340` → `0x11df570`,
CreateTexture3D `0x2a39360` → `0x11e1750`; a texture's memory is at `+0x98`. With those two texture
creators in the table, lookups in no resource fall to 7,475 of 270,100 (2.8%): 7,416 are the three
display buffers, made by the swap chain creator `0x26d5e90`, and the other 59 are small images, 256x1 down to 1x1
and 1024x1024, at addresses not aligned to a page. Textures with several images are then 422 of 3,452:
the same size again, or a separate image for each mip range of one texture (256x256, 128x128, 64x64 at
a growing byte count), which one image holding every mip ends.

### Textures by object: the first slice

`LANTERN_GX_TEXTURES=1`: `TextureCache::FindImage` widens a request at a known texture's address to the
texture's full mips and layers from its GX description, so the first lookup makes the whole image and
later ones are views of it. Depth targets and multisampled images are left out so far.

Two boots through the Hunter's Dream, Cathedral Ward and Central Yharnam, old against widened: images
created 3,927 to 3,578, expanded 49 to 5, candidates rejected 1,259 to 563; 42,690 requests widened, none
too large for the memory and none of another layout; no render errors, 0 trace differences. Not checked:
the picture itself. Still-identical images under one texture look like the collector, which frees an image
unused for 20 seconds, and these boots warp between areas; that is not proven. The overlap resolver still
runs, so it is not removed yet: what remains is the display buffers, depth targets and the small images.

This removes re-created images and their copies. It does not raise FPS in a steady scene, where the memo
already hits; that comes from stage A and B working from the objects, after this.

### The display buffers, depth targets and a texture's image

`0x26d5e90` makes the swap chain: it takes a description (format 0x57 only), allocates one block for all
the buffers, keeps a Gnm render target every 0x2c bytes at its start and registers them with VideoOut; a
buffer's address is the target's first word shifted left by 8, the description is at `+0x300` and the buffer
count at `+0x328`. It also makes through CreateTexture2D the texture the game draws into, and present
`0x26d6e50` copies that into the buffer through the holder at `+0x348`. `0x26b35e0` is not a creator: it is
the upload behind UpdateSubresource `0x2168a20`, and `0x26d6b20` is the gamma ramp, which uploads three
channels of 0x401 values into the texture at `+0x380`. A texture's byte count is at `+0x30` of its object.

`LANTERN_GX_TEXTURES=1` now also takes the display buffers from `0x26d5e90`, widens depth targets like the
others, and has a texture hold its image: a search past the memo first asks the object at the address, and
the image it holds answers if the checks every candidate has to pass let it. Everything else goes through
the old search and resolver, and what they return becomes the held image. `LANTERN_GX_TEXTURES=2` does the
same and also runs the old search's exact match beside it, to count any lookup where the two differ.

### Whole draws: the pipeline from a record

`LANTERN_GX_RECORDS=1` (needs `BB_NATIVE_CALLS`): at each GX draw with no hull, domain or geometry shader a
hash of what picks its pipeline is taken from the pending state: shaders, input layout, topology, state
objects, targets, and the views, samplers and constant buffers the shaders' input tables name, with an epoch
that moves when an object's address is reused. It reaches stage A beside the draw's record NOP. A record seen
before gives its pipeline at once, without the key and the shader specialization; a new one goes the old way
and is remembered. `=2` always goes the old way and counts a record that would have chosen another pipeline.
Run on 2026-10-31: 411,449 pipelines from the record, 0 differ.

### Whole draws: bindings and render state from the record

`LANTERN_GX_RECORDS=3` (`=2` compares instead): at the record's stamp the draw's descriptors are copied from
GX's slots for the vertex and pixel shaders' used slots (views at `+0x10`, samplers or GX's default at
`0x546c8b0`, constant buffers as GX's bind builds them from the current address, the global buffer at shader
`+0xf0`), with the shaders' input tables, the spill words Lantern's bind left for each upload input and GX's
ring copy of its shared block (`+0xb360` of the bind state), and the render state from the pending state and
its state objects (defaults `0x546ca58`, `0x546c8f4`, `0x546ca1c`). Per shader and GX shader object, stage A
keeps a fill plan: for every word of the flattened user data, its source in the record (a user data register
through the input naming it; a table word through the table plan's entry and the table input, views 8 words,
samplers and buffers 4; an upload word from the spill; a nested table through the spilled pointer's input).
A pipeline from the record then skips the table walk in guest memory: the user data registers are copied and
the rest is filled from the record, computed offsets with the plan's own expressions. A shader whose plan has a
word with no source, or a draw with an unbound slot, walks the tables as before and is counted by reason.
First `=3` run (2026-11-03, before the fill): 9,128,304 draws, every one's bindings and render state from the
record, nothing drawn wrong. The render state becomes the registers GX's flushes write (`gx_facts.py`):
`CB_TARGET_MASK` and `CB_BLEND0-7_CONTROL` from the blend object (`& 0xffff1fff`), the blend color from `+0x568`,
`DB_DEPTH_CONTROL`, `DB_STENCIL_CONTROL` and both `DB_STENCILREFMASK`s from the depth-stencil object and the
reference at `+0x584`, `PA_CL_CLIP_CNTL` and `PA_SU_SC_MODE_CNTL` from the rasterizer object, viewport 0 as
scale `w/2, -h/2, max-min` and offset `x+w/2, y+h/2, 0` with depth range 0-1, and viewport scissor 0 when the
rasterizer object enables it. `=3` puts them in the draw's register copy for the draw only; `=2` compares them
and the color and depth target addresses with the registers. Render target descriptions are the next part.

## Still to find, with our own tools

- The texture holder: where the resource's memory, format and tiling mode sit.
- Scaleform's renderer and its draws.

## Stages

Each stage is behind a `LANTERN_*` switch, off by default, with the current path kept. Each is
checked under the Vulkan validation layer for every family (`tools/validate_pipelines.sh`), goes out
in a beta, and becomes the default only after AMD and older NVIDIA cards have run it.

1. **Trace** (`LANTERN_GX_TRACE`). Hooks on the nine context methods count each kind of call a
   frame, and the pending state is read at each one and checked against what the PM4 path finds
   for the same draw. This proves the state offsets before anything depends on them.
2. **Objects** (`LANTERN_GX_OBJECTS`). GX's creators are hooked: every buffer, texture, view,
   shader and state object gets an entry in our tables when GX makes it, with a generation, and
   leaves it when it is released.
3. **Draws from GX** (`LANTERN_GX_DRAWS`). At a GX draw its inputs are recorded from the objects
   and a token takes its place in the command buffer. The GPU command thread builds the draw from
   the record, without reading registers or descriptors. A draw the record does not cover takes the
   PM4 path, and the log counts those by reason.
4. **Buffers by handle**. GX's immutable vertex and index buffers live in video memory from their
   creation, and draws bind them by handle. The upload heap is bound in place.
5. **Textures by object**. A texture's image belongs to its GX resource and generation: made at
   creation, filled by GX's upload and copy calls and the shaders that write it, dropped at
   release. No more guessing by address and hash. YEBIS's textures come from its own creation calls.
6. **Compute, clears, copies, queries and drains as tokens** from GX's calls, YEBIS's from YEBIS's
   context.
7. **Completions on the GPU's timeline**. The labels GX waits on are written when the submission
   has run. The PM4 walk then only orders the tokens.
8. **The shader lifter**. GCN shaders become structured, typed SPIR-V beside the native compiler,
   compared with it shader by shader.
9. **The translator goes**. Once stages 3–7 are the default everywhere, the PM4 path, the SRT walker,
   the constant-engine emulation, the GPU-write tracking and the address-keyed texture cache are
   deleted.

## Rules

- Facts only from outside sources: functions, addresses, what a call does. Each one is confirmed in
  the eboot by our own tools before it is used, and the code is written fresh.
- No code, file layout or text is taken from other projects.
