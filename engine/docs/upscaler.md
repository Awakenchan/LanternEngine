# Temporal upscaling and frame generation — frame analysis and plan

## Intel XeSS Super Resolution (upscaler=xess, 2026-11-01)

`upscaler=xess` runs XeSS-SR from Intel's XeSS SDK 3.0.2 (`vk_xess.cpp`, Windows only) on the same
frame as DLSS. XeSS Frame Generation is D3D12 only and not used. See docs/xess.md.

## FSR through AMD's FSR SDK 2.4 (upscaler=fsr, 2026-10-29)

`upscaler=fsr` / `BB_UPSCALER=fsr` runs AMD's own signed Vulkan upscaler from the FSR SDK 2.4.0
(`vk_fsr_sdk.cpp`). It is the single FSR entry of the Windows launcher and the default of the
shipped `lantern.ini`; saved `fsr3` and `fsr4` become `fsr` when the Graphics page opens, and so
does `fsr411` on AMD GPUs.

**What runs where.** The SDK chooses the version and the model by itself; the engine creates the
context without a version override, so the newest version the GPU supports is taken:

| GPU | What the SDK runs |
|---|---|
| Radeon RX 9000 (RDNA 4) | FSR 4.1.2, FP8 model |
| Radeon RX 6000 / 7000 (RDNA 2 / 3), Ryzen AI Max 300/400 | FSR 4.1.2, INT8 model |
| Ryzen AI 300/400, Z2 A, Z2 Extreme | FSR 4.1.2, lightweight INT8 model |
| Radeon RX 5000 (RDNA 1) and older, NVIDIA, Intel | FSR 3.1.6 |

The log names what was chosen: `Upscaler: AMD FSR SDK upscaler versions for this GPU: ...` (every
version the SDK offers on this device) and `Upscaler: FSR <version> (AMD FSR SDK 2.4), output WxH`
(the one in use). On AMD drivers that ship their own FSR provider (Adrenalin 26.10.1 and newer) the
SDK loader itself takes the driver's newer version when there is one, so the engine no longer looks
for the driver's `amdvkffx64.dll` (`vk_amd_ffx_driver.cpp`, the `LANTERN_FSR_AMD_DRIVER` switch and
the launcher's "FSR 4 from the AMD driver" card are gone).

**Files.** `engine/fsr_sdk/amd_fidelityfx_loader_vk.dll` (1.0.2.4555) and
`amd_fidelityfx_upscaler_vk.dll` (4.1.2.4555), unchanged from the SDK's `Kits/FidelityFX/signedbin`
at tag v2.4.0, signed by AMD; `SOURCE.txt` records the commit and hashes. `BB_FSR_SDK_DIR`
overrides the folder (`bb-start` sets it to `enginesr_sdk`); `BB_FSR_SDK=0` hides the files. The
SDK's frame generation DLL `amd_fidelityfx_framegeneration_vk.dll` (4.0.2.4555, same tag, signed
by AMD) ships beside them for `frame_gen=fsr` (`vk_fsr_framegen.cpp`, `notes/fsr-sdk-framegen-design.md`);
the other frame generation choices stay the built-in FSR 3.1 one and DLSS FG.

**License.** The DLLs come under AMD's binary redistribution license
(`fsr_sdk/LICENSE-AMD-FSR-SDK.md`): binary redistribution is allowed, its copyright notice,
permission notice and disclaimers must ship with them, and reverse engineering, decompilation and
disassembly are not permitted. The engine loads them at run time as separate programs, as it does
NVIDIA's DLSS and Streamline, and contains none of their code. The ABI structures in
`vk_fsr_sdk.cpp` and `vk_fsr_framegen.cpp` follow the SDK's public MIT headers (`ffx_api.h`,
`ffx_api_vk.h`, `ffx_upscale.h`, `ffx_framegeneration.h`, `ffx_api_framegeneration_vk.h`).

**Integration.** The same inputs as the other frame upscalers (`Fsr4Upscaler::Frame`): HDR scene
colour RGBA16F, depth D32S8 read through its depth aspect (standard depth, near = 0), motion vectors
in render pixels, the jitter offset, auto exposure from the menu. The context is created with the
SDK's version descriptor (`FFX_UPSCALER_VERSION` 4.1.2) and the current Vulkan backend descriptor
(`FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK`), on a worker thread: FSR 3.1 is shown until it is
ready (`BB_FSR_SDK_ASYNC=0` creates it on the render thread). The SDK picks the model by the
render/output ratio, so the presets' render sizes stay the engine's own and the "Sharper motion
(FSR 4)" switch does not apply. At most three frames are in flight for its descriptors.
Device features it may use are requested only when the files are present:
`shaderStorageImageReadWithoutFormat`, runtime descriptor arrays and variable descriptor counts
(each only where the GPU has it); FP8 cooperative matrices, INT8 dot products, FP16/INT8 storage
and subgroup size control are enabled already for the other upscalers.

**Fallbacks.** No SDK files, a non-Windows build or a GPU without
`shaderStorageImageWriteWithoutFormat`/`synchronization2`: `fsr` becomes the built-in FSR 3.1
before the first frame, and the menu says why. A failed context or dispatch: FSR 3.1 for the rest of
the session (choosing FSR again in the menu retries). FSR 4.1.1 through fsr4vk that fails falls
back to `fsr` when the SDK is there.

**fsr4vk stays experimental for NVIDIA and Intel.** The SDK gives them only FSR 3.1.6, so the
launcher keeps "FSR 4.1.1 (fsr4vk, experimental)" there; on AMD the entry is hidden (the SDK's own
FSR 4.1.2 is better and needs no driver fix). FSR 1 (`host_passes/fsr_pass.cpp`) is unchanged.

**Linux.** The SDK is Windows-only. The Linux launcher keeps FSR 3.1, the built-in FSR 4 v07 model
(`fsr4_shaders`) and the FSR 4.1.1 replay (`fsr4_411`), and maps `fsr` to `fsr4`. The Windows
release no longer ships `enginesr4_shaders` (33 MB); the folder stays in `package-template` for
the Linux package.

**Not verified yet.** Built only; not run on hardware in this change. To check per GPU family
before a release: RX 9000 (FP8), RX 6000/7000 (INT8, the first tester has an RX 6650 XT), an RDNA 1
card (expects 3.1.6), a Ryzen APU (lightweight INT8), NVIDIA and Intel (3.1.6), each with
the Vulkan validation layer on (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`), plus a package with `fsr_sdk` removed (expects the
FSR 3.1 fallback and the launcher hint).

## FSR with OptiScaler, and the built-in FSR 4 on Radeon (2026-10-31)

**"dispatch failed: runtime error (3)".** OptiScaler recognizes AMD's FFX DLLs by file name
(`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`) and hooks their exports. With its
FFX inputs on, our FSR SDK and fsr4vk calls become its own NGX feature. With `VulkanUpscaler=ffx_12`
that feature needs `vkGetDeviceProcAddr`, gets none ("InGDPA does not exist"), and fails with
"Failed to load Vulkan external memory functions"; the dispatch then returns 3. While OptiScaler is
loaded, the engine now loads the upscaler DLL directly from a hard link with a private name in
`engine/out/ffx-private`, which the hook does not match (`BB_OPTISCALER_FFX_PRIVATE=0`: the old way;
`=1`: the private name without OptiScaler, for tests). If a dispatch still fails with OptiScaler
loaded, the message names `EnableFfxInputs=false`.

**8-10 FPS with "FSR 4" on an RX 6700 XT.** That entry is the built-in v07 INT8 model
(`fsr4_shaders`, no longer in the Windows package, still in old installs): about 90 ms a frame there,
against 87 FPS with FSR 3.1 in the same session. On AMD with the SDK present, `fsr4` now runs through
the SDK and the menu entry is disabled (`BB_FSR4_BUILTIN_AMD=1` keeps the old model). Any FSR 4 path
under 20 FPS for 5 s falls back once per session with a message (`BB_FSR4_SLOW_GUARD=0`).

**Not verified on AMD.** Built and checked on NVIDIA only; these paths need a player on an RX 6000.

## DLSS jitter while the camera moves (2026-10-31)

Measured on an RTX 5060, 1280x720 -> 1920x1080, preset K, 40 presented frames, a scripted camera pan
of about 5.7 px a frame. The image's frame-to-frame shift alternated every frame (-5.60, -5.77, -5.58,
-5.73...) and was smooth with jitter off (about +/-0.02). The inputs are right: per block, the input's
shift equals the motion vector plus the change in jitter, and flipping the jitter sign doubles the
flicker of a still camera. DLSS trusts the new frame more in motion, so part of the jitter reaches
its output. Other things tried, with the camera still: the extra -1 mip bias (flicker 0.58 with it,
0.54 without), bloom off (no better), preset E (0.79, worse), FSR 3.1 (0.67).

The camera's own movement already gives sub-pixel coverage, so with DLSS the jitter can fade out as
the camera moves: full up to 0.25 render px a frame, none from 1 px a frame. Mean second difference of
the shift: 0.32 px with full jitter, 0.035 px with the fade; sharpness 63.5-65.6 and 63.4, within noise.
NVIDIA's guide (8.4) lists jitter only on a static camera among differences that can cause corruption,
so the fade is an experimental setting, off by default (`dlss_jitter_fade`, the menu,
`BB_DLSS_JITTER_FADE`). See [dlss-audit.md](dlss-audit.md).

**Not measured yet:** slower pans, walking, and the 1440p path (`RunScaled`, tonemapped input).

## Native TAA and missing FSR 4 assets (2026-10-01)

`upscaler=taa` / `BB_UPSCALER=taa` is a separate native-resolution temporal AA
mode, available live in the launcher and overlay. It uses the existing jitter and
camera/object motion vectors, reprojection, depth rejection and 3×3 neighborhood
clipping in one compute pass with two ping-pong RGBA16F histories. History stores
depth in alpha; the scene's original alpha is preserved by the HDR merge. HUD is
composed afterwards and never enters history. Output changes and AA/provider
changes reset history. TAA uses Native AA dimensions while preserving the saved
FSR preset, and does not allocate an FSR context or execute an FSR model/RCAS.
It still adds GPU work compared with disabling temporal AA; native 4K raster cost
also remains. The explicit `BB_RENDER_RES` compatibility path must be unset.

FSR Native AA is not a fast pass-through: it renders at the output resolution and
also executes the temporal reconstruction (including the model for FSR 4) and
optional sharpening. It can therefore be slower than no AA at the same output.

FSR 4.1.1 assets have separate 1080p/2160p tiers and standard/Ultra models:
`t1080_m0`, `t1080_m1`, `t2160_m0`, `t2160_m1`. Outputs above 1080p need the 2160
tier, irrespective of the preset. A missing `t2160_m0/spd.spv` means missing
assets, not a GPU feature failure. These assets are not bundled: build the full
set with `tools/fsr4cap/build_assets.sh` and put it in `BB_FSR411_DIR` or the
packaged data directory's `fsr4_411`. Fatal asset failures now select FSR 3.1 in
the live settings, so the menu shows the active provider, retains the error, and
allows retrying FSR 4 after changing the output/installing assets.

`taa-shader-test` checks real SPIR-V output for accumulation, clipping, depth
disocclusion, invalid history and out-of-frame motion on Vulkan (Lavapipe works).
`out/taa-appimage-build.log` records these checks, settings round trips and 64
Python tests. `out/taa-live-validation.log` and `out/taa-appimage-validation.log`
check TAA at 1080p/720p/1440p/4K, TAA ↔ FSR 3, and a missing 2160-tier FSR 4.1.1
model in one gameplay process, including output captures and camera movement.
`out/taa-launcher-validation.log` checks actual GTK controls and saved settings.
TAA has not yet been tested on the tester's GTX 1060.

## Startup patch is the default again for outputs other than 1080p (2026-10-02)

The live path below made the Steam Deck and a GTX 1060 + 4-core Haswell drop to 7–8 FPS
(release 0.1 AppImage: 40–50 FPS on the Deck). With guest allocations at 1920×1080 the game's
post-processing stays at 1080p even for 720p output, guest compute passes resolve scene proxies
back to 1080p every frame (RX 7800 XT, 720p FSR 3 Performance on 4 cores: 826 vs 576
draws/frame, 185 vs 210 FPS), and on GPUs without shader stencil export every depth/stencil
copy took nine draws.

- `run.sh` again patches the game's render size for outputs other than 1080p (as in 0.1) and
  marks it with `BB_AUTO_RENDER_RES=1`, so an in-game restart recomputes it. Preset and output
  changes then need a restart (the menu offers it). `BB_LIVE_RES=1` selects the live path.
  1080p output and TAA keep the live path.
- Depth/stencil scene proxies need `VK_EXT_shader_stencil_export` again; without it these
  targets stay native (as in 0.1). `BB_SCENE_STENCIL_BITS=1` still forces the portable
  resampler for tests.

## Live resolution changes (2026-10-01)

The in-game output selector now resizes 720p/1080p/1440p/2160p host targets at a
display-pass boundary. Presets use `output / scale`, including raster sizes above
1080p (4K Quality: 2560×1440; Native AA: 3840×2160). Guest allocations stay at
1920×1080; `run.sh` no longer inserts a resolution patch for normal output choices.
The explicit `BB_RENDER_RES` compatibility override is retained.

`SceneTargets` resolves and retires old proxies before changing size. FSR resources
and history are rebuilt for the new input/output, with completion waits for both
FSR 3 and FSR 4. The 1080p path keeps HDR reconstruction before game post;
other outputs reconstruct tonemapped scene proxies at the first Scaleform pass,
then draw HUD/menu geometry into the output-size target. The display buffer is
resized too. Movie shader identification prevents a native-size fullscreen post
pass from being mistaken for UI.

Guest compute passes retain their native dispatch sizes and can resolve proxies
back to 1080p. This change does not scale every post-processing pass; performance
and intermediate detail can differ from the old startup-patched render sizes.

Verification: `out/dev/live-resolution-summary.log` switches output, preset and FSR
off/on in one PID. `BB_PRESENT_DUMP_TRIGGER` captures the completed host display
buffer (including HUD) into `BB_DUMP_DIR`. Captures confirm actual 720p/1440p/2160p
buffers. `scene-resolution-test` covers color/depth/stencil round trips through
proxies both smaller and larger than the guest allocation. `BB_PRESET_FILE` accepts
`preset [output-index [upscaler-index]]` for scripted menu-equivalent changes.

Frame analyzer: `BB_CAPTURE_TRIGGER=<file> BB_CAPTURE_DIR=<dir>`; creating the file records
the next frame (boundary: the pass writing a display buffer) — passes, targets, shaders,
sampled textures, and the first 1 KiB of bound constants for small passes and first draws.

## Bloodborne's frame (1920x1080, Hunter's Dream / Nightmare)

| Passes | What |
|---|---|
| first pass of a frame | copy of the previous UI target into the display buffer (sRGB) |
| shadow | 4096x4096 D32 depth |
| G-buffer | 6 targets 1920x1080 (RGBA8 x3, sRGB albedo, B10G11R11, RGBA16F) + D32S8 depth |
| lighting | light volumes into two B10G11R11 targets |
| scene color | RGBA16F 1920x1080, also forward/transparent draws and effects |
| volumetric fog | compute, reads linear depth (R32F 1920x1080) and composites into scene color |
| half-res effects | RGBA8 960x540 with half-res depth |
| post | combine/bloom pyramid in RGBA16F / B10G11R11 |
| tonemap | into RGBA8 1920x1080 (the UI target) |
| game AA | ping-pong RGBA8 1920x1080 after tonemap (to be skipped when upscaling) |
| UI | stencil-masked draws over the same RGBA8 target |

There is no velocity buffer, even with the camera moving: motion vectors are computed.

## Scene constants (864 bytes, bound by most passes)

Signature: `[0]=3000 (far) [1]=1/3000 [4]=1920 [5]=1080 [6]=1/1920 [7]=1/1080`.

| Floats | Meaning |
|---|---|
| 8–19 | view matrix, 3x4 rows (rotation + translation); the only block that changes with the camera |
| 36–51 | inverse projection (0.700285 = 1/1.42799, 0.39391 = 1/2.53865) |
| 52, 57, 62, 63 | projection (rows 52–67), D3D depth 0..1: x 1.42799, y 2.53865, z 1.00002 / -0.0500679 (near 0.05, far 3000) |
| 112–175 | shadow cascade matrices |
| 180–191 | inverse view (camera to world), 3x4 (176–179: 2, 8, 15, 0) |

The previous frame's matrices are not there; the port keeps them itself.

## Plan

1. Find the scene constants every frame (signature), keep the previous view/projection.
2. Camera motion vectors: compute pass from depth and current/previous matrices into an
   RG16F target; debug view to check them. Object motion later (vertex shader replay with the
   previous frame's constants through the recompiler).
3. Jitter: sub-pixel offset of clip-space positions in the scene passes (recompiler).
4. Upscaler at the scene color stage (before post and UI): FSR 3.1 first (open, native Vulkan,
   also frame generation), then DLSS (native on Linux), XeSS/XeFG and OptiScaler through a
   loader for their Windows DLLs.

## Status

- Camera motion vectors: verified by reprojecting the previous frame (`BB_DEBUG_MOTION=1`,
  toggles 1<<22 reprojected frame, 1<<23 error map): static geometry matches.
- FSR 3.1 (`BB_UPSCALER=fsr3`, FireBurn/FSR-Vulkan submodule): scene color before the
  post-processing combine (compute shader 9a9cf8a9), 1:1, RGB written back (the game keeps data
  in the alpha). Toggle 1<<24 switches it off at run time.
- Jitter: viewport offset of scene geometry (drawn with the scene depth, not full-screen
  quads), Halton(2,3) 8 phases; sign checked by sharpness (correct 255, flipped 208, off 271 —
  1:1 jitter trades high-frequency aliasing for a slightly softer image). Toggle 1<<25 off.
- Frame-to-frame difference while standing still: ~33% lower with FSR.
- Missing: motion of animated objects (characters, cloth, foliage), reactive/transparency
  masks for particles and fog, render-resolution scaling, frame generation.


## 2026-10-01: FSR 4 did not work at 1440p/2160p output (flicker and shaking)

Video `video_2026-10-01_03-58-59.mp4` (3840x2160 output, FSR 4 Performance): every object flickered
and shook. The cause: in this mode the upscaler did not run at all while jitter stayed on, so the
screen showed the stretched scene frame, each frame shifted to its own Halton phase.

1. The UI pass was recognized by the exact size of the `BB_RENDER_RES` target (1916x1078), but the
   game allocates targets with an aligned height (1916x1080; the scene constants say 1916x1080
   too). `RunScaled` was never called (the log had no `UI: native composition`). Alignment of up to
   8 pixels is now allowed (`RenderTarget`), and the scene size for FSR comes from the scene
   constants (`CameraMotion::RenderSize`, `SceneSize`).
2. After that FSR 4 failed with `external image registration failed (-1000069000)`: `RunScaled`
   created new image views every frame, and FSR 4's registry holds eight. It now uses the same
   `CachedView`s as the Native AA path.

Checked with a dump (`BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<folder>`, `BB_DUMP_FRAMES`, 8 by default:
FSR's input, motion vectors and output, raw): with the camera still, the PSNR of consecutive output
frames is ~45 dB against ~28 dB for the jittered input; turning the camera and walking leave no
trails. FPS in this mode is ~107 instead of ~220: before, FSR 4 simply did not run.

Left: sprites (4 indices each) drawn into the scene colour with the scene depth (glows, small
lights) are still not jittered (the rule "≤ 6 indices = a full-screen pass").

## 2026-10-01: a faster FSR 4: the post pass (2.9 → 0.8 ms at 4K)

Measurements: `BB_FSR4_PROFILE=1` (the time of each FSR 4 pass, a provider patch in the submodule,
see `gpu/patches/fsr-vulkan`), the out-of-game benchmark `out/gpu/fsr4-bench` (`ninja -C out/gpu
fsr4-bench`; `--stats`: registers and instructions from RADV). 4K Balanced (2260x1272 → 3840x2160),
RX 7800 XT: all of FSR 4 takes 5.8 ms, of which **post 2.5–2.9 ms** (not the neural network: the
last layers, a 2x2 pixel shuffle and blending with the history), the model's 12 passes 2.4 ms, pre
0.55.

The cause: each thread computes a 2x2 block of output pixels and writes them one by one into three
images (the recurrent state, the history, the output), so each write instruction of a wave writes
every other pixel. Removing any one of the three writes made the pass 1.3–4 times faster with the
same code.

The fix: `tools/fsr4_optimize.sh` decompiles post (spirv-cross), `tools/fsr4_post_lds.pl` gathers
the workgroup's 16x16 block in shared memory and writes whole rows, glslang compiles it back into
`fsr4_shaders/opt/`; `vk_fsr4.cpp` takes it from there (`BB_FSR4_OPT=0`: the original). Two
subtleties found by comparing outputs:
- spirv-cross translates the signed int8 unpack (`OpBitcast` to `i8vec4`) as `unpack8(uint)`
  (unsigned): without a fix the result is entirely different (PSNR 17 dB);
- values in shared memory must stay float: the compiler turns half from shared memory into a
  16-bit write, which rounds differently in unorm8.

`tools/fsr4_verify.sh` compares the original and the optimized post for all presets at 1080p,
1440p and 4K output on pseudo-random inputs: 1440p and 4K are bit-identical; post at 4K 2.1–3.5 →
0.81–0.87 ms, 1440p 1.2–1.5 → 0.36–0.38 ms, 1080p 0.37–0.69 → 0.20–0.22 ms. In the game (4K
Balanced): FSR 4 5.8 → 4.0 ms, the GPU frame 13.1 → ~11.6 ms; beyond that the FPS is limited by the
CPU (the GPU command thread waits for guest memory copies, "host copies" ~20%).

Found along the way: **the original FSR 4 at the 1080 tier (1920x1080 output) is nondeterministic**:
from run to run on identical inputs a stripe at the left edge (columns 0–132) changes, so there is a
race or a read of uninitialized memory somewhere in the model or provider. At 1080p output this can
show as flicker at the left edge of the frame. Not investigated.

The reactive mask is no longer computed with FSR 4 (FSR 4 does not take it).

### Also checked (same place, 4K Balanced)

- **A/B in one process** (`ab.sh`, bit 24: FSR off, the UI copy without upscaling): 86.5 FPS with
  FSR 4 against 87.7 without. After the post optimization the frame is limited by the CPU (the GPU
  command thread, waiting for guest memory copies), not the GPU. Async compute for FSR (overlapping
  it with the start of the next frame) would give no more than this ~1%: postponed until the CPU
  side is faster; it would also need the UI and output commands of frame N reordered after frame
  N+1's work.
- **WMMA (`VK_KHR_cooperative_matrix`, supported by RADV on RDNA3)**: a prototype of the model's
  pass 1 (a 16-channel residual block: 3x3 16→16, 1x1 16→32 ReLU, 1x1 32→16): 0.85 ms in the first
  version, 0.49 ms with a word layout in shared memory, against 0.30 ms for the original dot4. The
  arithmetic on WMMA would take ~0.1 ms, but with 16 channels the tile layout, epilogues through
  shared memory and occupancy (6 waves per SIMD, limited by LDS) eat the gain. The ceiling is about
  1 ms for all 12 passes with careful hand work on each; not started.
- `RADV_PERFTEST=cswave32` (wave32 for compute): FSR 4 is slower, 4.2 → 5.4 ms.
- The pre pass (0.54 ms) is not limited by its writes (0.50 ms without them).

## 2026-10-01: FSR 4 flicker at the left edge at 1080p output: a race in the model's pass 11

The 1080 tier's nondeterminism (see above) is a bug in the v07 model's shader, pass 11 (the
decoder, 1/4 → 1/2 resolution). Each thread is one input pixel at 1/4 resolution and writes a 2x2
block of the 1/2 resolution output. The dispatch rounds the width up to 64 threads, and threads past
the input's width do not stop: at the 1080 tier (an input 480 wide) threads 480..511 write output
pixels 960..1023, that is the first pixels of the next row (any tensor's row is 15360 bytes), and
race with their real writers. At 4K output the width 960 divides by 64, so there is no error.

`tools/fsr4_pass11_guard.pl` adds an early exit for threads past the edge (the size comes from the
pass's own neighbour check); `tools/fsr4_optimize.sh` builds it into `fsr4_shaders/opt` for all
presets together with post; `vk_fsr4.cpp` and the benchmark take any pass from `opt/`.
`tools/fsr4_verify.sh`: 1440p/4K are bit-identical to the original (the guard does not trigger
there); 1080p gives the same result from run to run, and differs from the original after the first
frame only in the former stripe at the left edge.

Found at the same time: spirv-cross translates every signed int8 unpack (`OpBitcast` to `v4char`)
as the unsigned `unpack8(uint)`; pass 11 has nine different forms of it. The fix is shared by the
scripts: `tools/Fsr4SpirvCrossFixes.pm` (every `unpack8` through a signed helper).

## 2026-10-01: FSR 4.1.1, what is inside and how to run it on Vulkan (exploration)

Source: `amd_fidelityfx_upscaler_dx12.dll` 4.1.1.2740 from OptiScaler (`FSR4_LATEST`).
- The DLL has the provider `ffxProvider_FSR4_Int8` / `Fsr4Int8UpscalerModel` (version "FSR4-i8
  4.1.1"). 1028 DXIL containers (25.7 MB): the model passes `fsr4_model_v07_fp8_no_scale_pass1..12`
  (+ `_post`), 270 `prepass` variants, 144 `postpass`, `fsr_rcas_pass` and a fallback FSR 3.1/2.
  Nothing is compressed.
- Despite "fp8" in the name, the model passes are plain DXIL with `dot4AddPacked` (int8 dot4),
  without AMD extensions (WMMA). The architecture is the same v07 (12 passes), but the weights and
  biases are not built into the shader: they are read from `InitializerBuffer` (t18, a typed
  buffer), the tensor sizes from the cbuffer `CsTensorSizes`, scratch is `u11`. Each pass has 9
  variants: the grid is built in (tiers 1080 / 4K / 8K: 480x270, 960x540, 1920x1080 for pass 5) and,
  apparently, the presets.
- `amdxcffx64.dll` (loader 2.3.0.2913, which Proton loads) has the same FP8 FSR 4 passes and ML frame
  generation (`mlfi_*`), no separate INT8 model. `FSR4_INT8` 4.0.2 is `v07_i8`, which the port
  already uses.
- dxil-spirv (HansKristian-Work/dxil-spirv, built from source) translates the passes into correct
  SPIR-V: `dxil-spirv <pass> --enable-shader-i8-dot --ssbo-uav --use-reflection-names`.

What a provider of our own for 4.1.1 lacks: which container is which (preset, tier, pass), the
contents of `InitializerBuffer`, the values of `CsTensorSizes`, the prepass/postpass permutations,
constants and dispatch sizes. The most reliable path is not to take apart the provider's x86 code
but to record one frame: a small test program on the FFX API (DX12, MinGW) under Wine/Proton with
vkd3d-proton, the "FSR4-i8" provider chosen explicitly through a version override, and a Vulkan
layer that writes the pipelines (SPIR-V from dxil-spirv), buffer contents and dispatch sizes. Then
replay it with our own provider in the same benchmark (`fsr4-bench`), checked against the recorded
output.

## 2026-10-01: FSR 4.1.1 in the port (upscaler=fsr411)

AMD's 4.1.1 model (INT8) runs natively on Vulkan and matches the DLL bit for bit.

**How it was obtained.** `tools/fsr4cap/fsr4cap.exe` (C, MinGW) calls the FidelityFX API 2.3 from
`amd_fidelityfx_loader_dx12.dll` with an explicit version 4.1.1 and, by replacing D3D12 method
tables, records everything the DLL does under vkd3d-proton: pipelines (DXIL), root signatures,
descriptors, uploads, constants, dispatches. A frame is 29 dispatches: auto-exposure SPD, prepass,
pass0_post, model passes 1..12 (each followed by `_post`, clearing the tensor's border), postpass,
RCAS.
- There are two models: m0 (Native..Performance) and m1 (Ultra Performance, ratio 3.0), with 128 KB
  of weights (`InitializerBuffer`). Tiers: t1080 (output up to 1920x1080), t2160 (larger, including
  2560x1080).
- Tensors are computed from the output aligned to 8; the `CsTensorSizes` table (17 sizes), the group
  counts and the constants (MLSR as in v07, but width/height not aligned and `inv_scale = 1 /
  scale`; SPD; RCAS) are checked by `extract.py` on 20 recordings.
- dxil-spirv translates the DXIL the same way vkd3d-proton does: `--mixed-float-dot-product`
  (`VK_VALVE_shader_mixed_float_dot_product`, dot2 on half with accumulation in float); without it
  the PSNR is 40 dB instead of an exact match. `--class-bindings` (a patch) lays out the SRV, UAV,
  CBV and sampler registers at bindings +0/+32/+64/+96, and the runtime binds resources by name.
- `fsr411/fsr411.cpp` is an independent Vulkan core (the game and `fsr4-bench --fsr411`).
- `verify.sh`: the DLL and the replay on identical inputs, 8 frames: bit-identical for 1080p (both
  models), 1440p, 2560x1080 and 4K (both models). 1600x900 differs only at the right edge, where the
  DLL itself is nondeterministic (a race in its shaders when the tensor width is not a multiple of
  64).

**Speed-up.** The postpass was rewritten with workgroup memory at the level of SPIR-V assembly
(`postpass_lds.py`; going through GLSL is inexact: `DenormPreserve` and the exact placement of
`RelaxedPrecision` are lost, and something else changes the result too): 2.18 → 0.95 ms at 4K, bit
for bit as the DLL. All of FSR 4.1.1: 1.09 ms at 1080p (1280x720), 4.2 ms at 4K Balanced. In the
game (4K Balanced) the upscaler takes 4.12 ms against 4.32 ms for the accelerated v07.

**Building the assets** (by the user, from their own DLLs; nothing from AMD is in the repository):

    bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> <amd_fidelityfx_loader_dx12.dll 2.3.x>

Needs nix-shell (or MinGW, cmake, ninja, python3, SPIRV-Tools, umu-run), and network access for
dxil-spirv, the FidelityFX SDK headers and Proton (GE-Proton). The DLLs come with OptiScaler, for
example (`FSR4_LATEST/`), and with games that have FSR 3.1/4. A build from scratch gives the same
files; `VERIFY=1` at the end compares with the DLL.

**A correction to the A/B above ("the frame is limited by the CPU").** Those measurements ran while
12 hung shadPS4 processes each took a core. On an idle system: "host copies" 1.6%, waiting for the
GPU ~41%: the frame is limited by the GPU, so async compute for FSR makes sense again.
