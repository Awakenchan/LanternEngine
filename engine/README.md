# Lantern Engine

The engine of Lantern Engine: it runs your own copy of *Bloodborne* (CUSA03173, update 1.09) on a
Windows or Linux PC. The game's own x86-64 code runs directly on the CPU, a runtime written for this
one game stands in for the PS4's system libraries, and the game's graphics are translated to Vulkan.
The launcher that starts it is in `../launcher`.

> **No game files are included.** You need your own decrypted dump. This project is not affiliated
> with Sony Interactive Entertainment, FromSoftware, AMD or NVIDIA.

## Where it comes from

Lantern Engine started from [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc)
(tag `v0.4.0-darkizuku-preview.1`), itself built on [bbport](https://github.com/deadinside28/bloodborne_pc)
and [shadPS4](https://github.com/shadps4-emu/shadPS4). Since then it has its own:

- **start and runtime**: `start/` (bb-start: the game's executable prepared once and reused, mods,
  patches checked against the game's code, the GPU cache kept across updates), `src/` (the runtime,
  saves, files read in place, pads with menu and keyboard bindings);
- **the direct path away from emulation** (docs/EMULATION_REMOVAL_PLAN.md): draw calls taken from
  the game's graphics library as records, the PC memory model (the game's memory used by the GPU in
  place), a native shader compiler for the game's shaders (docs/native_shader_compiler.md);
- **the renderer's performance work**: two-stage draw preparation on worker threads, cached render
  state, pipeline cache warm-up with a crash guard, occlusion culling, frame pacing;
- **the PC version**: native PC menus and keyboard/mouse controls, high frame rates, widescreen,
  HDR, DLSS, FSR 3.1/4/4.1.1, frame generation, trophies, online through The Hunter's Dream.

The renderer and shader recompiler are still shadPS4's at their core (`gpu/shadps4`, changes marked
`bbport:`); the direct path replaces them step by step.

## Before changing it

Read [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md): who plays (GPUs, drivers, CPUs), what each GPU
family lacks, the rules every change follows and the checks before a release. A change is not done
when it works on one PC.

## Build

- **Windows**: `build.ps1` in the repository's root (MSYS2 with the CLANG64 toolchain in
  `C:\msys64`, .NET 8 for the launcher). `build.ps1 -Version <v> -Release` makes the release zip.
- **Linux**: `build.sh` here (clang, CMake, Ninja, Vulkan and SDL3 headers; Ubuntu 24.04:
  `packaging/linux/setup-ubuntu.sh` builds the libraries once). `packaging/linux/build.sh` makes
  the release archive with the Linux launcher.
- **Run from a checkout (Linux)**: `BB_GAME_DIR=/path/to/CUSA03173 ./run.sh`.

## Tests and checks

Brazilian Portuguese can be selected as the game's language without a region-unlock
patch. It uses the Brazilian text and voice files from the player's own dump; see
[Brazilian Portuguese game language](docs/BRAZILIAN_PORTUGUESE.md) for behavior and checks.

- `build.sh --test`, and the GPU library's tests (`ninja -C out/gpu pc-input-test ...`).
- `tools/validate_pipelines.sh`: the engine on Mesa's software Vulkan driver with the validation
  layer, as each GPU family (`BB_VK_HIDE`).
- `bb-shader-lab --check-cache <cache folder>`: what the start's warm-up would build from a player's
  cache, and whether each pipeline's layout matches its shaders.
- `tools/hardware_census.sh <logs>`: the GPUs, drivers and CPUs in players' logs.

## Layout

| Folder | What |
|---|---|
| `start/` | bb-start: prepares and starts the game (Windows) |
| `src/` | the runtime: loader, memory, threads, files, saves, pads, audio |
| `gpu/` | the renderer library: shadPS4's video core and shader recompiler with Lantern's changes (`gpu/shadps4`), the PC menus, input and settings (`gpu/shim`), online (`gpu/bbnet`) |
| `scripts/` | patches, mods, assets and GPU cache preparation (Python, also used on Linux) |
| `patches/` | the game's patches and their checks |
| `tests/` | tests and the shader lab |
| `tools/` | developer tools |
| `packaging/` | the Linux package (`packaging/linux`); `default.nix` and `launcher/` are bbport's AppImage and GTK launcher, kept for NixOS (the Linux release uses `../launcher/LanternLauncher`) |
| `docs/` | design notes; `docs/history` keeps the notes of the projects it started from |

## Credits and licenses

Lantern Engine builds are distributed under the **GNU GPL v3 or later** (`licenses/GPL-3.0.txt`):
they combine GPL-2.0-or-later code (shadPS4, bbport, bloodborne_pc) with the online backend for
The Hunter's Dream from [droogie/bbhost](https://github.com/droogie/bbhost) (GPL-3.0-or-later,
`licenses/BBHOST-PORTS.md`). Copyright in each part stays with its authors.

The built-in Lamp2Lamp patch is the work of Disschorde:
[Lamp2Lamp - Warp Between Lamps](https://www.nexusmods.com/bloodborne/mods/455).

Lantern Engine's own code, the launcher and the built-in game patches are © Awaken, under the
GNU GPL v3 or later with additional terms (GPL-3.0 section 7, see `NOTICE`): anything that uses
or ships them must keep the credit "Lantern Engine by Awaken" in its source, its about or
credits screen and its readme or release notes; changed versions must say they are changed and
must not pass as an official Lantern Engine release; and the name "Lantern Engine" is not
licensed for other projects.

Also in the engine, under their own licenses: [shadPS4](https://github.com/shadps4-emu/shadPS4)'s
video core and shader recompiler (GPL-2.0+), shadNet's client (`licenses/SHADNET-PORT.md`),
[sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (MIT),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT), [Dear ImGui](https://github.com/ocornut/imgui)
(MIT), NVIDIA Streamline (headers; DLSS and Reflex), cpp-httplib (MIT), wepoll (BSD), DejaVu
fonts and Lantern Menu SC (a Noto Sans SC subset, SIL OFL 1.1). The built-in game patches
(`patches/Bloodborne.xml`) are Lantern Engine's own, built from the eboot by `tools/re/game_patches.py`.
Renderer fixes from AwesomeObserver's and deadinside28's bloodborne_pc forks and 0xCydral's FSR 4
Vulkan path keep their GPL-2.0-or-later notices. AMD's FSR 4 DLLs and NVIDIA's DLSS files are not
distributed here.

fromsoftserve contributed the optional lighting effects (GTAO, indirect lighting, contact shadows,
PCSS, improved reflections, POM cavity, volumetric sunlight and lamp fog, point light shadows, shadow
resolution choices, motion blur quality) and the conservative occlusion fix, from their BBFSS fork.
