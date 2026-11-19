# DLSS audit against NVIDIA's guides (2026-10-31)

Sources: NVIDIA's DLSS Super Resolution Programming Guide 310.6.0 (`NVIDIA/DLSS`, `doc/`), and
Streamline's `ProgrammingGuide.md`, `ProgrammingGuideDLSS.md`, `ProgrammingGuideDLSS_G.md` and
`ProgrammingGuideReflex.md` (`NVIDIA-RTX/Streamline`). Measurements: RTX 5060, 1280x720 -> 1920x1080.

| Item | Guide | Ours | Verdict |
|---|---|---|---|
| Placement, 1080p output | 3.1: before tone mapping, early in post | HDR scene colour before the post combine | Correct |
| Placement, other outputs | 3.1: "must occur ... before tone mapping" | Default: `RunScaled`, the finished, tonemapped frame. `upscale_before_post=1` / `BB_UPSCALE_EARLY=1`: the game runs at output size and the scene is upscaled before post, as at 1080p | **Wrong by default; fix behind a switch, one 1440p boot only** |
| HDR/LDR flag | 3.1.2: LDR must be 0-1, perceptually encoded | Float formats HDR; tonemapped RGBA8 UNORM LDR | Correct |
| Motion vector format | 3.6.1: RG16F, render pixels, current to previous | Same; per-block image motion matches on a pan | Correct |
| Motion vector flags, scale | 3.6.2, 3.6.3: MVLowRes, MVJittered 0, scale 1 | Same | Correct |
| Depth | 3.8: near 0, far 1 unless flagged | Near 0, far 1, no flag | Correct |
| Jitter pattern, phases | 3.7.1: Halton, 8 x ratio squared | Same (18 at 1.5x) | Correct |
| Jitter method, range, units | 3.7.2, 3.7.3: viewport or camera, +/-0.5, render pixels | Viewport, same range and units | Correct |
| Jitter sign | 3.7.3: same system as motion vectors | Flipping it doubles still-camera flicker | Correct |
| Jitter while moving | 8.4 lists "apply jitter only when the camera is static" among differences that "can lead to graphical corruption" | Fade from `e9a14f00` was on by default | **Wrong, fixed**: now an opt-in experimental setting, off by default |
| All input jittered | 8.1: "all input to DLSS is properly jittered", secondary passes too | Sprites of 4-6 indices are not; full-screen lighting reconstructs from the unjittered camera | **Wrong, not fixed** |
| Exposure | 3.9, 3.10: exposure texture preferred, auto-exposure allowed | HDR: auto-exposure; LDR: none or the menu's | Correct |
| Pre-exposure | 3.9.2: only if the engine uses one | Not used | Not applicable |
| Reset | 3.13: first frame after a major transition | Setting changes and frames without the upscaler; no camera cuts | **Wrong, fixed**: a jump of half the render width in a frame resets too, and so do a load request clearing (lamp arrival, respawn, also with fast loading) and seamless co-op's revive and move to safe ground, which have no loading screen |
| Mip bias | 3.5: log2(render/display) - 1, may be eased | Same on G-buffer passes, toggle and slider | Correct |
| Bias mask | 3.15: "should not be used", only preset F reads it | Sent with every preset when the reactive mask is on | **Wrong, fixed**: never sent; F is no longer offered |
| Presets | 3.12: A-D removed, G-I reserved, E and F deprecated; K for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance, K throughout on RTX 20 and 30 | Menu: auto, J, K, L, M, default; a saved E or F runs auto; auto follows NVIDIA per mode and GPU generation (device ID below 0x2600: RTX 20 and 30) | Correct |
| Colour input | No NaN or Inf: one spreads through the history | Motion vectors cleaned; the HDR scene colour checked for NaN and Inf in the motion pass, such pixels written black | Correct |
| Command buffer state | Vulkan: the evaluate call may change pipeline, descriptors and push constants | Every draw and dispatch binds its own; dynamic state is marked dirty after the evaluate | Correct |
| Application or project ID | 2.5, 5.2.1: NVIDIA's ID, else project ID with engine type and version | NVIDIA's sample ID 231313132 | **Wrong, not fixed by default**: `BB_DLSS_PROJECT_ID=1` uses the project ID; see Check boots |
| Render sizes | 3.2.2: optimal settings query | Own sizes, multiples of 4 (1708 where 66.7% is 1707) | Deviation, not fixed |
| Resource states | 3.4: inputs sampled, output storage | Same | Correct |
| Library | 2.5, 4.2.2: production DLL, NVIDIA signature intact | `nvngx_dlss`, `nvngx_dlssg`, `sl.interposer`, `sl.dlss_g`: Authenticode valid, signed by NVIDIA | Correct |
| Interposer check | Streamline 2.1.1: verify the signature | `verifyEmbeddedSignature` before loading | Correct |
| Frame Generation inputs | DLSS-G 17.0: depth, vectors, Hud-less, UI, null when invalid | Same | Correct |
| Frame Generation constants | DLSS-G 17.0 names `renderingGameFrames` | The 2.14.16 header we build with has no such field: the guide is ahead of it | Not applicable |
| Reflex | Reflex 8.0: sleep and markers always sent | Yes, but simulation start and end are sent together: the game's own loop is not seen | Partly; not fixed |
| Streamline ID | Streamline: application ID from NVIDIA, optional project ID | Sample ID | Same switch as above |

## Check boots

RTX 5060, 1080p, windowed, on a copy of the saves. Without Streamline: the project ID init succeeded,
DLSS ran on preset K, a presented frame looked right.

| Build | Streamline | Presented per game frame, 5 s windows |
|---|---|---|
| Project ID | Yes | 1.00 in all 9 |
| Beta 24 package, sample ID | Yes | Mostly 1.00, with 1.51 and 1.92 in two |

Windowed mode largely stops generated frames in both, so the 1.00 is not caused by the project ID alone.
But only the old ID showed any, and two boots cannot clear it, so the project ID stays behind
`BB_DLSS_PROJECT_ID=1` until a fullscreen boot shows 2.00 with it. Not covered: a real camera cut, AMD,
Vulkan validation.

## Before post-processing at other outputs

`BB_UPSCALE_EARLY=1` at 1440p: DLSS took the 1708x960 HDR scene, picture and HUD looked right. That boot
reset history every frame (flicker 14.2 against 0.45, image swinging 0.3 px); the reset is fixed and
the fix is built but not booted. It uses the 9152 MiB pool: about 1.8 GB of commit left in game on a
16 GB PC, against about 6.9 GB on the old path. One uncapped 120 FPS boot crashed during loading (a null
write in `EzWorkPool_Type`, guest+0x24a497e); the old path with the same settings did not. Not tested:
frame rate cost, FSR 3.1, Frame Generation, the pause menu, 4K. Keep it off.

## Presets on a pan

Swing every other frame at 1080p, single runs: L 0.16 px, J 0.17, K 0.18, M 0.24, jitter off 0.02.

## Jitter wobble

On a pan, the image alternates by about 0.18 px frame to frame; with jitter off it is smooth. The
colour and motion vectors handed over are consistent. Section 8.1's rule about unjittered passes is
the leading unproven cause: the lighting passes rebuild positions from the unjittered camera. Proving
it needs NVIDIA's SDK debug DLL (8.4.2) or a jitter in the game's own projection constants.

## DLSS 5

Not added. NVIDIA's pages describe a separate neural rendering stage, "optional and independent", not
a better upscaler, on RTX 50 GPUs, taking colour and motion vectors. Integration is described as
Streamline or an Unreal plugin, but Streamline 2.14.16's public headers have no DLSS 5 feature,
NVIDIA's DLSS download page lists none, and none of NVIDIA's pages name Vulkan or give licence terms.
Needed: a public plugin and header, Vulkan support, and redistribution terms.
