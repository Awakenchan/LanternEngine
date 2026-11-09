# Hardware compatibility rules

Every change is checked against the hardware players actually use before it ships. A change that
works on the development PC (RTX 5060, Intel CPU, Windows 11) has proven nothing about an AMD card,
a GTX 10, a laptop iGPU or a 4-thread CPU. 0.9.37 beta 6 shipped a cache fix tested on that one PC:
every RX 9070 XT player crashed at the start (below, "What went wrong").

## Who plays

From the logs players attached to the Discord #issues forum (0.9.33 to 0.9.37 beta 6, 2026-10-20
to 22). Re-count before each release (`tools/hardware_census.sh <folder of logs>`).

| GPU family | Seen in logs | Driver | Notes |
|---|---|---|---|
| NVIDIA RTX 40 / 50 | 4060 (most common), 4060 Ti, 4070, 4070 Ti Super, 4080 Super, 4090, 5060, 5070 Ti, 5090 | 581 to 621 | DLSS, Reflex, frame generation (40/50) |
| NVIDIA RTX 20 / 30 | 2060, 2070, 2080 Super, 3050, 3060 (+ laptop), 3070, 3080 | 552 to 617 | DLSS; no DLSS frame generation |
| NVIDIA GTX 16 | 1660 Ti, 1660 Super | 617 | Turing without RT/tensor cores: no DLSS |
| NVIDIA GTX 10 and older | 1050 Ti, 1070 Ti | 576, 582 | Pascal: the 580 driver branch is its last; fewer Vulkan extensions |
| AMD RDNA 1 to 4 | RX 5600 XT, 6600, 7700 XT, 7900 GRE, 9070 XT | 2.0.353 to 2.0.416 | FSR 4 on RDNA 4 |
| AMD Polaris / Vega, old APUs | RX 580, RX 580 2048SP, Ryzen iGPUs | 2.0.279 (legacy, Vulkan 1.3.260) | AMD's legacy driver: the oldest feature set we run on |
| AMD APU / handheld | Radeon Graphics (Ryzen Z2 A, 4600H) | 2.0.279, 2.0.353 | shared memory, "0 GB" VRAM |
| Intel | none seen yet | | Arc and Iris Xe expected; untested |

| CPU family | Seen | Threads | Notes |
|---|---|---|---|
| AMD Zen 2 to 5 | Ryzen 3600, 5600, 5600G, 5700X(3D), 7800X3D, 9600X, 9700X, 9800X3D, Z2 A | 8 to 16 (some with SMT off) | Zen 4/5 save AVX-512 state in exception frames |
| Intel 12th to 14th gen | 12400F, 12700K, 13600KF, 13700KF, 14600K, 14900K | 12 to 32 | hybrid P/E cores; the red-zone stores (patches/redzone.json) |
| Intel 4th to 9th gen | 4790K, 6402P, 7700HQ, 9700K, 9750H, 9900K | 4 to 16 | the i5-6402P has 4 threads and no SMT |

## CPU instruction-set baseline

The native game runtime requires **AVX, POPCNT and SSE4.2** (including OS-enabled AVX state).
These remain hard requirements; `BB_SKIP_CPU_CHECK=1` bypasses the check but does not emulate
them.

**MOVBE, BMI1 and LZCNT are optional host features.** On hosts that lack any of them, the
preparation step scans the linked executable and emits a per-image rewrite table. The loader
replaces only the affected guest instruction sites with compatibility stubs or breakpoint
fallbacks that emulate the original operation. CPUs that have all three features skip scanning
and execute the original image unchanged. The compatibility layer deliberately does not cover
F16C.

This makes Sandy Bridge and Ivy Bridge candidates when their AVX state, POPCNT and SSE4.2 are
available. The new path is still experimental until tested on those physical CPUs on both
Windows and Linux; CI's differential tests validate operation results and flags, not game
stability or frame rate.

## What each GPU family lacks

From players' logs ("Extension ... unavailable") and the Vulkan hardware database
(vulkan.gpuinfo.org, Windows reports). The engine takes another path for each missing one; those
paths run only on that hardware unless tested on purpose (`BB_VK_HIDE`, below).

| Family | Missing (besides the other vendor's own extensions) |
|---|---|
| RTX 20-50, GTX 16 | `VK_EXT_shader_stencil_export`, `VK_EXT_shader_atomic_float2` |
| GTX 10 and older | the above, `VK_KHR_fragment_shader_barycentric`, `VK_KHR_compute_shader_derivatives`, `VK_KHR_maintenance8` (580 branch) |
| RDNA 1-4 | `VK_EXT_attachment_feedback_loop_dynamic_state`, `VK_EXT_shader_float8` (not RDNA 4) |
| Polaris / Vega / legacy APUs | the above, `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_image_2d_view_of_3d`, `VK_KHR_maintenance5`, `VK_KHR_maintenance8`, `VK_KHR_compute_shader_derivatives`, `VK_KHR_cooperative_matrix`, `VK_EXT_shader_atomic_float2` |
| Intel UHD 630 (gpuinfo) | `VK_EXT_extended_dynamic_state3`, `VK_KHR_maintenance5`, `VK_EXT_attachment_feedback_loop_layout`, `VK_EXT_mutable_descriptor_type`, `VK_EXT_descriptor_buffer`, `VK_KHR_fragment_shader_barycentric` |
| Everything but NVIDIA | `VK_KHR_present_id` + `VK_NV_low_latency2` (NVIDIA boost), DLSS, NVX image handles |

The driver's age matters as much as the family: an RTX 3070 on driver 566 lacks
`VK_KHR_maintenance8` and `VK_KHR_compute_shader_derivatives`, as a GTX 10 does (0.9.37 beta 6 log),
and an RTX 3050 on 552 the same. Test the `gtx10` set for "an RTX card on an old driver" too.

Required by the engine (it stops without them; every family above has them): Vulkan 1.3,
`VK_KHR_swapchain`, `VK_KHR_push_descriptor`, `VK_EXT_vertex_attribute_divisor`,
`VK_EXT_robustness2` with robustBufferAccess2, robustImageAccess2 and nullDescriptor.

## Rules for every change

1. **Say which hardware it reaches.** Before writing it: which GPU families, drivers, CPUs and
   operating systems take the new code path, and which keep the old one. A change to what the
   driver is given (pipelines, SPIR-V, descriptor layouts, formats, extensions, memory types) reaches
   every GPU.
2. **Valid Vulkan, proven by the validation layer.** NVIDIA's driver runs much invalid usage that
   AMD's ends the process on (0xC0000409) and Intel's draws wrong. Any change that reaches the driver
   runs `tools/validate_pipelines.sh` (below) with no new validation errors, for each `BB_VK_HIDE`
   family, before it is merged. Known errors are listed in the release notes' working file until
   fixed; their count may only go down.
3. **Features by query, never by vendor or name.** Use the extension or feature the path needs.
   Vendor checks only for a driver bug recorded in "Known driver behaviour" below, with the log
   that shows it.
4. **A switch and the old path.** A new path that changes rendering, memory or timing gets an
   environment switch (`BB_...=0` back to the old path), named in the log line that says it is on,
   and the old path stays until a release has shown the new one on every family.
5. **Widening is a change too.** Making more of something reach the driver (more cached records
   read, more pipelines built at the start, a feature turned on by default) gets the same checks as
   new code: beta 6 changed no pipeline code and still crashed AMD, because ten times more cached
   pipelines were built.
6. **Crash guards end the crash in one start.** A guard that leaves out what crashed must not let
   the next start crash on the next item (beta 6: eight pipelines left out per start, a crash every
   start). After one crash the whole feature turns off for that driver and engine build.
7. **Old GPUs and small memory.** Defaults must run on a GTX 1050 Ti (4 GB), an RX 580 on the legacy
   driver, a 4-thread CPU and 16 GB of RAM; features that need more are turned on by what the PC
   has, never assumed. Shared-memory APUs report 0 GB of VRAM: budgets come from the memory heaps.
8. **CPU threads from the hardware.** Thread pools from the hardware thread count (4 to 32 seen),
   never a fixed number; hybrid Intel CPUs have slow E-cores, so latency-critical threads get a
   priority, not a core count. Exception frames on Intel 12th gen+ and Zen 4/5 are larger (AVX-512
   state): guest code's red zone must stay protected (patches/redzone.json).
9. **Both operating systems.** Windows 10 (19045) and 11, and Linux (X11 and Wayland, NVIDIA,
   AMD's Mesa driver). A Windows-only path needs its Linux counterpart or a reason in the code.

## Before a release

- Build Windows and Linux; run every test (`build.ps1 -Release`, the WSL tests).
- `tools/validate_pipelines.sh` for `none`, `nvidia`, `gtx10`, `rdna`, `polaris`: no new errors, the
  warm-up building every cached pipeline.
- A test boot on the development PC (with the player's permission rules for this repository): the
  start's warm-up line, no Display or STOP lines, a clean exit.
- The same test boot with `BB_VK_HIDE=gtx10` and `BB_VK_HIDE=polaris` on the development PC.
- `bb-shader-lab --check-cache` on a player's cache from each vendor, when one is at hand.
- The release notes name what was not tested (vendor, OS, feature) instead of implying it was.
- A pre-release first; the public release only after players on AMD and on an older NVIDIA card
  have started it.

## Test tools

- `BB_VK_HIDE=<extension names or families>`: the engine treats those extensions as missing, so a
  GPU family's paths run on another GPU. Families: `nvidia`, `gtx10`, `rdna`, `polaris`
  (`vk_instance.cpp`, HideExtensions). Logged as "Vulkan extensions hidden for testing".
- `tools/validate_pipelines.sh <phase> [family]` (Linux or WSL): the engine on Mesa's software
  Vulkan driver (lavapipe) with the Khronos validation layer; `profile` writes that GPU's profile,
  `seed` copies a cache and recompiles it for it, `warmup` builds every cached pipeline under
  validation, `play` compiles in play from an empty cache.
- `bb-shader-lab --check-cache <cache folder>`: what the warm-up would build, and why the rest is
  left out, without a GPU.
- `tools/chunk_buffer_probe.cpp`: the PC memory model's guest memory chunk calls on Windows (host
  import, the chunk route, the arena's binding) alone, for the validation layer on any driver.

## Validation baseline

`tools/validate_pipelines.sh warmup` on the development PC's cache (508 pipeline keys), lavapipe,
2026-10-22. The count of each may only go down:

| Error | Count | What |
|---|---|---|
| `VUID-VkGraphicsPipelineCreateInfo-layout-07988`/`07990` | 0 (was 18+ before the fix) | a stage's bindings did not start where the stages before it end: modules reused after another fragment shader (StageSpecialization ignored the start of stages without guest resources). 15% of a cache's pipelines; the AMD crash of beta 6. Fixed: the start is compared unless a stage has no bindings at all; the warm-up leaves out keys made by older engines |
| `VUID-RuntimeSpirv-OpEntryPoint-08743` (motion) | 0 (was 4) | a fragment shader reading the motion varyings with a vertex shader that does not write them; such cached keys are left out |
| `VUID-RuntimeSpirv-OpEntryPoint-08743` (tessellation) | 1 | a hull shader reading location 1 that its vertex stage does not write (upstream) |
| `VUID-RuntimeSpirv-Location-06272` | 109 shaders | the motion varyings at locations 30 and 31: within the 32 locations AMD and NVIDIA give, over 128 components once the layer counts gl_Position. Move them below the shader's own outputs if a driver objects |
| `VUID-VkDeviceCreateInfo-pNext-pNext` | 1 | maintenance8's features: newer than Ubuntu 24.04's layer, not an error |
| `VUID-VkGraphicsPipelineCreateInfo-Input-07904` (`polaris`) | 0 (was 3000) | without dynamic vertex input the vertex input is baked from the record; a record made where it was dynamic has none. The warm-up leaves such keys out |

The `polaris` family with the development PC's (RTX) cache builds 46 of 508 pipelines at the start:
the rest are records made with dynamic vertex input, compiled in play there. A cache made on the
card itself keeps its vertex input.

`run.sh --software` in a source checkout builds first: export the build environment (CC, CXX,
PKG_CONFIG_PATH, CMAKE_PREFIX_PATH as for build.sh) before the script.

The shader catalogs of 0.9.37 beta 9 (docs/pipeline_compiles.md), each seeded into an empty cache with
its own profile and checked with `bb-shader-lab --check-cache` (the layout rule above without a GPU:
each pipeline's layout as the warm-up builds it, against its modules' SPIR-V):

| catalog | keys built at the start | layouts that do not match |
|---|---|---|
| NVIDIA (RTX 5060 profile) | 869 of 877 | 0 of 828 |
| AMD, new (RX 7800 XT profile) | 771 of 771 | 0 of 728 |

The AMD catalog is a widening (AMD cards build 771 pipelines at the start where they built none), so it
went through the warm-up under the validation layer as `rdna` (2026-10-23, WSL with 12 GB): recompiled
for that profile 738 of 738 keys, all 738 built at the start, 375 messages, every one of a kind the
baseline above accepts: `Location-06272` (the motion varyings), `OpEntryPoint-08743` only for
tessellation control shaders reading locations their vertex stage does not write (the catalog has more
tessellation pipelines than the dev cache; the same modules compile in play), and `pNext-pNext`. No
layout error (`07988`/`07990`).

lavapipe runs out of room for GPU arenas: its largest buffer is just under 4 GiB, an arena is 4 GiB, and
`BufferCache::GetArena` fails (`ErrorOutOfDeviceMemory`, exit 23) once the game's memory needs a new
one, a minute or two into a start (with beta 8's GX region every time). The warm-up runs before that,
so `warmup` still validates the pipelines; `play` does not get far. Linux GPU assertions now print their
call stack (`shim/common/assert.cpp`), which found this.

## Lighting effects

GTAO, improved reflections, POM cavity, contact shadows, PCSS, indirect lighting, volumetrics, lamp fog,
point shadows and higher shadow sizes are off by default, each with its own setting. With all off, a draw
pays one branch and no pipeline or image exists. Rules they follow:

- Passes are built on first use. A failed layout, shader module or pipeline, a storage format the GPU
  lacks, or too little free video memory prints one line and turns that effect off for the session.
- A replaced fragment shader needs the same layout as the one it replaces: constant buffers as uniform
  dwords (`Profile::uniform_dword_layout`, so not without `uniformBufferStandardLayout`), bindings from 0,
  and the expected resource counts. Otherwise the game's shader stays, on that hardware and profile.
- SPIR-V patches (SSR composite, water, POM) match exact words. They match the NVIDIA profile of this
  engine (checked against the dev cache), so on other profiles they stay off and the log says so.
- `BB_UPSCALER=none` stops camera tracking, and the effects that need the camera stay off with it.
- With a reduced scene (internal resolution below 1080p) the passes use the reduced targets.
- `effect-pipelines-test` builds every pass with the validation layer listening.

Not checked on any GPU yet: picture and cost on AMD, Intel and older NVIDIA cards.

## Known driver behaviour

- **AMD (Windows driver 2.0.x)** ends the process (0xC0000409) in `vkCreateGraphicsPipelines` on
  pipelines NVIDIA builds: 0.9.34 beta 8 (warm-up guard added), 0.9.37 beta 6 (RX 9070 XT, every
  start). Validation on lavapipe showed those pipelines are invalid: descriptor bindings the shader
  uses that the pipeline layout does not declare, or declares as another type.
- **AMD**: no Mailbox present mode on some setups ("falling back to Fifo").
- **AMD legacy driver (Polaris, Vega, old APUs)**: Vulkan 1.3.260, no dynamic vertex input: the
  vertex input is baked into every pipeline (GetVertexInputs).
- **AMD, `VK_AMD_anti_lag`**: another community port reported the input stage sleeping about 190 ms
  every second or two on an RX 5500M and switched it off. Not seen by us, since no AMD card was at
  hand. Lantern runs the call on a thread of its own and the game waits at most `BB_ANTI_LAG_WAIT_MS`
  (50). Three overruns in 30 s end it until the next start ("AMD Anti-Lag 2: off until the next start").
- **AMD discrete, 8 GB of VRAM or less**: `BB_AMD_LOW_VRAM` turns on a vendor-checked set of rules
  ("GPU: low-VRAM rules on"), because the request was to leave NVIDIA and Intel paths alone and only
  reports from Radeon cards motivate them. `BB_AMD_LOW_VRAM=1` runs them on any GPU for tests.
- **NVIDIA, the PC memory model**: the arena lists no memory type a host import may use, and the
  engine binds imports anyway, outside the Vulkan spec ("Guest memory: ... outside the Vulkan spec").
  Treat it as unproven on every driver: the arena is not made for external memory either
  (`VUID-VkSparseMemoryBind-memory-02731`), and on lavapipe the GPU reads other bytes through such a
  binding than the CPU wrote (`tools/chunk_buffer_probe.cpp`). `BB_CHUNK_BUFFERS=1` moves bindings in
  one chunk off it (EMULATION_REMOVAL_PLAN.md, step 6 item 2).
- **NVIDIA Pascal (GTX 10)**: the 580 branch is the last driver; features in later drivers never
  reach these cards.

## What went wrong in 0.9.37 beta 6

The cache reader fix made every cached shader record readable again, so the start built about ten
times more pipelines. It was tested on one NVIDIA card. On AMD the driver ended the process while
building them; the crash guard left out eight pipelines per start and the next start crashed on the
next ones. Validation (rule 2) would have shown the invalid pipelines before release, and rules 5
and 6 would have kept the widening switchable and the guard to one crash.
