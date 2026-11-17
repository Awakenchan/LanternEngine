#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import glob
import os
import re
import struct
import sys
import zlib

USAGE = 'usage: npc_quest_flags.py <game folder>/dvdroot_ps4/event <progress_flags.inc> > quest_extra_flags.inc'
READS = {3, 1003}
SET_FLAG, SET_BATCH = (2003, 2), (2003, 22)


def unpack(path):
    data = open(path, 'rb').read()
    if data[:4] != b'DCX\0':
        raise ValueError(f'{path}: not DCX')
    return zlib.decompress(data[0x4c:])


def events(path):
    d = unpack(path)
    if d[:4] != b'EVD\0':
        raise ValueError(f'{path}: not EMEVD')
    count, events_at, _, instructions_at = struct.unpack_from('<4Q', d, 0x10)
    args_at = struct.unpack_from('<Q', d, 0x10 + 13 * 8)[0]
    for e in range(count):
        event, n, first = struct.unpack_from('<3Q', d, events_at + 0x30 * e)
        steps = []
        for k in range(n):
            bank, ident, size, offset = struct.unpack_from('<IIQq', d, instructions_at + first + 0x20 * k)
            arg = d[args_at + offset:args_at + offset + size]
            steps.append((bank, ident, [struct.unpack_from('<i', arg, o)[0] for o in range(0, len(arg) - 3, 4)]))
        yield event, steps


def ranges(inc):
    out = []
    for m in re.finditer(r'\{\s*(\d+)u?\s*,\s*(\d+)u?\s*,\s*(\d+)\s*\}', open(inc, encoding='utf-8').read()):
        out.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out


def npc_group(flag):
    group = flag // 1000
    return flag > 0 and (group == 1 or group // 10000 == 7)


def inside(flag, spans):
    return any(a <= flag <= b for a, b in spans)


def merge(flags):
    out = []
    for f in sorted(flags):
        if out and f == out[-1][1] + 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(USAGE)
    table = ranges(sys.argv[2])
    quest = [(a, b) for a, b, kind in table if kind == 1]
    kept = [(a, b) for a, b, kind in table if kind != 1]
    scripts = []
    for path in sorted(glob.glob(os.path.join(sys.argv[1], '*.emevd.dcx'))):
        scripts.extend(events(path))
    extra = set()
    for _, steps in scripts:
        for bank, ident, ints in steps:
            if (bank, ident) == SET_BATCH and len(ints) >= 2 and npc_group(ints[0]) and ints[0] // 1000 == ints[1] // 1000:
                extra.update(range(ints[0], ints[1] + 1))
    while True:
        known = quest + [(f, f) for f in extra]
        grown = set()
        for _, steps in scripts:
            reads = any(bank in READS and any(inside(v, known) for v in ints[1:]) for bank, ident, ints in steps)
            if not reads:
                continue
            for bank, ident, ints in steps:
                if (bank, ident) == SET_FLAG and ints:
                    grown.add(ints[0])
                elif (bank, ident) == SET_BATCH and len(ints) >= 2 and ints[0] // 1000 == ints[1] // 1000:
                    grown.update(range(ints[0], ints[1] + 1))
        grown = {f for f in grown if npc_group(f) and not inside(f, quest) and not inside(f, kept)} - extra
        if not grown:
            break
        extra |= grown
    extra = {f for f in extra if not inside(f, quest) and not inside(f, kept)}
    for a, b in merge(extra):
        print(f'{{{a}u, {b}u}},')


if __name__ == '__main__':
    main()
