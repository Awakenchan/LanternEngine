#!/usr/bin/env python3
"""Widescreen's UI viewport for run.sh (the Linux start), as bb-start.exe sets it on Windows
(start/main.cpp; run_windows.py before it): the UI size patches.py chose (out/patches-info.json)
as BB_UI_SIZE, so the presenter composes the game's wider UI (2520x1080 at 21:9, 2560x1080 for a
2560x1080 output) over the scene instead of a 1920x1080 one, and on a screen of another shape than
16:9 the menu movies with their full-screen dims, fades and bands widened (BB_WIDE_UI_MOVIES,
BB_WIDE_OPTION_MOVIE). Without it a
Linux start with an ultrawide output drew the title and menus shifted right and cut off.

Prints its messages as they are and `env:KEY=VALUE` for each variable run.sh exports, BB_UI_SIZE
first: the patches already drew the UI at that size, so the movies failing must not lose it.

    wide_ui.py <game> --out <out> --data <data dir> [--tool <bb-engine-assets>]
"""
import argparse
import json
import os
from pathlib import Path
import sys

from engine_assets import prepare_wide_movies


def emit(key, value):
    print(f'env:{key}={value}', flush=True)


def prepare(game, out, data, tool=None):
    try:
        info = json.loads((out / 'patches-info.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        info = {}
    size = info.get('ui_size', '1920x1080')
    if size == '1920x1080':
        return
    emit('BB_UI_SIZE', size)
    print(f'Widescreen: UI viewport {size} (the 16:9 stage centred in it)', flush=True)
    try:
        width, height = (int(v) for v in size.split('x'))
    except ValueError:
        return
    if width * 1080 == height * 1920:
        return
    # run.sh passes the tool beside the engine's programs (bin/ in packages, out/ in a build); out
    # is where bb-start keeps them too.
    if not (tool and tool.is_file()):
        tool = next((out / name for name in ('bb-engine-assets', 'bb-engine-assets.exe') if (out / name).is_file()),
                    tool or out / 'bb-engine-assets')
    option_movie = None
    if os.environ.get('BB_PC_MENU_ASSETS'):
        prepared = Path(os.environ['BB_PC_MENU_ASSETS']) / 'dvdroot_ps4/menu/optionsetting.gfx'
        option_movie = prepared if prepared.is_file() else None
    try:
        movies = prepare_wide_movies(game, option_movie, data / 'engine-assets', tool, width, height)
        listing, names = '', []
        for relative, path, note in movies:
            if option_movie and relative == 'dvdroot_ps4/menu/optionsetting.gfx':
                emit('BB_WIDE_OPTION_MOVIE', path)
            else:
                listing += f'{relative}\t{path}\n'
            names.append(Path(relative).stem)
            if os.environ.get('BB_UI_VIEWPORT_LOG') == '1':
                print(f'Widescreen: {relative}: {note}', flush=True)
        if listing:
            (out / 'wide-ui-movies.txt').write_text(listing, encoding='utf-8')
            emit('BB_WIDE_UI_MOVIES', out / 'wide-ui-movies.txt')
        print(f"Widescreen: {len(movies)} menu movies' full-screen dims, fades and bands widened for {size}: "
              f"{', '.join(names)}", flush=True)
    except Exception as error:  # a bad cache or tool output: the menus stay 16:9, the viewport stays
        print(f"Widescreen: the menus' full-screen dims and fades stay 16:9: {error}", flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder as the engine sees it (mods merged over it)')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--tool', type=Path, help='bb-engine-assets (default: the one in --out)')
    args = parser.parse_args()
    prepare(args.game, args.out, args.data, args.tool)
    sys.exit(0)
