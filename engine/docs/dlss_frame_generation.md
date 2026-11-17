# DLSS Frame Generation and the upscaler options (2026-10-22)

`frame_gen=dlss` in `lantern.ini` (the menu's Frame generation > DLSS, the launcher's Graphics
page) runs NVIDIA DLSS Frame Generation (DLSS-G) through NVIDIA Streamline. Off by default;
loaded at start (a change needs a restart), switched on and off live once loaded. FSR 3.1 frame
generation (`frame_gen=fsr3`, `vk_frame_generation.cpp`) stays as it is and is the choice on
every other GPU.

## What NVIDIA requires (Streamline 2.14.16, 2026-10-16)

Sources: [Streamline releases](https://github.com/NVIDIA-RTX/Streamline/releases) (v2.14.16),
[ProgrammingGuideDLSS_G.md](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md),
[ProgrammingGuideManualHooking.md](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md),
[ProgrammingGuideReflex.md](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideReflex.md),
[ProgrammingGuidePCL.md](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuidePCL.md),
the SDK headers (`sl_dlss_g.h`, `sl_core_api.h`, `sl_core_types.h`) and the Vulkan interposer
source (`source/core/sl.interposer/vulkan/wrapper.cpp`).

- **Only through Streamline.** There is no public NGX API for frame generation; DLSS-G is the
  `sl.dlss_g` plugin (binary only) driving `nvngx_dlssg.dll`. Vulkan is supported
  (`FeatureRequirementFlags::eVulkanSupported`).
- **GPU and driver.** NVIDIA only (`eErrorNoSupportedAdapterFound` elsewhere). Frame generation
  needs an RTX 40 or 50 GPU; `DLSSGState::numFramesToGenerateMax` says how many frames this
  system may generate per real one: 1 on RTX 40, up to 5 (6x) on RTX 50. Native optical flow on
  Vulkan needs driver 527.64+ (Windows) / 525.72+ (Linux); without it DLSS-G runs optical flow
  in interop mode. Dynamic multi frame generation needs driver 595.41+ and is D3D12 only.
- **Windows.** Windows 10 20H1 or newer and Hardware-accelerated GPU scheduling on. Streamline
  ships Windows x64 binaries only: no DLSS-G on Linux.
- **Reflex is required** (`sl.reflex` + `sl.pcl`, `NvLowLatencyVk.dll` on Vulkan):
  `slReflexSetOptions` at least once, `slReflexSleep` and the PCL markers every frame, and the
  present markers carry the same frame index as the frame's constants and tags
  (`eFailReflexNotDetectedAtRuntime` otherwise).
- **Inputs per frame:** depth and motion vectors (required; the same as DLSS SR's), HUD-less
  colour and a UI buffer (UI alpha preferred) "strongly recommended", both exactly the back
  buffer's size; common constants once per frame (row-major matrices without jitter, `mvecScale`
  to [-1, 1], `renderingGameFrames`); all tags null and the mode off in menus, loading and
  movies.
- **Presentation is DLSS-G's.** It intercepts `vkCreateSwapchainKHR`, `vkGetSwapchainImagesKHR`,
  `vkAcquireNextImageKHR` and `vkQueuePresentKHR`: the host renders into off-screen images and
  DLSS-G presents real and generated frames from its own present thread and paces them. It waits
  for the present semaphore and signals the acquire semaphore; by default it blocks the host's
  presenting queue until it has read the inputs.
- **Limits on Vulkan:** no VSync with frame generation (D3D12 only), no dynamic multi frame
  generation, no FP16/scRGB swapchain (HDR10 only).
- **Cost** (NVIDIA's table): 1.1-1.5 ms at 1080p on RTX 4090/5080/5090 for 2x, 1.8-2.4 ms for 4x;
  about 270 MB VRAM at 1080p, 490 MB at 1440p, 725 MB at 4K.

## Licence

- Streamline's headers and source are MIT (`license.txt` in the SDK): the headers this engine
  uses are vendored in `gpu/third_party/streamline/include` with that licence.
- `nvngx_dlssg.dll`, `nvngx_dlss.dll` (the NVIDIA RTX SDKs licence, `nvngx_dlss.license.txt`) and
  `NvLowLatencyVk.dll` (`reflex.license.txt`) may be redistributed in object form inside an
  application with material functionality of its own, with the licence terms passed on, not as a
  stand-alone product, and not in a way that subjects them to an open source licence (section
  4.e). They are separate files loaded at run time (as `nvngx_dlss.dll` already is in the
  package), never linked; the package carries the licence texts next to them. Whether shipping
  them beside a GPL engine satisfies 4.e is the maintainer's call; `tools/fetch_streamline.sh`
  downloads them from NVIDIA's release instead of the repository holding them.

## What others do

- **OptiScaler** ([README](https://github.com/optiscaler/OptiScaler)): its frame generation
  (OptiFG: FSR 3 FG, FSR 4 FG, XeFG) runs on D3D12 only; on Vulkan it offers upscalers (DLSS,
  FSR 2/3.1, XeSS, FSR 4 through a D3D12 path) but no frame generation of its own. It never
  offers DLSS-G itself: games ship it or not.
- **NVIDIA Smooth Motion** ([Khronos news](https://www.khronos.org/news/permalink/nvidia-adds-vulkan-support-to-smooth-motion-frame-generation-a-boost-for-many-games-and-emulators)):
  a driver-level frame generation for Vulkan games on RTX 50, through a Vulkan layer in the
  NVIDIA app. It needs nothing from the game, but has no depth, motion vectors or HUD-less frame
  (the HUD warps), and it cannot be combined with DLSS-G.
- Vulkan titles with DLSS-G use Streamline the same way (interposer as the Vulkan loader).

## How it fits in the engine

```
guest flip ─► GPU command thread ─────────────────────────────► swap thread
              OnFrameStart: frame N (token, Reflex sleep,       Present(frame N):
              simulation/render-submit markers)                   blit frame → back buffer
              upscaler: depth + motion vectors (render size)      overlay (menu, FPS counter)
              before the first UI draw: HUD-less copy              HUD-less → back-buffer size
              PrepareFrame: inputs copied into frame N's slot,     UI alpha = final ≠ HUD-less
              HUD-less through the same post process               constants + tags (frame N)
                                                                   present markers, vkQueuePresentKHR
                                                                   → DLSS-G generates and paces
```

- **Loading** (`vk_streamline.cpp`): at start, with `frame_gen=dlss`, on Windows, an NVIDIA GPU
  and no OptiScaler: `sl.interposer.dll` next to the engine is checked (NVIDIA's signature,
  `WinVerifyTrust`), loaded, `slInit` asks for DLSS-G, Reflex and PCL (Vulkan, no OTA updates,
  frame-based tagging), and its `vkGetInstanceProcAddr` becomes the Vulkan loader for the whole
  engine. Its `vkCreateInstance` / `vkCreateDevice` add what the plugins need (extensions,
  features, its own queues after ours); everything else passes through to `vulkan-1.dll`. Any
  failure on the way is logged and the engine starts on `vulkan-1.dll` as before, with FSR 3.1
  frame generation in the menu.
- **Frame identity.** The GPU command thread numbers game frames; the number travels with the
  presentation frame to the swap thread, where the constants, tags and present markers of that
  frame go out together right before its present (`slSetConstants`, `slSetTagForFrame` with
  `eValidUntilPresent`, `ePresentStart` / `ePresentEnd`).
- **Inputs it already had:** the camera motion pass's motion vectors (RG16F, render pixels,
  previous - current: `mvecScale = 1 / render size`), the scene depth (copied to R32F,
  standard 0..1 depth), the HUD-less frame the FSR 3.1 path captures before the first UI draw,
  and the camera from the scene constants (view/projection without jitter, near 0.05, far 3000,
  vertical FOV). New: the HUD-less frame through the presenter's own post process (so it has the
  back buffer's colour), scaled into a back-buffer-size image like the frame itself, and a UI
  alpha image (1 where the final back buffer differs from the HUD-less one: the game's HUD and
  the engine's overlay alike).
- **Lifetime.** Depth, motion and HUD-less live in a slot per presentation frame, so the next
  game frame's copies never overwrite what DLSS-G still has to read; the back-buffer-size images
  are written on the presenting queue, which DLSS-G blocks until it is done with them.
- **Pacing.** DLSS-G presents and paces generated frames; the presenter shows only real frames
  (no half-interval wait, no extra swapchain image: those are FSR 3.1's). The fixed frame-rate
  presets (30, 60, 90, 120, 144) are fixed-timestep patches whose game speed follows the flip
  rate, so their flip rate is never lowered for frame generation: 60 game FPS shows 120 with one
  generated frame. Only with the Uncapped preset does the frame limit count frames shown (the game
  at limit / (N + 1), and with "even pacing" at refresh / (N + 1)). The first build halved the 60
  FPS preset to 30 under DLSS-G: slow motion and twice the input lag in the user's test (the same
  rule had done that to FSR 3.1 frame generation since 0.9.34 beta 9.9). VSync is not available
  with DLSS-G on Vulkan: use G-SYNC/VRR.
- **Off frames.** Menus, movies, loading screens and frames without the upscaler have no inputs:
  DLSS-G's mode goes to off (resources kept) and comes back with the next game frame. The
  in-game menu open also turns it off. Window resizes and fullscreen switches turn it off first.
- **HDR output** (0.9.37 beta 9): the HUD-less frame goes through the same HDR encode as the back
  buffer's frame (`Presenter::PresentHdr`), so DLSS-G gets both in the swapchain's HDR format;
  `BB_DLSSG_HDR=0` turns it off with HDR. FSR 3.1 frame generation interpolates the 8-bit frame
  before the encode, and the generated frame is encoded with the real frame's gain map in native
  mode.
- **Fallback.** No RTX 40/50, old driver, HAGS off, missing DLLs, OptiScaler loaded: the
  menu says why, the log names the Streamline result, and Frame generation offers FSR 3.1.

## OptiScaler's options against the engine's

| OptiScaler | Engine before | Now |
|---|---|---|
| Upscaler choice | FSR 3.1, FSR 4, FSR 4.1.1, DLSS, TAA, off | same |
| DLSS presets (render preset override) | `dlss_preset` auto/K/J/M/L/default | same; letters NVIDIA's 310.9 SDK removed (A-D) or ignores (G-I, N, O) are refused at load, deprecated E and F run auto |
| Quality presets / DLAA | Native AA (DLAA) .. Ultra Performance | + Custom: `render_scale` 1.0-3.0 (render = output / scale) |
| Ratio overrides per quality mode | fixed 1.5 / 1.7 / 2.0 / 3.0 | Custom covers it |
| Sharpening (RCAS) and strength | `sharpen`, `sharpness` 0-2 | same |
| Motion-adaptive sharpening | - | not added: RCAS runs at the output size before the motion vectors are, a follow-up |
| Mip LOD bias override | `dlss_mip_bias` (DLSS -1) | + `mip_bias` offset -2..+1 for every upscaler |
| Auto exposure | FSR 4 only | + `dlss_auto_exposure` (DLSS's own, any input) |
| Reactive mask and its parameters | on/off, scale, threshold, maximum | same (now safe at every output size, below) |
| Output scaling | `output_res` above the display (downscaled by the presenter) | same |
| Frame generation choice | FSR 3.1 | + DLSS (2x-6x where the GPU allows) |
| FPS limiter / Reflex | the engine's limiter and precise pacing | + Reflex (`reflex` off/on/boost, with DLSS-G loaded) |
| HUD handling (HUDFix) | HUD-less frame captured by the engine | same; DLSS-G also gets UI alpha |
| Init flags (HDR, inverted depth, jitter cancellation, MV resolution) | set by the engine from what it knows | not exposed: the engine's values are right by construction |
| FSR 3.1 velocity / shading / accumulation factors | - | not exposed: the vendored FSR 3.1.6 Vulkan API has no such inputs |
| XeSS | - | XeSS-SR through Intel's libxess.dll on Windows (`upscaler=xess`, docs/xess.md); XeSS-FG is D3D12 only |

## Player reports

- **Crash after DLSS preset K -> M with OptiScaler (winmm.dll).** `DlssUpscaler::Record` released
  the NGX feature after `scheduler.Finish()`, which submits the current command buffer, and then
  created the new feature and evaluated it into that same, already submitted command buffer
  (`f.cmdbuf`): recording into a pending command buffer. The driver faulted on the draw recording
  thread, inside OptiScaler's NGX hook. Any change of the feature's key (preset, quality mode,
  sizes, HDR) took that path, with or without OptiScaler. Now it waits only for the earlier
  submissions (`WaitSubmittedWork`, the feature is not used by the open command buffer yet) and
  records into the open one.
- **Reactive mask "busted" (blue noise, DLAA at 1360x768, Special K).** Two ways the mask could
  hand DLSS garbage: `upscale_reactive.comp` bounded its writes by the scene image's size while
  the dispatch and the mask are the scene's render size (the game's aligned targets are larger
  than the scene: writes past the mask's edge), and a mask made earlier in the frame survived
  `EnsureResources` making the images again later in the same frame (output, preset or upscaler
  change): DLSS was given a new, never-written image in an undefined layout. Now the shader
  bounds by the mask, and making the images again drops the frame's mask. Special K's own DLSS
  overrides are outside the engine.

## Settings

| `lantern.ini` | Env | Values | Live |
|---|---|---|---|
| `frame_gen` | `BB_FRAME_GEN` | `off`, `fsr3`, `dlss` | yes once loaded; `dlss` loads at start |
| `frame_gen_frames` | `BB_FRAME_GEN_FRAMES` | 1-5 generated frames per real one (DLSS) | yes |
| `frame_gen_cap` | `BB_FRAME_GEN_CAP` | Uncapped only: game at refresh / (generated + 1) | yes |
| `reflex` | `BB_REFLEX` | `off`, `on`, `boost` (with Streamline loaded; on while DLSS-G runs) | yes |
| `render_scale` | `BB_RENDER_SCALE` | 1.0-3.0, the Custom preset's ratio | as presets |
| `mip_bias` | `BB_MIP_BIAS` | -2.0..1.0 added to the scene's LOD bias | yes |
| `dlss_auto_exposure` | | 0/1 | yes (the feature is made again) |

## In-game results (RTX 5060, driver 617.42, 1080p, DLSS Quality, 60 FPS preset)

- Streamline 2.14.16 loads, `numFramesToGenerateMax` 5 (6x on this RTX 50). DLSS Super Resolution
  and DLSS-G share NGX on the device without trouble.
- `BB_FRAME_STATS=1`: `DLSS Frame Generation: 5.0 s: game 60.0 FPS, shown 120.0 FPS (2.00 per game
  frame), status 0` - every generated frame presented, no status errors.
- Found by the test and fixed: the halved flip rate (above); the FPS counter and the frame limit
  following Streamline's per-call frame count back and forth (now: on while DLSS-G's mode is on);
  the UI alpha in R8_UNORM, which Streamline's Vulkan format table does not have (refused on every
  present: no UI mask, HUD flicker) - now R16_SFLOAT.
- Streamline logs, harmless: `Hook sl.common:Vulkan:CmdBindPipeline is NOT supported` (command
  buffer state tracking, off here), `FC feedback` (flip metering), `VSync interval 1 not supported
  with FG` (Mailbox), `Invalid backbuffer resource extent` (no back buffer subrect tagged).

## Still to test in game

- 3x-6x on this RTX 50 (frame_gen_frames 2-5), and an RTX 40 card (2x only, the menu clamps).
- Outputs other than 1080p (the scaled path's HUD-less capture), a letterboxed window (the blit
  into the back-buffer-size HUD-less image), fullscreen switches and window resizes (mode off
  first).
- The in-game menu open (mode off), loading screens and movies (mode off, back on after).
- HDR output on (DLSS-G stays off and says so).
- A non-RTX GPU and a missing `sl.interposer.dll` with frame_gen=dlss (clean fallback, the menu's
  reason).
- Input lag with Reflex on / boost; FrameView's MsBetweenDisplayChange for pacing.
- The Custom preset (render_scale) at 1.0-3.0 with FSR 3.1, FSR 4 and DLSS; mip_bias at -2 and +1;
  DLSS auto exposure in dark areas.
- The two player reports: DLSS preset K -> M in game (with and without OptiScaler), and the
  reactive mask with DLSS DLAA at 1360x768.
