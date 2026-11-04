#!/usr/bin/env python3
"""The engine's own features for run.sh (the Linux start), as bb-start.exe prepares them on Windows
(start/main.cpp; run_windows.py before it): the executable checked, whose identity
(BB_ENGINE_IMAGE_SHA256) every engine hook checks before it goes in - keyboard and mouse, the PC
menus, camera, widescreen, live graphics, cheats, fast loading - then the Dream mirror and the PC
menus made from the player's own files. Without it, from the first Linux build (0.9.37 beta 5) to
beta 8, no Linux start had any of them: the mouse did nothing and key bindings were not read.

Prints its messages as they are and `env:KEY=VALUE` for each variable run.sh exports.

    engine_features.py <game> --out <out> --data <data dir> --config <lantern.ini>
"""
import argparse
import os
from pathlib import Path
import struct
import sys
import zlib

from engine_assets import prepare_effect_fixes, prepare_menu_assets, prepare_mirror, verify_executable


def settings_value(config, key):
    if not config.is_file():
        return None
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def plate_title(config, key):
    title = ''.join(c for c in (settings_value(config, key) or '') if ord(c) >= 0x20 and c != chr(0x7f))
    return title.encode()[:48].decode(errors='ignore')


def prepare(game, out, data, config):
    env = {}
    try:
        env['BB_ENGINE_IMAGE_SHA256'] = verify_executable(out / 'eboot.elf')
        if settings_value(config, 'change_appearance') == '1':
            try:
                asset = prepare_mirror(game, out / 'eboot.elf', data / 'engine-assets')
                env['BB_DREAM_MIRROR_ASSET'] = str(asset)
                print(f'Engine: Dream mirror prepared locally: {asset}', flush=True)
            except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
                print(f'Engine: Dream mirror skipped: {error}', flush=True)
        if settings_value(config, 'ui_4k') != '0':
            ui = data / 'engine-assets' / 'ui-4k'
            if (ui / 'complete').is_file():
                env['BB_UI_4K'] = str(ui)
                print(f'Engine: 4K menu textures from {ui}', flush=True)
        if (any(settings_value(config, key) == '1' for key in ('change_appearance', 'camera_controls', 'rebirth', 'graphics_controls'))
                or any(settings_value(config, key) != '0' for key in ('pc_controls', 'widescreen'))):
            tool = next((out / name for name in ('bb-engine-assets', 'bb-engine-assets.exe') if (out / name).is_file()),
                        out / 'bb-engine-assets')
            try:
                rebirth = settings_value(config, 'rebirth') == '1'
                menus = prepare_menu_assets(game, out / 'eboot.elf', data / 'engine-assets', tool, rebirth=rebirth,
                                            host_title=plate_title(config, 'plate_host_title'),
                                            guest_title=plate_title(config, 'plate_guest_title'))
                env['BB_PC_MENU_ASSETS'] = str(menus)
                print(f'Engine: native PC menus prepared locally: {menus}', flush=True)
                if rebirth:
                    env['BB_REBIRTH_ASSET'] = str(menus / 'dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx')
            except (OSError, ValueError) as error:
                print(f'Engine: native PC menus disabled: {error}', flush=True)
        if settings_value(config, 'widescreen') != '0':
            try:
                from patches import output_size, read_settings, ui_size
                from engine_assets import prepare_wide_movies
                settings = read_settings(config)
                out_res = output_size(settings)
                ui = ui_size(settings, out_res)
                if ui[0] * 1080 != ui[1] * 1920:
                    env['BB_UI_SIZE'] = f'{ui[0]}x{ui[1]}'
                    print(f"Widescreen: UI viewport {env['BB_UI_SIZE']} (the 16:9 stage centred in it)", flush=True)
                    option_movie = None
                    if env.get('BB_PC_MENU_ASSETS'):
                        prepared = Path(env['BB_PC_MENU_ASSETS']) / 'dvdroot_ps4/menu/optionsetting.gfx'
                        option_movie = prepared if prepared.is_file() else None
                    tool = next((out / name for name in ('bb-engine-assets', 'bb-engine-assets.exe') if (out / name).is_file()),
                                out / 'bb-engine-assets')
                    movies = prepare_wide_movies(game, option_movie, (data / 'engine-assets').resolve(),
                                                 tool.resolve(), ui[0], ui[1])
                    listing = ''
                    for relative, path, _ in movies:
                        resolved_path = Path(path).resolve()
                        if option_movie and relative == 'dvdroot_ps4/menu/optionsetting.gfx':
                            env['BB_WIDE_OPTION_MOVIE'] = str(resolved_path)
                        else:
                            listing += f'{relative}\t{resolved_path}\n'
                    if listing:
                        wide_file = (out / 'wide-ui-movies.txt').resolve()
                        wide_file.write_text(listing, encoding='utf-8')
                        env['BB_WIDE_UI_MOVIES'] = str(wide_file)
            except (OSError, ValueError) as error:
                print(f'Widescreen: UI preparation disabled: {error}', flush=True)
    except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
        print(f'Engine: enhancement preparation skipped: {error}', flush=True)
    if os.environ.get('BB_EFFECT_FIXES', '') != '0':
        try:
            fixed = prepare_effect_fixes(game, data / 'engine-assets',
                                         log=lambda line: print(line, flush=True))
            if fixed:
                listing = out / 'file-fixes.txt'
                listing.write_text(''.join(f'{relative.as_posix()}\t{path}\n' for relative, path in fixed),
                                   encoding='utf-8')
                env['BB_FILE_FIXES'] = str(listing)
                print(f"Engine: Ebrietas' trail effect replaced as Nexus mod 41 does, in {len(fixed)} effect files "
                      '(BB_EFFECT_FIXES=0 keeps the game\'s)', flush=True)
        except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
            print(f'Engine: Ebrietas effect fix skipped: {error}', flush=True)
    return env


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder as the engine sees it (mods merged over it)')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    args = parser.parse_args()
    for key, value in prepare(args.game, args.out, args.data, args.config).items():
        print(f'env:{key}={value}', flush=True)
    sys.exit(0)
