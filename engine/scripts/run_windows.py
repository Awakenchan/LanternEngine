#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepare the game image, compile the patches and start
out/bb-probe.exe. Same environment variables and lantern.ini settings as run.sh.

    run.bat [--game-dir DIR] [bb-probe options...]

The game folder: --game-dir, else BB_GAME_DIR, else the last one used (out/game_dir.txt), else
../CUSA03173 (as run.sh). Unless BB_PREBUILT=1
the port is (re)built first through MSYS2 (build.sh in the CLANG64 environment)."""
import atexit
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import struct
import time
import zlib
from mods import remove_overlay

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / 'scripts'
PACKAGE = ROOT.parent if ROOT.name.lower() == 'engine' and (ROOT.parent / 'package.json').is_file() else ROOT
PYTHON = sys.executable
LOG_STREAM = None


def init_session_log():
    """Route launcher/runtime diagnostics to BB_LOG_FILE when the GUI launcher supplies one."""
    global LOG_STREAM
    path = os.environ.get('BB_LOG_FILE')
    if not path:
        return
    log_path = Path(path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    LOG_STREAM = log_path.open('a', encoding='utf-8', errors='backslashreplace', buffering=1)
    atexit.register(LOG_STREAM.close)
    sys.stdout = LOG_STREAM
    sys.stderr = LOG_STREAM
    print('=' * 80)
    print(f'Bloodborne PC session log started: {datetime.now().isoformat(timespec="seconds")}')
    print(f'Launcher root: {ROOT}')
    print(f'Log file: {log_path.resolve()}')
    print('=' * 80, flush=True)


def msys_root():
    return Path(os.environ.get('BB_MSYS2', r'C:\msys64'))


def display_size():
    """The primary display's resolution in pixels (output_res = auto), or None."""
    try:
        import ctypes
        user32 = ctypes.windll.user32
        user32.SetProcessDPIAware()
        w, h = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
        return (w - w % 2, h - h % 2) if w >= 640 and h >= 360 else None
    except (AttributeError, OSError):
        return None


def large_scene(render):
    """Whether a scene of `render` ('WxH') needs the 9152 MiB pool: above 1920x1080."""
    w, h = (int(v) for v in render.split('x'))
    return w * h > 1920 * 1080


def run(arguments, capture=False, check=True, env=None):
    result = subprocess.run(
        [str(a) for a in arguments],
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE if capture else LOG_STREAM,
        stderr=LOG_STREAM,
        text=True,
    )
    if capture and LOG_STREAM is not None and result.stdout:
        LOG_STREAM.write(result.stdout)
        if not result.stdout.endswith('\n'):
            LOG_STREAM.write('\n')
        LOG_STREAM.flush()
    if check and result.returncode:
        print(f'Command failed with exit code {result.returncode}: '
              f'{" ".join(shlex.quote(str(a)) for a in arguments)}', flush=True)
        sys.exit(result.returncode)
    return result.stdout.strip() if capture else result.returncode


def build():
    """build.sh in a CLANG64 login shell (its clang, cmake, ninja and pkg-config)."""
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        sys.exit(f'MSYS2 not found at {msys_root()} (set BB_MSYS2, or BB_PREBUILT=1 with a built out/)')
    env = dict(os.environ, MSYSTEM='CLANG64', CHERE_INVOKING='1')
    run([bash, '-lc', 'bash build.sh'], env=env)


PREPARE_SCRIPTS = ('prepare.py', 'link_libc.py', 'link_modules.py', 'content_profile.py', 'game_check.py')
PREPARED = ('boot.bin', 'eboot.elf', 'entry.bin', 'analysis.json', 'boot-libc.bin', 'libc-link.json',
            'boot-linked.bin', 'link.json', 'content.bin', 'content-profile.json')


def prepare_key(game, sku):
    digest = hashlib.sha256()
    modules = sorted((game / 'sce_module').iterdir()) if (game / 'sce_module').is_dir() else []
    for path in [game / 'eboot.bin', game / 'sce_sys/param.sfo', *modules, *(SCRIPTS / s for s in PREPARE_SCRIPTS),
                 ROOT / 'patches' / 'redzone.json', ROOT / 'patches' / 'redzone_functions.json']:
        digest.update(path.name.encode('utf-8') + b'\0')
        if path.is_file():
            with path.open('rb') as f:
                while chunk := f.read(1 << 20):
                    digest.update(chunk)
        digest.update(b'\0')
    digest.update(f'sku={sku};check={os.environ.get("BB_SKIP_GAME_CHECK", "")};'
                  f'redzone_functions={os.environ.get("BB_RED_ZONE_FUNCTIONS", "")}'.encode('utf-8'))
    return digest.hexdigest()


def prepare_image(game, out):
    """The boot image for `game` in `out`: reused when unchanged, else made by the scripts."""
    started = time.monotonic()
    sku = os.environ.get('BB_CONTENT_SKU', 'full')
    stamp = out / 'prepared.json'
    key = prepare_key(game, sku)
    try:
        reusable = json.loads(stamp.read_text(encoding='utf-8')).get('key') == key
    except (OSError, ValueError, AttributeError):
        reusable = False
    if reusable and all((out / name).is_file() for name in PREPARED):
        print(f'Boot image unchanged: reused ({time.monotonic() - started:.1f} s)', flush=True)
        return
    stamp.unlink(missing_ok=True)
    for script, extra in (('prepare.py', []), ('link_libc.py', []), ('link_modules.py', []),
                          ('content_profile.py', ['--sku', sku])):
        run([PYTHON, SCRIPTS / script, game, '--out', out, *extra])
    stamp.write_text(json.dumps({'key': key}) + '\n', encoding='utf-8')
    print(f'Boot image prepared in {time.monotonic() - started:.1f} s (reused on later launches)', flush=True)


def settings_value(config, key):
    if not config.is_file():
        return None
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def gpu_cache_dir(env=os.environ):
    """Where the engine keeps its shader and pipeline caches (bb-probe runs in ROOT)."""
    return Path(env.get('BB_GPU_USER_DIR', PACKAGE / 'user')) / 'cache'


def gpu_cache_id(out, engine):
    """The GPU cache's identity: out/gpu-cache.id (engine/build.sh), else the engine id."""
    try:
        value = (out / 'gpu-cache.id').read_text(encoding='utf-8').strip()
        return f'gpu {value}' if value else engine
    except OSError:
        return engine


def reset_stale_gpu_cache(cache, engine):
    """Deletes the shader and pipeline caches another engine build wrote. The engine checks them
    against the GPU only, so a damaged record or one from an older build was preloaded every
    session, through updates (characters' skin and hair missing in every save: GitHub issue 30).
    `engine` is the GPU cache's identity (gpu_cache_id): a hash of the sources that decide what
    the caches hold, so an update that leaves the shader recompiler and the pipeline records alone
    keeps every shader the player's PC compiled (no stutter in each area again after updates).
    The GPU driver's own cache (*.vkcache) stays: the engine checks it against the GPU and driver,
    and it holds only what the driver compiled, so the next start compiles less."""
    stamp = cache / 'engine.txt'
    try:
        if not engine or stamp.read_text(encoding='utf-8').strip() == engine:
            return False
    except OSError:
        pass
    removed = False
    if cache.is_dir():
        for entry in cache.iterdir():
            if entry == stamp or entry.suffix == '.vkcache':
                continue
            try:
                if entry.is_dir() and not entry.is_symlink():
                    shutil.rmtree(entry)
                else:
                    entry.unlink()
                removed = True
            except OSError as error:
                print(f'GPU cache: cannot remove {entry.name}: {error}', flush=True)
                return False
    cache.mkdir(parents=True, exist_ok=True)
    stamp.write_text(engine + '\n', encoding='utf-8')
    if removed:
        print(f'GPU cache: cleared ({engine}: the shader compiler changed); shaders compile again on this start', flush=True)
    return removed


def configure_frame_rate(env):
    """Windows supports the tested fixed-timestep presets, including old config migration.

    Explicit limits also override inherited BB_VBLANK_HZ=0 / BB_FPS_LIMIT=0 so a
    legacy uncapped launch cannot saturate the desktop during menus or loading.
    """
    fps = env.get('BB_FPS', '60')
    if fps in ('120', '144'):
        env.update(BB_FPS=fps, BB_FPS_LIMIT=fps, BB_VBLANK_HZ=fps)
        return fps
    if fps == 'uncap' or (fps.isdigit() and int(fps) > 90):
        limit = fps if fps.isdigit() else env.get('BB_FPS_LIMIT', '0')
        env.update(BB_FPS='uncap', BB_FPS_LIMIT=limit if limit.isdigit() else '0', BB_VBLANK_HZ='0')
        return 'uncap'
    if fps not in ('30', '60', '90'):
        print(f'Unsupported Windows FPS preset {fps!r}; using 60 FPS.', flush=True)
        fps = '60'
    env.update(BB_FPS=fps, BB_FPS_LIMIT=fps, BB_VBLANK_HZ='90' if fps == '90' else '60')
    return fps


def main():
    arguments = sys.argv[1:]
    game = os.environ.get('BB_GAME_DIR')
    if arguments[:1] == ['--game-dir'] and len(arguments) > 1:
        game, arguments = arguments[1], arguments[2:]
    data = Path(os.environ.get('BB_DATA_DIR', PACKAGE))
    out = data / 'out' if (data / 'out' / 'bb-probe.exe').is_file() else ROOT / 'out'
    try:
        engine = json.loads((PACKAGE / 'package.json').read_text(encoding='utf-8-sig')).get('engine', '')
        reset_stale_gpu_cache(gpu_cache_dir(), gpu_cache_id(out, engine))
    except (OSError, ValueError):
        pass
    out.mkdir(parents=True, exist_ok=True)
    if not (data / 'lantern.ini').exists() and (data / 'bbport.ini').exists():
        try:
            shutil.copy2(data / 'bbport.ini', data / 'lantern.ini')
        except OSError:
            pass
    os.environ.setdefault('BB_CONFIG', str(data / 'lantern.ini'))
    config = Path(os.environ['BB_CONFIG'])
    from online import configure_online, read_settings
    try:
        override = configure_online(read_settings(config) if config.exists() else {}, data, os.environ)
        print('Online: configured server profile and automatic game redirect' if override else 'Online: offline mode')
    except (OSError, ValueError) as error:
        sys.exit(f'Online configuration: {error}')
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    if 'BB_FSR4VK_DIR' not in os.environ and (ROOT / 'fsr4vk' / 'amd_fidelityfx_upscaler_vk.dll').is_file():
        os.environ['BB_FSR4VK_DIR'] = str(ROOT / 'fsr4vk')
    if 'BB_FSR_SDK_DIR' not in os.environ and (ROOT / 'fsr_sdk' / 'amd_fidelityfx_loader_vk.dll').is_file():
        os.environ['BB_FSR_SDK_DIR'] = str(ROOT / 'fsr_sdk')
    remembered = out / 'game_dir.txt'
    if not game and remembered.is_file():
        game = remembered.read_text(encoding='utf-8').strip()
    game = Path(game) if game else PACKAGE.parent / 'CUSA03173'
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (pass --game-dir or set BB_GAME_DIR).')
    original = game.resolve()
    remembered.write_text(str(original), encoding='utf-8')
    os.environ['BB_GAME_DIR'] = str(original)
    os.environ['BB_RESTART_COMMAND'] = subprocess.list2cmdline([PYTHON, str(Path(__file__).resolve()), *sys.argv[1:]])

    first_layers = []
    try:
        chosen = json.loads(Path(os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        chosen = {}
    lamp2lamp_bosses = 'Bloodborne.xml/Lamp2Lamp: warp to bosses' in chosen.get('enabled', [])
    if lamp2lamp_bosses or 'Bloodborne.xml/Lamp2Lamp (warp between lamps)' in chosen.get('enabled', []):
        try:
            import engine_assets, mods
            for name in ('lamp2lamp', 'lamp2lamp-bosses') if lamp2lamp_bosses else ('lamp2lamp',):
                layer = engine_assets.prepare_lamp2lamp(original, mods.update_folder(original), PACKAGE / 'engine' / f'{name}.bin',
                                                        data / 'engine-assets')
                first_layers += ['--first-layer', layer]
            print('Engine: Lamp2Lamp on', flush=True)
        except (OSError, RuntimeError, ValueError) as error:
            print(f'Engine: Lamp2Lamp off: {error}', flush=True)
    merged = Path(run([PYTHON, SCRIPTS / 'mods.py', original, '--out', out, *first_layers,
                       '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                       '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                       '--enabled', os.environ.get('BB_MODS_ENABLED', '1')], capture=True))
    overlay = merged if merged.resolve() != original else None
    core = ('dvdroot_ps4/shader/gxshader.shaderbnd.dcx', 'dvdroot_ps4/other/systex.tpf.dcx')
    missing = [name for name in core if not (merged / name).is_file()]
    if missing and overlay and all((original / name).is_file() for name in core):
        print('Mods: the merged game lost its own files; starting without mods', flush=True)
        remove_overlay(overlay)
        merged, overlay, missing = original, None, []
    if missing:
        if overlay:
            remove_overlay(overlay)
        sys.exit(f'The game folder {original} is incomplete: {", ".join(missing)} missing. '
                 'Dump or extract the game again (base game plus the 1.09 update).')
    try:
        prepare_image(merged, out)
        prebuilt = os.environ.get('BB_PREBUILT') == '1'
        if not prebuilt:
            build()
        scanner = out / 'bb-isa-scan.exe'
        if not scanner.is_file():
            sys.exit(f'ISA scanner not found: {scanner}. Rebuild the engine or reinstall its updated package.')
        scan_env = dict(os.environ)
        scan_env['PATH'] = os.pathsep.join([str(msys_root() / 'clang64/bin'), scan_env.get('PATH', '')])
        scan_output = run([scanner, out / 'boot-linked.bin', out], capture=True, env=scan_env)
        if scan_output:
            print(scan_output, flush=True)
        for key in ('BB_DREAM_MIRROR_ASSET', 'BB_ENGINE_IMAGE_SHA256', 'BB_PC_MENU_ASSETS', 'BB_REBIRTH_ASSET'):
            os.environ.pop(key, None)
        try:
            from engine_assets import prepare_mirror, verify_executable
            os.environ['BB_ENGINE_IMAGE_SHA256'] = verify_executable(out / 'eboot.elf')
            if settings_value(config, 'change_appearance') == '1':
                try:
                    asset = prepare_mirror(merged, out / 'eboot.elf', data / 'engine-assets')
                    os.environ['BB_DREAM_MIRROR_ASSET'] = str(asset)
                    print(f'Engine: Dream mirror prepared locally: {asset}', flush=True)
                except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
                    print(f'Engine: Dream mirror skipped: {error}', flush=True)
            if (any(settings_value(config, key) == '1' for key in ('change_appearance', 'camera_controls', 'rebirth', 'graphics_controls'))
                or any(settings_value(config, key) != '0' for key in ('pc_controls', 'widescreen'))):
                from engine_assets import prepare_menu_assets
                try:
                    menus = prepare_menu_assets(merged, out / 'eboot.elf', data / 'engine-assets',
                                                out / 'bb-engine-assets.exe', rebirth=settings_value(config, 'rebirth') == '1')
                    os.environ['BB_PC_MENU_ASSETS'] = str(menus)
                    print(f'Engine: native PC menus prepared locally: {menus}', flush=True)
                    if settings_value(config, 'rebirth') == '1':
                        os.environ['BB_REBIRTH_ASSET'] = str(menus / 'dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx')
                except (OSError, ValueError) as error:
                    print(f'Engine: native PC menus disabled: {error}', flush=True)
        except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
            print(f'Engine: enhancement preparation skipped: {error}', flush=True)
        if not os.environ.get('BB_DISPLAY_SIZE'):
            size = display_size()
            if size:
                os.environ['BB_DISPLAY_SIZE'] = f'{size[0]}x{size[1]}'
                if (settings_value(config, 'output_res') or '').lower() == 'auto':
                    print(f'Output resolution: the display\'s own, {size[0]}x{size[1]}', flush=True)
        if os.environ.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                os.environ.pop(key, None)
        fps = configure_frame_rate(os.environ)
        scaled_render = scaled_output = None
        if not os.environ.get('BB_RENDER_RES'):
            sizes = run([PYTHON, SCRIPTS / 'patches.py', '--print-scaled', '--settings', config], capture=True, check=False)
            if sizes and len(sizes.split()) == 2:
                scaled_render, scaled_output = sizes.split()
        live = '0'
        if scaled_output:
            live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution') or '0'
            if live == 'auto':
                caps = out / 'bb-gpu-capabilities.exe'
                live = run([caps, '--live-resolution'], capture=True, check=False) or '0'
            live = '1' if live == '1' else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            os.environ.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            if large_scene(scaled_render):
                os.environ.setdefault('BB_DMEM_MB', '9152')
            dmem = os.environ.get('BB_DMEM_MB', '5056')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {dmem} MiB '
                  '(live_resolution=1: live changes)')
        run([PYTHON, SCRIPTS / 'patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
             '--settings', config, '--game-dir', merged, '--render-res', os.environ.get('BB_RENDER_RES', ''),
             '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
             '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
             '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')])
        try:
            info = json.loads((out / 'patches-info.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            info = {}
        heaps = info.get('heap_patch')
        if info.get('ui_size', '1920x1080') != '1920x1080':
            os.environ['BB_UI_SIZE'] = info['ui_size']
            print(f"Widescreen: UI viewport {info['ui_size']} (the 16:9 stage centred in it)", flush=True)
        else:
            os.environ.pop('BB_UI_SIZE', None)
        # Widescreen on a screen of another shape: widened copies of the menu movies mounted over
        # the game's (BB_WIDE_UI_MOVIES) and the PC menus' optionsetting.gfx (BB_WIDE_OPTION_MOVIE),
        # as bb-start.exe does (start/main.cpp).
        for key in ('BB_WIDE_UI_MOVIES', 'BB_WIDE_OPTION_MOVIE'):
            os.environ.pop(key, None)
        # Only a UI viewport of another shape than 16:9, as bb-start.exe has it.
        ui = [int(v) for v in os.environ.get('BB_UI_SIZE', '1920x1080').split('x')]
        wide_ui = ui[0] * 1080 != ui[1] * 1920
        if wide_ui:
            from engine_assets import prepare_wide_movies
            try:
                width, height = (int(v) for v in os.environ['BB_UI_SIZE'].split('x'))
                option_movie = None
                if os.environ.get('BB_PC_MENU_ASSETS'):
                    prepared = Path(os.environ['BB_PC_MENU_ASSETS']) / 'dvdroot_ps4/menu/optionsetting.gfx'
                    option_movie = prepared if prepared.is_file() else None
                movies = prepare_wide_movies(merged, option_movie, data / 'engine-assets',
                                             out / 'bb-engine-assets.exe', width, height)
                listing = ''
                for relative, path, _ in movies:
                    if option_movie and relative == 'dvdroot_ps4/menu/optionsetting.gfx':
                        os.environ['BB_WIDE_OPTION_MOVIE'] = str(path)
                    else:
                        listing += f'{relative}\t{path}\n'
                if listing:
                    (out / 'wide-ui-movies.txt').write_text(listing, encoding='utf-8')
                    os.environ['BB_WIDE_UI_MOVIES'] = str(out / 'wide-ui-movies.txt')
                print(f"Widescreen: {len(movies)} menu movies' full-screen dims, fades and bands widened for "
                      f"{os.environ['BB_UI_SIZE']}", flush=True)
            except (OSError, ValueError) as error:
                print(f"Widescreen: the menus' full-screen dims and fades stay 16:9: {error}", flush=True)
        if heaps and 'BB_DMEM_MB' not in os.environ:
            os.environ['BB_DMEM_MB'] = '9152'
            print("Patches: the game's heaps are enlarged; direct memory 9152 MiB", flush=True)
        probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
        command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
                   '--patches', out / 'patches.bin', '--app0', merged,
                   '--user', os.environ.get('BB_USER_DIR', data / 'user'),
                   '--timeout', os.environ.get('BB_TIMEOUT', '0'), *arguments]
        os.environ['PATH'] = os.pathsep.join([str(msys_root() / 'clang64/bin'), os.environ.get('PATH', '')])
        print('Starting:', ' '.join(shlex.quote(str(c)) for c in command), flush=True)
        try:
            status = subprocess.call(
                [str(c) for c in command],
                cwd=ROOT,
                stdout=LOG_STREAM,
                stderr=LOG_STREAM,
            )
        except KeyboardInterrupt:
            status = 130
        print(f'bb-probe exited with code {status}', flush=True)
        return status - (1 << 32) if status >= (1 << 31) else status
    finally:
        if overlay:
            remove_overlay(overlay)


if __name__ == '__main__':
    init_session_log()
    sys.exit(main())
