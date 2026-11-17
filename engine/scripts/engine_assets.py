# SPDX-License-Identifier: GPL-2.0-or-later
"""Local, version-gated asset preparation for the engine features: the Dream mirror's layout, the
effect fixes and the native menu assets, from the player's own dump. Game files stay local.
"""
import hashlib
import shutil
import os
import json
import subprocess
from pathlib import Path
import struct
import zlib

LAYOUT = Path('dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx')
INPUT_SHA = '8e1de0ee223cd5137a93383cfc7bb1d27964bc480c63e35013674717b403ef8f'
OUTPUT_SHA = '99d5c7b7f52307344c6e8296b499d5e57969f21c443fcc607cb42cb2c38c8afa'
EBOOT_SHA = {'941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7',
             'cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86'}
IMAGE_SHA = '071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a'
HOOKS = ((0x1c1cce0, bytes.fromhex('554889e54157415653504d89ce4889fb')),
         (0x2034770, bytes.fromhex('554889e54157415641554154534883ec38')))


def sha(data):
    return hashlib.sha256(data).hexdigest()


DCX_HEADER = bytes.fromhex(
    '44435800000100000000001800000024000000440000004c'
    '4443530000000000000000004443500044464c5400000020'
    '09000000000000000000000000000000000001014443410000000008')
DCX_LARGEST = 32 << 20


def dcx_unpack(data):
    """The file inside a DCX container; anything else is returned as it is."""
    if data[:4] != b'DCX\0':
        return data
    if len(data) < 0x4c or data[0x28:0x2c] != b'DFLT':
        raise ValueError('Unsupported DCX format')
    dca = data.find(b'DCA\0', 0x2c)
    if dca < 0 or dca + 8 > len(data):
        raise ValueError('DCX without a valid DCA header')
    begin = dca + struct.unpack_from('>I', data, dca + 4)[0]
    size = struct.unpack_from('>I', data, 0x1c)[0]
    if not dca + 8 <= begin < len(data) or not 0 < size <= DCX_LARGEST:
        raise ValueError('Invalid DCX header')
    inflater = zlib.decompressobj()
    raw = inflater.decompress(data[begin:], size + 1)
    if len(raw) != size or not inflater.eof or inflater.unused_data:
        raise ValueError('Invalid DCX compressed data or size')
    return raw


def dcx_pack(raw):
    packed = zlib.compress(raw, 9)
    header = bytearray(DCX_HEADER)
    struct.pack_into('>II', header, 0x1c, len(raw), len(packed))
    return bytes(header) + packed


MIRROR_PART, MIRROR_SLOT, FIRST_ENEMY = 807, 742, 736
KIND_ENEMY, KIND_DUMMY_ENEMY = 2, 10
EVENT_PART_FIELDS = (0x48, 0x50, 0x5c)
PART_PART_FIELDS = (0x128, 0x13c, 0x144, 0x178, 0x180, 0x188)


def mirror_layout(raw):
    """The Dream's layout with the mirror stand-in as an enemy; only from the verified 1.09 file."""
    if sha(raw) != INPUT_SHA:
        raise ValueError('Dream mirror requires the original 1.09 Dream layout')
    b = bytearray(raw)
    tables, at = [], 0x10
    for n in range(4):
        if not 0 <= at <= len(b) - 16:
            raise ValueError('Invalid MSB list header')
        count = struct.unpack_from('<i', b, at + 4)[0]
        if not 1 <= count <= (len(b) - at - 16) // 8:
            raise ValueError('Invalid MSB list size')
        *entries, following = struct.unpack_from(f'<{count}q', b, at + 16)
        if any(not 0 < e < len(b) for e in entries) or (n < 3 and not 0 < following < len(b)):
            raise ValueError('Invalid MSB entry')
        tables.append((at + 16, list(entries)))
        at = following
    events = tables[1][1]
    parts_table, parts = tables[3]
    if len(parts) <= MIRROR_PART + 1 or not parts[MIRROR_SLOT] < parts[MIRROR_PART] < parts[MIRROR_PART + 1]:
        raise ValueError('Dream layout parts are not as expected')
    if struct.unpack_from('<i', b, parts[MIRROR_PART] + 0x14)[0] != KIND_DUMMY_ENEMY:
        raise ValueError('Mirror stand-in is not a DummyEnemy')

    def moved(index):
        if index == MIRROR_PART:
            return MIRROR_SLOT
        return index + 1 if MIRROR_SLOT <= index < MIRROR_PART else index

    boundaries = sorted(e for _, entries in tables for e in entries) + [len(b)]
    for entries, fields in ((events, EVENT_PART_FIELDS), (parts, PART_PART_FIELDS)):
        for entry in entries:
            end = next(x for x in boundaries if x > entry)
            for field in fields:
                if entry + field + 4 <= end:
                    index = struct.unpack_from('<i', b, entry + field)[0]
                    if index >= 0 and moved(index) != index:
                        struct.pack_into('<i', b, entry + field, moved(index))
    for index, entry in enumerate(parts):
        kind, number = struct.unpack_from('<ii', b, entry + 0x14)
        if kind == KIND_ENEMY and index >= MIRROR_SLOT:
            struct.pack_into('<i', b, entry + 0x18, number + 1)
        elif kind == KIND_DUMMY_ENEMY and index > MIRROR_PART:
            struct.pack_into('<i', b, entry + 0x18, number - 1)
    struct.pack_into('<ii', b, parts[MIRROR_PART] + 0x14, KIND_ENEMY, MIRROR_SLOT - FIRST_ENEMY)
    first, stand_in, after = parts[MIRROR_SLOT], parts[MIRROR_PART], parts[MIRROR_PART + 1]
    b[first:after] = b[stand_in:after] + b[first:stand_in]
    size = after - stand_in
    struct.pack_into('<q', b, parts_table + 8 * MIRROR_SLOT, first)
    for index in range(MIRROR_SLOT, MIRROR_PART):
        struct.pack_into('<q', b, parts_table + 8 * (index + 1), parts[index] + size)
    if sha(b) != OUTPUT_SHA:
        raise ValueError('Mirror layout does not match the verified result')
    return bytes(b)


def verify_executable(elf_path):
    elf = Path(elf_path).read_bytes()
    if sha(elf) not in EBOOT_SHA:
        raise ValueError('Mirror hooks require the verified 1.09 executable')
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    entsize, count = struct.unpack_from('<HH', elf, 54)
    if entsize != 56 or not 0 < count < 256:
        raise ValueError('Invalid executable program headers')
    segments = [struct.unpack_from('<IIQQQQQQ', elf, phoff + i * 56) for i in range(count)]
    for address, expected in HOOKS:
        found = False
        for kind, flags, offset, va, _, size, _, _ in segments:
            if kind == 1 and flags & 1 and va <= address and address + len(expected) <= va + size:
                found = elf[offset + address - va:offset + address - va + len(expected)] == expected
                break
        if not found:
            raise ValueError(f'Unexpected 1.09 engine bytes at {address:#x}')
    return IMAGE_SHA


def prepare_mirror(game, elf_path, cache):
    verify_executable(elf_path)
    raw = dcx_unpack((Path(game) / LAYOUT).read_bytes())
    rewritten = mirror_layout(raw)
    destination = Path(cache) / 'dream-mirror' / LAYOUT
    try:
        if sha(dcx_unpack(destination.read_bytes())) == OUTPUT_SHA:
            return destination.resolve()
    except (OSError, ValueError, zlib.error):
        pass
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + '.tmp')
    temporary.write_bytes(dcx_pack(rewritten))
    os.replace(temporary, destination)
    return destination.resolve()


SFX = Path('dvdroot_ps4/sfx')
EFFECT_NAME = 'f001251720.fxr'
EFFECT_SHA = '988c55ba0f689bc0b4f8e7a9f9b184ef8cd9bcd4601b2c925387ff5ce3fc9f0b'
EFFECT_SOURCE = (SFX / 'frpg_sfxbnd_m36.ffxbnd.dcx', 'f001454436.fxr',
                 '990644087f325191d34fd2f4b633a9f7c6a63ce9d71786c4289a7cae5cdfb4e0')
EFFECT_FIXES = (
    (SFX / 'frpg_sfxbnd_m24_02.ffxbnd.dcx',
     '6cab7031c5fafba95ed71c4ddb86e590b77b2d669988e8b53bed159e1dbc1015',
     'b0bd083145a753a667c4021a3f817879646b1f8b84201e2774daa17e1c5b0e99',
     'fae6b9639fb02b34188a3dd262b2695f7667812749f984f1eb2fd2ab298a9e9b'),
    (SFX / 'frpg_sfxbnd_m29a.ffxbnd.dcx',
     '0466ebd7125cc705e341cab18e18c3d1282000a8d54b0628d47da992daed0fd6',
     '085f2d3d60d54e50fa981c7aad89ca42f47b3a733a78b40ee98f572883ce122b',
     '8348b2b7f110feb7ca0a444e8331886b85fe24ba422b8b9909cdc5d551ffcb07'),
)
BND4_ENTRY = 0x24


def binder_entries(bnd):
    """(header offset, name, data offset, size) of each file in a BND4 with 0x24-byte entries
    (format 0x74: sizes, 32-bit offsets, ids, UTF-16 names), as the game's sfx binders are."""
    if bnd[:4] != b'BND4' or struct.unpack_from('<q', bnd, 0x20)[0] != BND4_ENTRY or bnd[0x30:0x32] != b'\x01\x74':
        raise ValueError('Unexpected effect binder layout')
    count = struct.unpack_from('<i', bnd, 0x0c)[0]
    entries = []
    for i in range(count):
        at = 0x40 + i * BND4_ENTRY
        packed, unpacked, offset, _, name_at = struct.unpack_from('<qqIii', bnd, at + 8)
        if packed != unpacked or offset + packed > len(bnd):
            raise ValueError('Unexpected effect binder entry')
        end = name_at
        while bnd[end:end + 2] != b'\0\0':
            end += 2
        entries.append((at, bnd[name_at:end].decode('utf-16-le'), offset, packed))
    return entries


def replace_entry(bnd, name, data):
    """The binder with `name` (its file name) holding `data`: appended 16-byte aligned, its
    entry's sizes and offset pointed at it. The other entries and the name table stay put."""
    found = [e for e in binder_entries(bnd) if e[1].replace('\\', '/').rsplit('/', 1)[-1] == name]
    if len(found) != 1:
        raise ValueError(f'{name} is not in the effect binder once')
    at = found[0][0]
    offset = (len(bnd) + 15) & ~15
    out = bytearray(bnd) + bytes(offset - len(bnd)) + data
    struct.pack_into('<qqI', out, at + 8, len(data), len(data), offset)
    return bytes(out)


def binder_file(bnd, name):
    for _, entry, offset, size in binder_entries(bnd):
        if entry.replace('\\', '/').rsplit('/', 1)[-1] == name:
            return bnd[offset:offset + size]
    raise ValueError(f'{name} is not in the effect binder')


def prepare_effect_fixes(game, cache, log=print):
    """The fixed effect binders under cache/effect-fixes, as (game path, file) pairs for the
    runtime (BB_FILE_FIXES); empty when none applies. A prepared binder is kept while it matches
    the hash written beside it (<binder>.sha256, of its .dcx)."""
    game = Path(game)
    root = Path(cache) / 'effect-fixes'
    source, source_name, source_sha = EFFECT_SOURCE
    replacement = None
    fixed = []
    for relative, packed_sha, input_sha, output_sha in EFFECT_FIXES:
        try:
            packed = (game / relative).read_bytes()
        except OSError:
            continue
        raw = None
        if sha(packed) != packed_sha:
            raw = dcx_unpack(packed)
            if sha(raw) != input_sha:
                log(f'Engine: Ebrietas effect fix: {relative.name} is not the 1.09 file (a mod changes it): left as it is')
                continue
        destination = root / relative
        stamp = destination.with_name(destination.name + '.sha256')
        try:
            if stamp.read_text(encoding='ascii').strip() == sha(destination.read_bytes()):
                fixed.append((relative, destination.resolve()))
                continue
        except OSError:
            pass
        raw = raw if raw is not None else dcx_unpack(packed)
        if sha(binder_file(raw, EFFECT_NAME)) != EFFECT_SHA:
            raise ValueError(f'Unexpected {EFFECT_NAME} in {relative.name}')
        if replacement is None:
            replacement = binder_file(dcx_unpack((game / source).read_bytes()), source_name)
            if sha(replacement) != source_sha:
                raise ValueError(f'Unexpected {source_name} in {source.name}')
        result = replace_entry(raw, EFFECT_NAME, replacement)
        if sha(result) != output_sha:
            raise ValueError(f'{relative.name} does not match the verified result')
        out = dcx_pack(result)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + f'.{os.getpid()}.tmp')
        temporary.write_bytes(out)
        os.replace(temporary, destination)
        stamp.write_text(sha(out) + '\n', encoding='ascii')
        fixed.append((relative, destination.resolve()))
    return fixed


def prepare_lamp2lamp(game, update, patch, cache):
    file = Path(patch).read_bytes()
    if file[:4] != b'L2LP':
        raise RuntimeError('not a Lamp2Lamp patch file')
    body = zlib.decompress(file[8:])
    at = 0

    def take(count):
        nonlocal at
        if count > len(body) - at:
            raise RuntimeError('damaged Lamp2Lamp patch file')
        at += count
        return body[at - count:at]

    def u32():
        return struct.unpack('<I', take(4))[0]

    rows = []
    state = sha(file)
    for _ in range(u32()):
        name = take(struct.unpack('<H', take(2))[0]).decode()
        wanted_in, wanted_out = take(32).hex(), take(32).hex()
        u32()
        ops = []
        for _ in range(u32()):
            where, length = u32(), u32()
            ops.append(take(length) if where == 0xFFFFFFFF else (where, length))
        source = Path(update) / name if update and (Path(update) / name).is_file() else Path(game) / name
        if not source.is_file():
            raise RuntimeError(f'{name} is missing from the game')
        packed = source.read_bytes()
        state += sha(packed)
        rows.append((name, wanted_in, wanted_out, ops, packed))
    root = Path(cache) / Path(patch).stem
    stamp = root / 'complete'
    wanted = sha(state.encode())
    if stamp.is_file() and stamp.read_text().strip() == wanted:
        return root.resolve()
    shutil.rmtree(root, ignore_errors=True)
    for name, wanted_in, wanted_out, ops, packed in rows:
        raw = dcx_unpack(packed)
        if sha(raw) != wanted_in:
            raise RuntimeError(f'{name} is not the 1.09 file')
        result = bytearray()
        for op in ops:
            result += raw[op[0]:op[0] + op[1]] if isinstance(op, tuple) else op
        if sha(bytes(result)) != wanted_out:
            raise RuntimeError(f'{name} does not match the verified result')
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(dcx_pack(bytes(result)))
    stamp.write_text(wanted + '\n')
    return root.resolve()


def prepare_menu_assets(game, elf_path, cache, tool, rebirth=False, host_title='', guest_title=''):
    """Generate bbhost's native option movie and messages from the player's own dump.

    Validate every language, source and cached output together before publishing the
    directory to the runtime. No version string or stale stamp enables native hooks.
    """
    verify_executable(elf_path)
    game, tool = Path(game), Path(tool)
    os.environ['BB_PLATE_HOST_TITLE'] = '=' + host_title
    os.environ['BB_PLATE_GUEST_TITLE'] = '=' + guest_title
    plates = f'={host_title}|={guest_title}'
    root = Path(cache) / 'pc-menus'
    movie = Path('dvdroot_ps4/menu/optionsetting.gfx')
    sources = [(movie, 'movie')]
    message_dir = game / 'dvdroot_ps4/msg'
    sources += [(p.relative_to(game), 'messages')
                for p in sorted(message_dir.glob('*/menu.msgbnd.dcx')) if p.is_file()]
    if len(sources) < 2 or len(sources) > 24:
        raise ValueError('Native options require valid menu bundles for every installed language')
    if rebirth:
        sources.append((Path('dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx'), 'rebirth'))
    generator = sha(tool.read_bytes())
    manifest_path = root / 'manifest.json'
    try:
        previous = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        previous = {}
    rows = {}
    for relative, kind in sources:
        source, destination = game / relative, root / relative
        if source.stat().st_size > 64 * 1024 * 1024:
            raise ValueError(f'Native menu source is oversized: {relative}')
        digest = sha(source.read_bytes())
        prior = previous.get('files', {}).get(relative.as_posix(), {})
        current = None
        if (previous.get('generator') == generator and prior.get('input') == digest and
                (kind != 'messages' or prior.get('plates') == plates)):
            try:
                current = sha(destination.read_bytes())
                if current != prior.get('output'):
                    current = None
            except OSError:
                pass
        if current is None:
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(destination.name + f'.{os.getpid()}.tmp')
            try:
                result = subprocess.run([str(tool), kind, str(source), str(temporary)],
                                        capture_output=True, text=True, errors='replace')
                if result.returncode:
                    raise ValueError(result.stderr.strip() or f'Menu generator failed: {relative}')
                current = sha(temporary.read_bytes())
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
        rows[relative.as_posix()] = {'input': digest, 'output': current, 'plates': plates}
    root.mkdir(parents=True, exist_ok=True)
    temporary = manifest_path.with_suffix(f'.{os.getpid()}.tmp')
    temporary.write_text(json.dumps({'generator': generator, 'files': rows}, indent=2), encoding='utf-8')
    os.replace(temporary, manifest_path)
    return root.resolve()


def prepare_wide_movies(game, option_movie, cache, tool, width, height):
    """Widescreen: the game's menu movies whose full-screen dims, fades and bands stop at the
    16:9 stage's edges, widened by tool (bb-engine-assets wide-movies) for the UI viewport
    width x height (BB_UI_SIZE) into cache/wide-ui-py/WxH, and kept while the movies, the tool and the
    size stay the same. option_movie, when given, is the PC menus' optionsetting.gfx, widened in
    place of the game's. Returns (relative path, file, what changed) for each movie made. As
    start/assets.cpp PrepareWideMovies."""
    game, tool = Path(game), Path(tool)
    size = f'{width}x{height}'
    # Apart from bb-start.exe's cache/wide-ui: the two take a file's time from different clocks, and
    # in one folder each would make the other's movies again at every start.
    root = Path(cache) / 'wide-ui-py' / size
    option = 'dvdroot_ps4/menu/optionsetting.gfx'
    sources = sorted((f'dvdroot_ps4/menu/{p.name}', Path(option_movie) if p.name == 'optionsetting.gfx' and option_movie else p)
                     for p in (game / 'dvdroot_ps4/menu').glob('*.gfx') if p.is_file())
    if not sources:
        raise ValueError(f'no menu movies in {game / "dvdroot_ps4/menu"}')
    # Told by each file's size and time: hashing some 70 movies at every start took longer than the
    # check is worth.
    def stamp(path):
        try:
            info = path.stat()
        except OSError:
            return 'missing'
        return f'{info.st_size}:{info.st_mtime_ns}'
    key = sha(f'{stamp(tool)} {size}'.encode() +
              b''.join(f'\n{relative} {stamp(source)}'.encode() for relative, source in sources))
    manifest_path = root / 'manifest.json'
    try:
        previous = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        previous = {}
    if previous.get('key') == key and 'files' in previous:
        made = [(f['relative'], root / f['relative'], f.get('note', '')) for f in previous['files']]
        try:
            if all(stamp(path) == f['output'] for (_, path, _), f in zip(made, previous['files'])):
                return made
        except (OSError, KeyError):
            pass
    # Only the movies this made before are removed: a movie that needs nothing now must not stay.
    for f in previous.get('files', []):
        if f.get('relative'):
            (root / f['relative']).unlink(missing_ok=True)
    root.mkdir(parents=True, exist_ok=True)
    listing = root / 'sources.txt'
    listing.write_text(''.join(f'{relative}\t{source}\n' for relative, source in sources), encoding='utf-8')
    result = subprocess.run([str(tool), 'wide-movies', size, str(listing), str(root)],
                            capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise ValueError(f'the menu movie tool failed: {(result.stderr or result.stdout).strip()}')
    made, rows = [], []
    for line in result.stdout.splitlines():
        relative, tab, note = line.partition('\t')
        if not tab or not relative:
            continue
        if relative.startswith('!'):
            print(f'Widescreen: {relative[1:]} left as it is: {note}', flush=True)
            continue
        path = root / relative
        rows.append({'relative': relative, 'output': stamp(path), 'note': note})
        made.append((relative, path, note))
    temporary = manifest_path.with_suffix(f'.{os.getpid()}.tmp')
    temporary.write_text(json.dumps({'key': key, 'files': rows}, indent=2), encoding='utf-8')
    os.replace(temporary, manifest_path)
    return made

# "Jump on its own key" (the Windows start's PrepareJumpKey): the game's 1.09 action script with
# L3 jumping by itself, sprinting or not, and the dodge button dodging only. Same offsets and
# hashes as engine/start/assets.cpp, so a Linux and a Windows start prepare the identical file.
JUMP_SCRIPT = Path('dvdroot_ps4/action/script/c0000.hks')
JUMP_INPUT_SHA = '335a1af801d4f2108fc77d347593a2ca65a43f5dd6e23348f47d953606d34c59'
JUMP_OUTPUT_SHA = '026275a6d3fef4830ee765698abb1658a747a0f5c9a0246ffe9ec8b28cef9bdf'
JUMP_SITES = ((0x1e450, bytes.fromhex('b6004d0298004e14'), bytes.fromhex('32001a0250052602')),
              (0x1e48c, bytes.fromhex('39000b00'), bytes.fromhex('38ffff00')))


def prepare_jump_key(game, cache, log=print):
    """The action script for "Jump on its own key", as a (game path, file) pair for BB_FILE_FIXES,
    or None: the script is already patched (a mod ships it) or is not the 1.09 file. The patched
    script is kept under cache/jump-key, as the Windows start keeps it. Same bytes as
    engine/start/assets.cpp PrepareJumpKey."""
    game, cache = Path(game), Path(cache)
    try:
        script = (game / JUMP_SCRIPT).read_bytes()
    except OSError:
        return None
    if sha(script) == JUMP_OUTPUT_SHA:
        return None
    if sha(script) != JUMP_INPUT_SHA:
        log('Engine: jump on its own key: the action script is not the 1.09 file '
            '(a mod changes it): jump stays on the dodge button')
        return None
    data = bytearray(script)
    for site, old, new in JUMP_SITES:
        if data[site:site + len(old)] != old:
            raise ValueError('Unexpected action script')
        data[site:site + len(new)] = new
    if sha(data) != JUMP_OUTPUT_SHA:
        raise ValueError('The action script does not match the verified result')
    destination = cache / 'jump-key' / JUMP_SCRIPT.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(bytes(data))
    return JUMP_SCRIPT, destination.resolve()
