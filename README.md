# Lantern Engine
<a href="https://trendshift.io/repositories/302591?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-302591" target="_blank" rel="noopener noreferrer"><img src="https://trendshift.io/api/badge/trendshift/repositories/302591/daily?language=C%23" alt="Awakenchan%2FBloodBorneButPc | Trendshift" width="250" height="55"/></a>
<a href="https://trendshift.io/repositories/302591?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-302591" target="_blank" rel="noopener noreferrer"><img src="https://trendshift.io/api/badge/trendshift/repositories/302591/weekly?language=C%23" alt="Awakenchan%2FLanternEngine | Trendshift" width="250" height="55"/></a>

<a href="https://recomplauncher.com/game/lanternengine"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

Lantern Engine runs your own copy of Bloodborne on Windows and Linux: a launcher and my engine, built on bbport and shadPS4 and reworked by me for performance. The game's own code runs natively on your CPU; its graphics are translated to Vulkan.

**No game files are included.** You need your own legal copy of Bloodborne (CUSA03173) with update 1.09, dumped from your own PS4.

**Help:** [Help & FAQ](https://awakenchan.github.io/LanternEngine/): setup, a graphics settings guide, fixes for common problems and the beta guide. Bug reports go to the `#issues` forum on the [Discord](https://discord.gg/d3Uv3Cfr5x).

## Install

1. Download `LanternEngine-<version>-win64.zip` from [Releases](https://github.com/Awakenchan/LanternEngine/releases) and extract it anywhere.
2. Run `Lantern Engine.exe` and pick your game folder (the one with `eboot.bin`).
3. Press **Play**. The launcher updates itself. `Bloodborne.exe` starts the game directly (add it to Steam as a non-Steam game).

### Linux (beta)

1. Download `LanternEngine-<version>-linux-x64.tar.gz` from [Releases](https://github.com/Awakenchan/LanternEngine/releases) and unpack it (`tar xzf LanternEngine-*-linux-x64.tar.gz`).
2. Run `Lantern Engine` in the unpacked folder, pick your game folder and press **Play**. From a terminal: `./lantern.sh /path/to/CUSA03173`.
3. It needs a Vulkan driver (Mesa or NVIDIA), Python 3 and glibc 2.38 or newer (Ubuntu 24.04, Fedora 39, Arch, SteamOS 3.6, Bazzite). From 0.9.37 beta 7 the launcher updates itself on Linux too, and DLSS Super Resolution works on RTX cards with NVIDIA's driver (DLSS Frame Generation is Windows only).

## Sponsors

Seamless co-op runs on a server in Frankfurt, sponsored by [1337 Hosting](https://1337hosting.com). Thank you!

## Credits

| | |
|---|---|
| **Lantern Engine** | by **Awaken**: the launcher, the built-in game patches, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, cheats, occlusion culling, trophies, fixes) |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) (built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| Renderer crash fixes | adapted from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc) |
| Online backend for The Hunter's Dream | [droogie/bbhost](https://github.com/droogie/bbhost) |
| FSR (FSR 4.1.2 and FSR 3.1.6) | [AMD FSR SDK 2.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), AMD's signed Vulkan binaries |
| Seamless co-op server | [1337 Hosting](https://1337hosting.com) |
| FSR 3.1 and FSR 4 on Vulkan | [FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK |
| FSR 4.1.1 | [fsr4vk](https://github.com/dvj5411/fsr4vk), with the AMD driver fix from [0xCydral](https://github.com/0xCydral/bloodborne_pc) |
| DLSS | NVIDIA DLSS SDK |
| XeSS Super Resolution | [Intel XeSS SDK 3.0.2](https://github.com/intel/xess), Intel's `libxess.dll` (downloaded by `engine/tools/fetch_xess.sh`) |
| Sound | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) |
| In-game menu | [Dear ImGui](https://github.com/ocornut/imgui) |
| 4K menus | [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) with [ncnn](https://github.com/Tencent/ncnn), downloaded by the launcher and run on the player's own game files |
| Controller mappings | [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) |
| UI library | [WPF-UI](https://github.com/lepoco/wpfui) |

## License

Lantern Engine's own code, the launcher and the built-in game patches are GPL-3.0-or-later with additional terms (see [LICENSE](LICENSE) and [NOTICE](NOTICE)): anything that uses or ships them must keep the credit "Lantern Engine by Awaken", changed versions must say they are changed and must not pass as an official Lantern Engine release, and the name "Lantern Engine" is not licensed for other projects. Parts by others keep their own licenses, listed in the engine's `licenses/` folder.

Source: the engine's full source is in [`engine/`](engine) (0.9.37 beta 7 on), the launchers' in [`launcher/windows/`](launcher/windows), and the Help & FAQ page in [`faq/`](faq).
