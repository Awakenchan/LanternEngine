#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import bisect
import math
import struct
import sys
from pathlib import Path
from xml.sax.saxutils import escape

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

BASE = 0x400000
ROOT = Path(__file__).resolve().parents[2]
XML = ROOT / 'patches' / 'Bloodborne.xml'
TITLE_IDS = ('CUSA00207', 'CUSA00208', 'CUSA00299', 'CUSA00900', 'CUSA01363', 'CUSA03014', 'CUSA03023',
             'CUSA03173')
AUTHOR = 'Lantern Engine'

ONE_THIRTIETH = struct.pack('<f', 1 / 30)
THIRTY = struct.pack('<f', 30.0)
NOPS = {1: '90', 2: '6690', 3: '0f1f00', 4: '0f1f4000', 5: '0f1f440000', 6: '660f1f440000',
        7: '0f1f8000000000', 8: '0f1f840000000000', 9: '660f1f840000000000'}


def f32(*values):
    return b''.join(struct.pack('<f', v) for v in values)


def u32(value):
    return struct.pack('<I', value)


def h(text):
    return bytes.fromhex(text.replace(' ', ''))


def nop(n):
    out = b''
    while n:
        step = min(n, 9)
        out += h(NOPS[step])
        n -= step
    return out


def rel(at, length, target):
    return struct.pack('<i', target - (at + length))


def jmp(at, target):
    short = target - (at + 2)
    return bytes([0xeb, short & 0xff]) if -128 <= short < 128 else b'\xe9' + rel(at, 5, target)


def jmp32(at, target):
    return b'\xe9' + rel(at, 5, target)


def call(at, target):
    return b'\xe8' + rel(at, 5, target)


def rip_op(prefix, at, target, tail=b''):
    return prefix + rel(at, len(prefix) + 4 + len(tail), target) + tail


class Eboot:
    def __init__(self, path):
        self.file = Path(path).read_bytes()
        phoff, = struct.unpack_from('<Q', self.file, 0x20)
        phnum, = struct.unpack_from('<H', self.file, 0x38)
        headers = [struct.unpack_from('<IIQQQQQQ', self.file, phoff + 56 * i) for i in range(phnum)]
        self.loads = [(vaddr, offset, filesz, memsz) for kind, _, offset, vaddr, _, filesz, memsz, _ in headers
                      if kind == 1]
        frames = next(hd for hd in headers if hd[0] == 0x6474e550)
        code_va, code_offset = self.loads[0][0], self.loads[0][1]
        count, = struct.unpack_from('<i', self.file, frames[2] + 8)
        self.functions = []
        for i in range(count):
            start, fde = struct.unpack_from('<ii', self.file, frames[2] + 12 + 8 * i)
            size, = struct.unpack_from('<i', self.file, frames[3] + fde - code_va + code_offset + 12)
            self.functions.append((frames[3] + start + BASE, size))
        self.functions.sort()
        self.starts = [s for s, _ in self.functions]

    def function(self, address):
        k = bisect.bisect_right(self.starts, address) - 1
        if k >= 0 and address < self.functions[k][0] + self.functions[k][1]:
            return self.functions[k][0]
        return None

    def read(self, address, length):
        image = address - BASE
        for vaddr, offset, filesz, memsz in self.loads:
            if vaddr <= image and image + length <= vaddr + memsz:
                start = image - vaddr
                data = self.file[offset + start:offset + min(filesz, start + length)] if start < filesz else b''
                return bytes(data) + bytes(length - len(data))
        raise ValueError(f'{address:#x} is outside the eboot')

    def code(self):
        vaddr, offset, filesz, _ = self.loads[0]
        return vaddr + BASE, self.file[offset:offset + filesz]

    def disp_target(self, at, length):
        return at + length + struct.unpack('<i', self.read(at + length - 4, 4))[0]


class Patch:
    def __init__(self, name, note, author=None):
        self.name, self.note, self.writes = name, note, []
        self.author = author

    def write(self, at, was, now):
        if len(was) != len(now):
            raise ValueError(f'{self.name}: {at:#x} changes size')
        if was != now:
            self.writes.append((at, bytes(was), bytes(now)))
        return self


class Builder:
    def __init__(self, eboot):
        self.eboot = eboot

    def site(self, patch, at, was, now):
        found = self.eboot.read(at, len(was))
        if found != was:
            raise ValueError(f'{patch.name}: {at:#x} holds {found.hex()}, not {was.hex()}')
        patch.write(at, was, now)

    def same(self, patch, at, now):
        self.site(patch, at, self.eboot.read(at, len(now)), now)

    def retarget(self, patch, at, length, was_target, target):
        if self.eboot.disp_target(at, length) != was_target:
            raise ValueError(f'{patch.name}: {at:#x} does not read {was_target:#x}')
        self.same(patch, at + length - 4, rel(at, length, target))


def skip_intro(b):
    p = Patch('Skip Intro', 'Starts at the title screen: the PlayStation, Japan Studio and FromSoftware logo '
                            'movies are not shown.')
    for at in (0x1fb50fa, 0x1fb5145, 0x1fb5190):
        b.site(p, at, h('440fb6c0'), h('4531c0') + nop(1))
    return p


def renderer_switch(b, name, note, at):
    p = Patch(name, note)
    b.site(p, at, h('01'), h('00'))
    return p


def effects(b):
    return [
        renderer_switch(b, 'Disable SSAO', "Turns the game's screen-space ambient occlusion off.", 0x26c2548),
        renderer_switch(b, 'Disable Motion Blur (perf increase)',
                        "Turns the game's motion blur off, and the velocity pass it needs. Faster.", 0x26c2549),
        renderer_switch(b, 'Disable AA', "Turns the game's own anti-aliasing off.", 0x26c254a),
        renderer_switch(b, 'Disable Dynamic Light Shadows (perf increase)',
                        'Lights that move (lamps, fire, effects) cast no shadows. Faster.', 0x26c2538),
        chromatic_aberration(b), depth_of_field(b)]


def chromatic_aberration(b):
    p = Patch('Disable Chromatic Aberration', 'No colour fringing at the edges of the picture.')
    b.site(p, 0x269faa8, h('8b8590f5ffff'), h('31c0') + nop(4))
    return p


def depth_of_field(b):
    p = Patch('Disable DoF', 'No depth of field blur.')
    b.site(p, 0x25d7bbc, h('0f84de000000'), jmp32(0x25d7bbc, 0x25d7ca0) + nop(1))
    return p


def model_detail(b):
    out = []
    for name, note, level in (('Model LOD -2 (Highest)', 'Every model at its most detailed level, at any distance. '
                                                         'Slower.', -2),
                              ('Model LOD 1 (Lower)', 'Models one detail level lower than the game picks. Faster.', 1),
                              ('Model LOD 2 (Lowest)', 'Models two detail levels lower than the game picks. Faster.', 2)):
        p = Patch(name, note)
        b.site(p, 0x216fc09, h('8b0d11818103'), b'\xb9' + struct.pack('<i', level) + nop(1))
        out.append(p)
    return out


def graphics_heaps(b):
    p = Patch('Increased Graphics Heap Sizes', "The game's two graphics heaps at 3952 and 2560 MiB instead of "
                                               "2287 and 886, for scenes above 1080p. Needs the larger memory pool.")
    b.site(p, 0x4b36dd8, struct.pack('<Q', 0x8ef00000), struct.pack('<Q', 0xf7000000))
    b.site(p, 0x4b36de0, struct.pack('<Q', 0x37600000), struct.pack('<Q', 0xa0000000))
    return p


def intel_tonemap(b):
    p = Patch('Intel Black Tonemap Fix', "The tone mapping's colour grading always runs. On Intel processors the "
                                         'flag the game computes for it came out wrong and the picture turned black.')
    b.site(p, 0x25843e5, h('0f843a010000'), nop(6))
    return p


def sfx_ribbon(b):
    p = Patch('Intel 12th Gen+ SFX workaround', 'Draws no ribbon effects (weapon trails of one kind). Only for '
                                                'the crash some newer Intel processors had there.')
    b.site(p, 0x2cf83e0, h('55'), h('c3'))
    return p


def region(b):
    p = Patch('Unlock Game Region', 'The game takes the region that lists every language, keeping Cross to '
                                    'confirm. That region has no online server: with online on, the engine leaves '
                                    'this patch out.')
    b.site(p, 0x23b9190, h('554889e54157'), h('b805000000c3'))
    b.site(p, 0x23b8730, h('554889'), h('31c0c3'))
    return p


def all_languages(b):
    p = Patch('Unlock All Languages (Works Online)',
              'The European text list becomes the full list, and the voice list the eight languages with their '
              'own recordings. The sell region stays the international one, so the EU/US server still matches. '
              'Leave Unlock Game Region off.')
    b.retarget(p, 0x23b9710, 7, 0x4b36780, 0x4b368f0)
    b.site(p, 0x23b9724, h('0e'), h('15'))
    b.retarget(p, 0x23b9728, 7, 0x4b367c0, 0x4b36950)
    b.retarget(p, 0x23b9cbe, 7, 0x4b34980, 0x4b34b50)
    b.site(p, 0x23b9cd2, h('05'), h('08'))
    b.retarget(p, 0x23b9cd6, 7, 0x4b36a80, 0x4b36ba0)
    return p


def camera_auto_turn(b):
    p = Patch('Disable Camera Auto Rotation via Movement', 'The camera stops turning by itself as the character '
                                                           'moves or nears a wall. The stick and lock-on turn it as '
                                                           'usual.')
    for at, was in ((0x183c6e8, 'c4c17a118544010000'), (0x183c870, 'c4c17a11ad44010000'),
                    (0x183c984, 'c4c17a118d30010000'), (0x183dde6, 'c4c17a118594020000')):
        b.site(p, at, h(was), b'\x90' * 9)
    return p


def spare(b, p, at, data):
    if not (FREE_BLOCK[0] <= at and at + len(data) <= FREE_BLOCK[1]):
        raise ValueError(f'{p.name}: {at:#x} is outside the unused block')
    b.site(p, at, bytes(len(data)), data)


def utf16(text, size):
    data = text.encode('utf-16-le') + b'\0\0'
    return data + bytes(size - len(data))


def debug_menu(b):
    p = Patch('Restore Debug Menu (READ NOTES)', "The developers' debug menu: left of the touchpad (Tab) opens it. "
                                                 'It needs the debug font, DbgFont14h.ccm and DbgFont14h.tpf, in '
                                                 'dvdroot_ps4/font (Nexus mod #253); the engine leaves the patch '
                                                 'out without them.')
    ccm, tpf = FONT_PATHS
    spare(b, p, ccm, utf16('adhoc:/font/DbgFont14h.ccm', 0x40))
    spare(b, p, tpf, utf16('adhoc:/font/DbgFont14h.tpf', 0x40))
    b.site(p, 0x136d90d, h('74'), h('75'))
    cave = 0x2199ef6
    code = (rip_op(h('488d15'), cave, ccm) + rip_op(h('488d0d'), cave + 7, tpf) + h('31f6 4889df') +
            call(cave + 19, 0x136dc70) + h('4c89f7'))
    code += jmp(cave + len(code), 0x2199f26)
    b.same(p, cave, code + b'\xcc' * (0x2199f1b - cave - len(code)))
    b.site(p, 0x2199f23, h('4c89f7'), jmp(0x2199f23, cave) + nop(1))
    b.site(p, 0x23abef9, call(0x23abef9, 0x13b5350), call(0x23abef9, 0x13b5360))
    b.site(p, 0x23abf06, call(0x23abf06, 0x13b5360), call(0x23abf06, 0x13b5350))
    b.site(p, 0x2418058, h('b9') + u32(0), h('b9') + u32(1))
    return p


SCREEN_W, SCREEN_H, RENDER_W, RENDER_H = 0x55289f8, 0x55289fc, 0x59404d8, 0x59404dc
SCENE_SITES = ((0x2196a6b, SCREEN_W, 0xb8), (0x2196a7a, SCREEN_H, 0xb8))
UI_SITES = ((0x2358554, SCREEN_W, 0xb8), (0x235855d, SCREEN_H, 0xb9))
HUD_SITES = ((0x19e83af, SCREEN_W, 0xb8), (0x1a44357, SCREEN_W, 0xb8), (0x1a44365, SCREEN_H, 0xb8),
             (0x1a44c55, SCREEN_W, 0xb8), (0x1a44c63, SCREEN_H, 0xb8), (0x1a452c7, SCREEN_W, 0xb8),
             (0x1a452d5, SCREEN_H, 0xb8), (0x1ffd491, SCREEN_W, 0xb8), (0x1ffd4d1, SCREEN_H, 0xb8),
             (0x212c674, RENDER_W, 0xbe), (0x212c6a4, RENDER_H, 0xbe), (0x241848f, RENDER_W, 0xb8),
             (0x2418498, RENDER_H, 0xb9), (0x2438bb9, RENDER_W, 0xb8), (0x2438bc7, RENDER_H, 0xb8),
             (0x2438f59, RENDER_W, 0xb8), (0x2438f67, RENDER_H, 0xb8))


def size_load(b, p, at, source, opcode, value, split):
    load = b.eboot.read(at, 9)
    if load[1] != 0x8d or load[7] != 0x8b or b.eboot.disp_target(at, 7) != source:
        raise ValueError(f'{p.name}: {at:#x} is not a load of {source:#x}')
    now = bytes([opcode]) + u32(value) + nop(4)
    if split:
        b.site(p, at, load[:4], now[:4])
        b.site(p, at + 4, load[4:], now[4:])
    else:
        b.site(p, at, load, now)


def resolution(b):
    p = Patch('Resolution Patch 1280x720 (16:9)', 'The scene and UI at 1280x720, the HUD, lock-on and health bars '
                                                  'kept in the 1920x1080 space they are placed in. The engine uses '
                                                  'it as the template for its render resolutions.')
    for at, source, opcode in SCENE_SITES + UI_SITES:
        size_load(b, p, at, source, opcode, 1280 if source == SCREEN_W else 720, True)
    for at, source, opcode in HUD_SITES:
        size_load(b, p, at, source, opcode, 1920 if source in (SCREEN_W, RENDER_W) else 1080, False)
    b.site(p, SCREEN_W, u32(1920) + u32(1080), u32(640) + u32(360))
    return p


FRAME_TIME = 0x575a908
FRAME_TIMER = 0x59404f8
FRAME_TIME_CLAMP = 0x4d26364
FREE_BLOCK = (0x3abbde2, 0x3abcdc6)
EASE_WEIGHT = 0x3abc000
FONT_PATHS = (0x3abc010, 0x3abc050)
TASK_STEP_EXCEPTIONS = {0xf383ca, 0x1972a80}
FRAME_TIMER_UPDATE = (0x2434770, 0x2435000)
STEP_THIRTY_CODE = (0x13ef51c, 0x13ef6ec, 0x187305d, 0x187458d, 0x187afec, 0x187b0ad, 0x1880cfd, 0x1880e1a,
                    0x18b254d, 0x18b382d, 0x1ab67ef, 0x1c9f889, 0x1ca975a, 0x1ca9892, 0x203acb7, 0x21bb004,
                    0x23dd026, 0x23dede6)
STEP_THIRTY_DATA = (0x4c9f358, 0x4d02f10, 0x4d02f14, 0x4d02f18, 0x4d02f1c, 0x4d02f70, 0x4d02f74, 0x4d02f78,
                    0x4d02f7c)
STEP_DATA = (0x4befdcc, 0x4c88688, 0x4cad65c, 0x4cad660, 0x4cad664, 0x4cadc08, 0x4cadd28, 0x4ccc3f4, 0x4d265fc,
             0x4d26618, 0x4d26c9c, 0x4d270e4, 0x4d27554, 0x4d275f4, 0x4d27720, 0x4d27c4c, 0x4d28418, 0x4d28e98,
             0x4d29b80, 0x4d29e74)
FRAME_TIME_LOADS = ((0xeb9f0d, 8, 0x4befdcc), (0x1444943, 8, 0x4ccc3f4), (0x1bf9d26, 8, 0x4d27554),
                    (0x1c0f647, 8, 0x4d275f4), (0x1cbc4f7, 8, 0x4d27c4c), (0x1cbc50d, 8, 0x4d27c4c),
                    (0x2377d4a, 8, 0x4d29170), (0x267784b, 8, 0x4d29b80), (0x2677892, 8, 0x4d29b80),
                    (0x2713d8d, 8, 0x4d29e70), (0x2713dad, 8, 0x4d29e70), (0x2713db7, 8, 0x4d29e74),
                    (0x2715d71, 8, 0x4d29e74), (0x1915fab, 8, 0x4d26364), (0x1916030, 8, 0x4d26364))
CLOTH_CONSTRAINTS = ((0xb20dee, 0xb20e12, 0xa0), (0xb2b2fe, 0xb2b322, 0xa0), (0xb30ade, 0xb30b02, 0xa0),
                     (0xb38711, 0xb3873d, 0xb0), (0xb41f4e, 0xb41f72, 0x80), (0xb420ee, 0xb42112, 0xa0))
CLOTH_SCALE = 0x575a90c
SIXTY = 0x3abc004
ONE = 0x3abc0a0
CLOTH_SCALE_CODE = 0x3abc0b0
FRAME_MODE_TIMES = (0x2434840, 0x243485a, 0x243487e, 0x24348b3)
LIMITER_SLEEP = {30: 33000, 60: 16000, 90: 10000}
PERF_CAVES = 0x3abc300
ENEMY_CAVES = 0x3abc400
AIR_DRAG_CODE = 0x3abc500
AIR_DRAG_DATA = 0x3abc560
AIR_DRAG = 0x1e4abca
PERF_DATA = (
    (0x547aed0, '09'),
    (0x547aef4, '09'),
    (0x5522750, '01'),
    (0x553b310, '00'),
    (0x553ac90, '01'),
    (0x553acc8, '00'),
    (0x553accc, '00'),
    (0x553acd4, '01'),
    (0x553ae80, '01000000'),
    (0x553ae84, '01000000'),
    (0x553aed0, '09000000'),
    (0x553b124, '00'),
    (0x553b128, 'ff'),
    (0x553b134, 'ff'),
    (0x553b1a0, 'ff'),
    (0x553b18c, '00'),
    (0x58b0870, '01'),
    (0x553ad1c, '00'),
    (0x553ad1a, '00'),
    (0x5527b8e, '00'),
    (0x5aa6bc0, '01'),
    (0x4b35024, '1e000000'),
    (0x4b35028, '1e000000'),
    (0x4b35030, '1e000000'),
    (0x5922ca0, '0000803e0000803e0000803e0000803e'),
    (0x5929eb4, '0000803e'),
)
PERF_NOPS = (
    (0x25843e5, 6),
    (0x26a2829, 5),
    (0x26a10c0, 5),
    (0x26a283f, 5),
    (0x26ab335, 5),
    (0x26ab381, 5),
    (0x24b56d2, 11),
    (0x2ab86a7, 2),
    (0x2ab9207, 2),
    (0xfc96fd, 5),
    (0xfe93d1, 5),
    (0xff35c2, 5),
    (0xff35e9, 5),
    (0xff37a2, 5),
    (0xff37c9, 5),
    (0xff2952, 5),
    (0xff2975, 5),
    (0x2434bd2, 5),
    (0xded443, 5),
    (0xded470, 5),
)
PERF_ALWAYS = (0x26ab34e, 0x126445c, 0x216a963, 0xfc96ed, 0x2394f62, 0x2434fe3, 0x2435043, 0x24350a3, 0x2435103, 0x2435163, 0x24351c3)
PERF_SKIPS = (
    (0x24cc437, 0x24cc48e),
    (0x24cc4cf, 0x24cc526),
    (0x14485b1, 0x14485fe),
    (0x144864f, 0x14486b1),
    (0x1db6000, 0x1db600f),
    (0x1db60e4, 0x1db6131),
    (0x1db6517, 0x1db6526),
    (0x1db65e0, 0x1db662e),
    (0x24ccb14, 0x24ccb5b),
    (0x24ccb68, 0x24ccbaf),
    (0x24cfc0f, 0x24cfc66),
    (0x24cfc9a, 0x24cfcf1),
    (0x24de725, 0x24de794),
    (0x24de7d5, 0x24de844),
    (0x24dede0, 0x24dee27),
    (0x24dee60, 0x24deea7),
)
PERF_RETURNS = (
    (0xe73a60, 0),
    (0xe73aa0, 0),
    (0x466800, 1),
)
PERF_EDITS = (
    (0x1bfa58e, '05', '01'),
    (0x261b108, 'c745b406000000', 'c745b409000000'),
    (0x216fc09, '8b0d11818103', 'b9feffffff90'),
    (0x26b7720, '48c7459800000000', '41c6868179000001'),
    (0x26b2f58, '91200000', '60000000'),
    (0x26b90c5, 'c745c800010000', 'c745c880070000'),
    (0x26b90cc, 'c745cc00010000', 'c745cc38040000'),
    (0x26b8634, '48c78588feffff00000000', '41c785b878000010000000'),
    (0x26b8620, '41b806000000', '41b810000000'),
    (0x26b8694, '498d95bd780000', '488d15f225e802'),
    (0x2695cb6, 'c783a878000080000000', 'c783a878000080070000'),
    (0x2695cc0, 'c783ac78000080000000', 'c783ac78000038040000'),
    (0x241ae52, '8b008d48ce488d159adb10038b12', 'b80e0100008d48cebae001000090'),
    (0x241a33a, '488d15976152038902488d05926152038908', 'c1e802c1e902890594615203890d92615203'),
    (0x125b1d8, 'c1e802', 'c1e804'),
    (0x125b1e1, 'c1e802', 'c1e804'),
    (0xfd342b, '4189f7', '4531ff'),
    (0xfd34bf, 'c644085001', 'c644085000'),
    (0x1d13e1d, 'c7402c00002042', 'c7402c0000a041'),
    (0x111a884, '69ffe8030000', '69ff64000000'),
    (0x26a9579, '0000003f', '0000803e'),
    (0x26c107a, 'c7435c203000ff', 'c7435c000000ff'),
    (0x26c5369, 'c7405c00000000', 'c7405c000000ff'),
    (0x26c5146, '01', '00'),
    (0x26c4c6f, '01', '00'),
    (0x26c4fd7, '01', '00'),
    (0x26c4660, '01', '00'),
    (0x26c4649, 'e8d21dfaff', '31c00f1f00'),
    (0x26c3e43, 'e8d825faff', '31c00f1f00'),
    (0x26c43ca, 'e85120faff', '31c00f1f00'),
    (0x26c4740, 'e8db1cfaff', '31c00f1f00'),
    (0x172b2ca, '458aa58a000000', '41b40190909090'),
    (0x19550f1, '41c745580000c841', '41c745580000a041'),
    (0x19550f9, '41c7455c0000f041', '41c7455c0000c841'),
    (0x1955101, '41c74560cdcc4c3e', '41c7456000000000'),
    (0x1955109, '41c745646666a63f', '41c745649a99993f'),
    (0x195516c, '41c785000800000000c841', '41c785000800000000a042'),
    (0x1955177, '41c785040800000000f041', '41c785040800000000b442'),
    (0x1955182, '41c78508080000cdcc4c3e', '41c7850808000000000000'),
    (0x195518d, '41c7850c0800006666a63f', '41c7850c0800009a99993f'),
    (0x1264606, '0f83cf0b0000', 'e9d00b000090'),
    (0x2606a7f, '01', '00'),
    (0x2606a86, '01', '00'),
    (0x260691b, '01', '00'),
    (0x2606b39, '01', '00'),
    (0x2606b32, '00', '01'),
    (0x16fcea4, 'e87f238c0148', 'b81200000090'),
    (0x16339ff, 'e824b8980148', 'b80700000090'),
    (0x1698acc, 'e8576792014c', 'b80100000090'),
    (0x2594aa6, '418b8cc480010000898c24a00000004d8db4c484010000418b8cc484010000898c24a4000000c78424 b000000057000000c78424b400000000000000c78424b800000000000000c78424a80000003c000000c78424ac00000001000000498b8cc4 8801000048898c24bc000000c78424c400000003000000c78424c800000003000000', '418d4dff83f97f7718c78424a000000080020000 c78424a400000068010000eb1e418b8cc480010000898c24a0000000418b8cc484010000898c24a40000004d8db4c484010000c68424b000 000057c78424a80000003c000000c68424ac00000001498b8cc48801000048898c24bc000000c68424c400000003c68424c80000000390'),
    (0x2594eb0, '8b4348488d0c40418b84cc80010000894328418b8ccc84010000894b2c4180bc24050100000089ca75664180bc2406010 0000089ca7459', '8b43488d48ff83f97f7712b880020000894328b968010000894b2ceb1a488d0c40418b84cc80010000894328418b8cc c84010000894b2c'),
    (0x466816, 'e8030000', '64000000'),
    (0x138a540, '3c', '2c'),
    (0x1970ca0, '01', '00'),
    (0x1970cbb, '01', '00'),
    (0x1970cd6, '01', '00'),
    (0x1970cf1, '01', '00'),
    (0x1970d0c, '01', '00'),
    (0x1970d2f, '01', '00'),
    (0x1970d4b, '01', '00'),
    (0x1970d67, '01', '00'),
    (0x1970d83, '01', '00'),
    (0x1970d9f, '01', '00'),
    (0x1970003, '01010100', '00000000'),
    (0x1970010, '0101', '0000'),
    (0x197001a, '01010101', '00000000'),
    (0x1c31160, 'bfa0860100', 'bfa8610000'),
    (0x23ce1ed, 'bf204e0000', 'bf88130000'),
    (0x25629f0, 'bfe8800000', 'bf401f0000'),
    (0x25b69a7, 'bf409c0000', 'bf10270000'),
    (0x23ce3f5, 'bf204e0000', 'bf88130000'),
    (0x224a709, 'bf4d010000', 'bf53000000'),
    (0x24bd1c0, 'bfe8030000', 'bffa000000'),
    (0x24bdd50, 'bfe8030000', 'bffa000000'),
    (0x24cfb32, 'bfe8030000', 'bffa000000'),
    (0x24d2d90, 'bff4010000', 'bf7d000000'),
    (0x24d4859, 'bff4010000', 'bf7d000000'),
    (0x23868f0, 'bfe8030000', 'bffa000000'),
    (0x2f98d55, 'bf803e0000', 'bfa00f0000'),
    (0x2f98d87, 'bf803e0000', 'bfa00f0000'),
)
UNCAPPED_SLEEP = 1000
SSR_TWO = 0x3abc008
FREE_CAMERA = 0x3abc100


def immediate_stores(eboot, value):
    disasm = Cs(CS_ARCH_X86, CS_MODE_64)
    start, code = eboot.code()
    found = []
    at = code.find(value)
    while at != -1:
        address = start + at
        if eboot.function(address):
            for back in range(2, 11):
                ins = next(disasm.disasm(eboot.read(address - back, 16), address - back), None)
                if ins and ins.mnemonic == 'mov' and ins.address + ins.size == address + 4 and '[' in ins.op_str:
                    found.append(address)
                    break
        at = code.find(value, at + 1)
    return found


def task_steps(b, p, skip=()):
    steps = [a for a in immediate_stores(b.eboot, ONE_THIRTIETH) if a not in TASK_STEP_EXCEPTIONS and
             a not in skip and not FRAME_TIMER_UPDATE[0] <= a < FRAME_TIMER_UPDATE[1]]
    for address in steps:
        b.site(p, address, ONE_THIRTIETH, f32(1 / 60))
    return steps


def frame_time_cave(b, p, fps):
    a = 0x2418e3d
    first = (h('c5fa108364020000') + h('c5fa5f4318') + rip_op(h('c5fa5d05'), a + 13, FRAME_TIME_CLAMP) +
             h('c5fa1145c8') + rip_op(h('c5fa5c05'), a + 26, FRAME_TIME))
    second_at = 0x2418e9e
    first += jmp(a + len(first), second_at)
    b.same(p, a, first + b'\xcc' * (0x2418e6b - a - len(first)))
    b.site(p, 0x2418e9c, h('488d'), jmp(0x2418e9c, 0x2418ebd))
    second = (rip_op(h('c5fa5905'), second_at, EASE_WEIGHT) + rip_op(h('c5fa5805'), second_at + 8, FRAME_TIME) +
              rip_op(h('c5fa1105'), second_at + 16, FRAME_TIME))
    second += jmp32(second_at + len(second), CLOTH_SCALE_CODE)
    b.same(p, second_at, second + b'\xcc' * (0x2418ebd - second_at - len(second)))
    spare(b, p, EASE_WEIGHT, f32(0.15, 60.0))
    spare(b, p, ONE, f32(1.0))
    c = CLOTH_SCALE_CODE
    code = (rip_op(h('c5fa590d'), c, SIXTY) + h('c5f259c9') + rip_op(h('c5f25d0d'), c + 12, ONE) +
            rip_op(h('c5fa110d'), c + 20, CLOTH_SCALE))
    code += jmp32(c + len(code), 0x2418e6b)
    spare(b, p, c, code)
    b.site(p, FRAME_TIME, bytes(4), f32(1 / fps))
    b.site(p, CLOTH_SCALE, bytes(4), f32(min(1.0, (60 / fps) ** 2)))


def frame_count_cave(b, p):
    cave = 0x21bc181
    code = rip_op(h('488b05'), cave, FRAME_TIMER) + h('c5fa598064020000') + jmp32(cave + 15, 0x21bc310)
    b.same(p, cave, code + b'\xcc' * (0x21bc1a2 - cave - len(code)))
    b.site(p, 0x21bc308, h('c5fa590588cbb602'), jmp32(0x21bc308, cave) + nop(3))


def frame_timer(b, p, fps):
    b.site(p, 0x2434806, h('48631491'), h('48635108'))
    for at in FRAME_MODE_TIMES:
        b.site(p, at, b.eboot.read(at, 5), h('41c7442418'))
        b.same(p, at, h('41c7442418') + f32(1 / fps))
    b.site(p, 0x2434887, h('48b9000000001e000000'), h('48b9') + bytes(8))
    b.site(p, 0x24348a2, h('743d'), h('eb3d'))
    b.site(p, 0x24348f0, h('7808'), h('eb08'))
    b.site(p, 0x2434907, h('7808'), h('eb08'))
    b.site(p, 0x2434d98, h('7405'), nop(2))
    b.site(p, 0x24345ee, h('b804000000410f46c6'), h('b802000000') + nop(4))


def frame_limiter(b, p, fps):
    for at, skip_to in ((0x2c0118a, 0x2c011da), (0x2c011eb, 0x2c01237), (0x2c012ca, 0x2c01322)):
        b.site(p, at, h('31f6'), jmp(at, skip_to))
    cave = 0x2c012cc
    cap = u32(LIMITER_SLEEP.get(fps, UNCAPPED_SLEEP))
    code = h('81ff') + cap + h('7e05') + b'\xbf' + cap + jmp32(cave + 13, 0x2fbfe68)
    b.same(p, cave, code)
    b.site(p, 0x2c0137d, call(0x2c0137d, 0x248bac0), call(0x2c0137d, cave))


def shared_rate(b, p, fps, rate, skip_steps=(), skip_loads=()):
    frame_time_cave(b, p, 60 if fps is None else fps)
    frame_count_cave(b, p)
    frame_limiter(b, p, fps)
    b.site(p, 0x25629f0, b'\xbf' + u32(33000), b'\xbf' + u32(990000 // rate))
    b.site(p, 0xfbc40f, h('7f1d'), h('eb1d'))
    task_steps(b, p, skip_steps)
    for at in STEP_DATA:
        b.site(p, at, ONE_THIRTIETH, f32(1 / 60))
    for at in STEP_THIRTY_CODE + STEP_THIRTY_DATA:
        b.site(p, at, THIRTY, f32(60.0))
    for at, length, constant in FRAME_TIME_LOADS:
        if at not in skip_loads:
            b.retarget(p, at, length, constant, FRAME_TIME)
    b.site(p, 0x192c405, u32(10), u32(rate // 3))
    b.site(p, 0x4cf6420, f32(0.1, 0.1, 0.1, 0.1), f32(*[3 / rate] * 4))
    b.site(p, 0x4d27fd4, f32(-0.1), f32(-3 / rate))
    b.site(p, 0x4d27fe0, f32(0.1), f32(3 / rate))
    ease = 60 if fps is None else fps
    b.site(p, 0x4d25e68, f32(0.1), f32(1 - 0.9 ** (30 / ease)))
    b.site(p, 0x4d25e94, ONE_THIRTIETH, f32(1 - (1 - 1 / 30) ** (30 / ease)))
    b.site(p, 0xb6e67a, h('8b581885db7504418b5d20'), b'\xbb' + u32(1) + nop(6))
    air_drag(b, p)
    if rate > 60:
        b.site(p, 0x4d29170, ONE_THIRTIETH, f32(1 / rate))
        b.site(p, 0xf383ca, ONE_THIRTIETH, f32(1 / rate))


def frame_rate(b, fps):
    name = f'{fps} FPS++'
    p = Patch(name, f'The game at {fps} frames a second: its frame timer in the 60 FPS mode the game already has, '
                    'every step it takes per frame and the timings, counts and weights tuned for 30 FPS following '
                    'the frame time. The engine applies it from the frame-rate setting.')
    frame_timer(b, p, fps)
    shared_rate(b, p, fps, fps)
    b.site(p, 0x1929676, u32(5), u32(fps // 6))
    if fps > 60:
        cloth_stiffness(b, p)
        b.site(p, 0x1cbdb42, h('c5fa1083e0000000c5fa11857cffffff'),
               h('c7857cffffff') + f32(1.125 / fps) + b'\x90' * 6)
    return p


def uncapped_frame_timer(b, p):
    b.site(p, 0x24347f3, h('8b11'), h('31d2'))
    b.site(p, 0x243480f, h('41c744241002000000'), h('41c744241001000000'))
    b.site(p, 0x2434818, h('41c644241401'), h('41c644241400'))
    for at in FRAME_MODE_TIMES:
        b.site(p, at, b.eboot.read(at, 5), h('41c7442418'))
        b.same(p, at, h('41c7442418') + f32(0))
    b.site(p, 0x2434863, h('48b9010000001e000000'), h('48b9') + bytes(8))
    b.site(p, 0x2434887, h('48b9000000001e000000'), h('48b9') + bytes(8))
    b.site(p, 0x24348a2, h('743d'), h('eb3d'))
    b.site(p, 0x24348f0, h('7808'), h('eb08'))
    b.site(p, 0x2434907, h('7808'), h('eb08'))
    b.site(p, 0x2434d98, h('7405'), nop(2))


def uncapped_physics_step(b, p):
    b.site(p, 0x13d2e1a, h('7515'), h('eb15'))
    cave = 0x13d2e1c
    code = h('c4c17a104608') + h('41c74608') + f32(1.0) + jmp32(cave + 14, 0x13d3562)
    b.same(p, cave, code + b'\xcc' * (0x13d2e31 - cave - len(code)))
    b.site(p, 0x13d355c, h('c4c17a104608'), jmp32(0x13d355c, cave) + nop(1))


def uncapped_step_limit(b, p):
    b.site(p, 0x1bf9bde, h('0f85c5000000'), nop(6))
    cave = 0x1bf9ca9
    limit = f32(1 / 60)
    code = (rip_op(h('488b05'), cave, FRAME_TIMER) + h('8b8064020000') + b'\x3d' + limit + h('7605') + b'\xb8' +
            limit + h('8945b8'))
    code += jmp32(cave + len(code), 0x1bf9b78)
    b.same(p, cave, code + b'\xcc' * (0x1bf9cd7 - cave - len(code)))
    b.site(p, 0x1bf9b71, h('c745b8') + ONE_THIRTIETH, jmp32(0x1bf9b71, cave) + nop(2))


def uncapped_effect_step(b, p):
    b.site(p, 0x2377cea, h('7524'), h('eb24'))
    cave = 0x2377cec
    code = rip_op(h('4c8b1d'), cave, FRAME_TIMER) + h('c4c17a108364020000')
    code += jmp32(cave + len(code), 0x2377d52)
    b.same(p, cave, code + b'\xcc' * (0x2377d10 - cave - len(code)))
    b.site(p, 0x2377d4a, rip_op(h('c5fa1005'), 0x2377d4a, 0x4d29170), jmp32(0x2377d4a, cave) + nop(3))


def uncapped(b):
    p = Patch('Uncap FPS++', 'The game with no frame-rate limit of its own: its frame timer without a minimum frame '
                             'time or vertical sync, each step taking the frame time the timer measures, and the '
                             'physics, cloth and the values tuned for 30 FPS set as for 90. The engine applies it '
                             'from the frame-rate setting, with its own limiter.')
    uncapped_frame_timer(b, p)
    shared_rate(b, p, None, 90, skip_steps=(0x1bf9b74,), skip_loads=(0x2377d4a,))
    cloth_stiffness(b, p)
    uncapped_physics_step(b, p)
    uncapped_step_limit(b, p)
    uncapped_effect_step(b, p)
    b.site(p, 0x25b3271, h('4489e6'), h('31f6') + nop(1))
    b.site(p, 0x2fbf178, h('ff25725b8202'), h('31c0c3') + b'\xcc' * 3)
    for at in (0x4b693f0, 0x4bb63e4, 0x4bb8474, 0x4bb91c0, 0x4bb98b8):
        b.site(p, at, f32(1.0), f32(0.5))
    b.site(p, 0x4cc0024, f32(1.0), f32(10.0))
    b.site(p, 0x486aab4, bytes(4), f32(1.0))
    b.site(p, 0x486aabc, h('00000400'), f32(1.0))
    return p


def air_drag(b, p):
    b.site(p, AIR_DRAG, rip_op(h('c5f85905'), AIR_DRAG, 0x4d0b5b0), jmp32(AIR_DRAG, AIR_DRAG_CODE) + nop(3))
    c = AIR_DRAG_CODE
    code = h('c5fa109578f6ffff')
    code += rip_op(h('c5ea5915'), c + len(code), AIR_DRAG_DATA)
    code += rip_op(h('c5ea591d'), c + len(code), AIR_DRAG_DATA + 4)
    code += rip_op(h('c5e2581d'), c + len(code), AIR_DRAG_DATA + 8)
    code += h('c5e259da')
    code += rip_op(h('c5e25c1d'), c + len(code), AIR_DRAG_DATA + 12)
    code += h('c5e259da')
    code += rip_op(h('c5e2581d'), c + len(code), AIR_DRAG_DATA + 12)
    code += h('c5e0c6db00c5f859c3')
    code += jmp32(c + len(code), AIR_DRAG + 8)
    spare(b, p, c, code)
    spare(b, p, AIR_DRAG_DATA, f32(-30 * math.log(0.95), -1 / 6, 0.5, 1.0))


def cloth_stiffness(b, p):
    for store, profile, slot in CLOTH_CONSTRAINTS:
        was = h('c5f82945') + bytes([slot])
        cave = profile + 2
        b.site(p, store, was, jmp32(store, cave))
        b.site(p, profile, h('7317'), h('eb17'))
        code = rip_op(h('c5fa5905'), cave, CLOTH_SCALE) + was + jmp32(cave + 13, store + 5)
        b.same(p, cave, code + b'\xcc' * (23 - len(code)))


def reflections(b):
    p = Patch('Enable Screen Space Reflections (READ NOTE)', 'Screen-space reflections on wet and shiny surfaces, '
                                                             'a pass the renderer has but the game never turns on. '
                                                             'Slower; not in the original game.')
    b.site(p, 0x26c4fd0, h('41c6857729000001'), h('41c6857429000001'))
    b.site(p, 0x26c3d2f, h('7453'), nop(2))
    b.site(p, 0x26c428d, h('d1eed1ea'), nop(4))
    b.site(p, 0x4d29c8c, f32(-1.0, 0.01), f32(-100.0, 1.0))
    b.site(p, 0x26b8d80, h('c5f828d9'), h('c5f828d8'))
    spare(b, p, SSR_TWO, f32(2.0))
    b.retarget(p, 0x12a99dd, 8, 0x4ca91a4, SSR_TWO)
    return p


class Asm:
    def __init__(self, at):
        self.at, self.parts, self.labels = at, [], {}

    def __call__(self, text):
        self.parts.append(h(text))
        return self

    def label(self, name):
        self.parts.append(name)
        return self

    def branch(self, opcode, target):
        self.parts.append((h(opcode), target))
        return self

    @staticmethod
    def size(part):
        if isinstance(part, bytes):
            return len(part)
        if isinstance(part, str):
            return 0
        return len(part[0]) + 4

    def assemble(self):
        at = self.at
        for part in self.parts:
            if isinstance(part, str):
                self.labels[part] = at
            at += self.size(part)
        out, at = b'', self.at
        for part in self.parts:
            if isinstance(part, bytes):
                out += part
            elif isinstance(part, tuple):
                out += part[0] + rel(at, self.size(part), self.labels.get(part[1], part[1]))
            at += self.size(part)
        return out


def free_camera(b):
    p = Patch('Restore Debug Camera', "The game's own debug camera: hold Cross and press L3 to fly it from where "
                                      'the camera is, again to keep it still while the hunter moves, again to go '
                                      'back.')
    a = Asm(FREE_CAMERA)
    a.label('found')('c6413001')('488b4128')
    a.label('common')('4889c3')('4889df')('be08000000').branch('e8', 0x1391c60)('84c0').branch('0f84', 'check')
    a('4180bc24c400000001').branch('0f84', 'on')
    a('41c68424c400000001')('41c6855001000001').branch('e9', 'copy')
    a.label('on')('4180bd5001000000').branch('0f84', 'off')
    a('41c6855001000000')('41c68424c800000001').branch('e9', 'copy')
    a.label('off')('49c78424c400000000000000')
    a.label('copy')('498b8c24d0000000')('4885c9').branch('0f84', 'check')
    a('41807c244800').branch('0f84', 'game')
    a('c4c17828442450')('c5f8294110')('c4c17828442460')('c5f8294120')('c4c17828442470')('c5f8294130')
    a('c4c17828842480000000')('c5f8294140')('c4c17a1044244c')('c5fa114150').branch('e9', 'check')
    a.label('game')('498b542410')('4885d2').branch('0f84', 'check')
    for field in ('50', '54', '58', '5c'):
        a('c5fa1042' + field)('c5fa1141' + field)
    a('c5f8284210')('c5f8284a20')('c5f8285230')('c5f8285a40')
    a('c5f8294110')('c5f8294920')('c5f8295130')('c5f8295940')
    a.label('check')('4180bc24c400000001').branch('0f85', 'held')('4180bc24c800000000').branch('0f85', 'held')
    a('41c6855001000001')
    a.label('held')('4889df')('be41000000').branch('e8', 0x1391c60)('84c0').branch('0f84', 'flag')
    a('41c6855001000000')
    a.label('flag')('4180bd5001000000')('410f958551010000')
    a('4183bc24c400000001').branch('e9', 0x19729bf)
    code = a.assemble()
    spare(b, p, FREE_CAMERA, code)
    b.site(p, 0x19729ab, h('c6413001eb05'), jmp32(0x19729ab, a.labels['found']) + nop(1))
    b.site(p, 0x19729b6, h('4183bc24c400000001'), jmp32(0x19729b6, a.labels['common']) + nop(4))
    return p


def sprint(b):
    p = Patch('Sprint Fix (High FPS)', "Sprinting keeps its speed above 60 FPS: the game's check for walking into "
                                       'a wall measured distance per frame (a 30 FPS rule); it divides by the '
                                       "frame's time instead. Goes with the 90, 120 and 144 FPS settings.")
    b.site(p, 0x1914c24, h('c5fa590500174103'), h('c5fa5e458c') + nop(3))
    return p


def text_scale(b):
    p = Patch('50% Text scale', "The game's text at half its size, for high output resolutions. The scale is the "
                                'float written at 0x4D2913C.')
    b.site(p, 0x4d2913c, f32(1.0), f32(0.5))
    return p


def analog_run(b):
    p = Patch('Sensitive Analog Input (easier to run)', 'The left stick reaches running speed at about 80% of its '
                                                        'travel instead of nearly all of it.')
    b.site(p, 0x4d27714, f32(1.0), f32(1.2))
    return p


def no_rally_decay(b):
    p = Patch('No Rally Decay', 'HP lost to a hit stays recoverable by Rally until you strike back: the Rally '
                                'window and its decay rates are turned off.')
    b.site(p, 0x5527b0c, f32(0.25, 3.0, 1.0, 0.6), f32(0.0, 0.0, 0.0, 10.0))
    b.site(p, 0x5952d74, bytes(2), h('0101'))
    return p


def no_rally(b):
    p = Patch('Disable Rally (HP Regain)', 'Striking back regains no HP.')
    b.site(p, 0x1966aec, h('4188c6'), h('41b601'))
    return p


def debug_flag(b, name, note, at, value):
    p = Patch(name, note)
    b.site(p, at, bytes(len(value)), value)
    return p


def debug_flags(b):
    return [debug_flag(b, 'Player No Dead (Read note)', "The game's debug flag: the hunter takes damage but HP never "
                                                         'drops below 1.', 0x593e888, u32(1)),
            debug_flag(b, 'Player Stealth (Read note)', "The game's debug flag: enemies do not see you or react to you "
                                                        'unless you attack them.', 0x593e88e, h('01')),
            debug_flag(b, 'Player Silent (Read note)', "The game's debug flag: enemies do not hear you but can still "
                                                       'see you.', 0x593e88f, h('01'))]


def corpse_physics(b, name, note, weight, flags):
    p = Patch(name, note)
    b.site(p, 0x1bf9439, f32(5.0), f32(weight))
    b.site(p, 0x1bf9463, u32(0x101), u32(flags))
    return p


def physics(b):
    return [corpse_physics(b, 'DS1-like physics', 'Corpses and loose objects fly when you hit or run into them, as '
                                                  'in Dark Souls.', 9999.0, 0x1),
            corpse_physics(b, 'Lower object/corpse physics', 'Corpses and loose objects barely move when hit or run '
                                                             'into.', 0.5, 0x1010101)]


def light_grid(b, name, w, height):
    p = Patch(name, f"The light grid's tiles at {w}x{height}, a single tile for a window of that size: far fewer "
                    'light draw calls. Use the one that matches your window size, not the render size.')
    b.site(p, 0x2695cbc, u32(0x80), u32(w))
    b.site(p, 0x2695cc6, u32(0x80), u32(height))
    return p


def light_grids(b):
    return [light_grid(b, '1280x800 Light Grid For SteamDeck (READ NOTES)', 1280, 800),
            light_grid(b, '1080p Light Grid (READ NOTES)', 1920, 1080),
            light_grid(b, '1440p Light Grid (READ NOTES)', 2560, 1440),
            light_grid(b, '4k Light Grid (READ NOTES)', 3840, 2160)]


def capture_folder(b):
    p = Patch('Bookmark and Capture outputs', "Screenshots and bookmarks the game writes go to /data/bloodborne/ "
                                              'instead of the PS4 capture folder.')
    was = 'system:/../capture'.encode('utf-16-le')
    b.site(p, 0x4dba9be, was, utf16('/data/bloodborne/', len(was)))
    return p


def no_http(b):
    p = Patch('Disable HTTP Requests', "The game's web requests are not made: the two calls that start them, and "
                                       'the date lookup that stamps them.')
    for at, target in ((0x227e2f0, 0x13d10d0), (0x23e4d96, 0x13d10d0), (0x24ee2a9, 0x2fc0db8)):
        b.site(p, at, call(at, target), nop(5))
    return p


def fmod_fix(b):
    p = Patch('FMOD Crash Fix', "FMOD's channel setup clears the field at +0x170 it left uninitialized, and the "
                                'channel index is checked as the 16-bit value it is. May leave a sound unplayed.')
    at = 0x1122534
    code = h('4989c8') + h('498d4d08') + h('6681bdc8fcffff') + struct.pack('<H', 0x1ffe)
    code += b'\x77' + bytes([(0x11225c4 - (at + len(code) + 2)) & 0xff]) + h('41898084000000')
    b.site(p, at, h('3dff1f00000f87850000004989c841898084000000498d4d08'), code)
    b.site(p, 0x11f3e4d, u32(0x178), u32(0x170))
    b.site(p, 0x11f3eab, u32(0x110), u32(0x118))
    return p


def camera_distance(b):
    p = Patch('Increased camera distance', 'The follow camera sits further back from the hunter.')
    b.site(p, 0x1836940, h('8a480d'), h('30c9') + nop(1))
    b.site(p, 0x5527a94, h('01'), h('00'))
    b.site(p, 0x184107f, h('c7042400000000'), h('c74304') + u32(10002))
    b.site(p, 0x18410b1, h('c7042400000000'), h('c74308') + u32(10002))
    return p


def enemy_control(b):
    p = Patch('Enemy Control', 'Lock on to an enemy and press R3 to control it; L3 gives control back. Not with the '
                               'debug camera.')
    take = Asm(ENEMY_CAVES)
    take('50515256574150415141524153')('4883ec08')('c680880000001d')('4889c7')('31f6').branch('e8', 0x1ccde10)
    take('4883c408')('415b415a415941585f5e5a5958')('83787807')('0f94c0').branch('e9', 0x1c90ef8)
    code = take.assemble()
    spare(b, p, ENEMY_CAVES, code)
    b.site(p, 0x1c90ef1, h('837878070f94c0'), jmp32(0x1c90ef1, ENEMY_CAVES) + nop(2))
    back = Asm(ENEMY_CAVES + 0x80)
    back.label('found')('c6413001')('488b4128')
    back.label('common')('4889c7')('be17000000').branch('e8', 0x1391c60)('84c0').branch('0f84', 'done')
    back('31ff')('be28000000').branch('e8', 0x1ccde10)
    back.label('done')('4183bc24c400000001').branch('e9', 0x19729bf)
    code = back.assemble()
    spare(b, p, ENEMY_CAVES + 0x80, code)
    b.site(p, 0x19729ab, h('c6413001eb05'), jmp32(0x19729ab, back.labels['found']) + nop(1))
    b.site(p, 0x19729b6, h('4183bc24c400000001'), jmp32(0x19729b6, back.labels['common']) + nop(4))
    return p


def messenger_skins(b):
    p = Patch('Enable Extra Messenger Skins', "The game treats its hidden pre-order messenger skins as installed and "
                                              "offers them in the Hunter's Dream: they are in the game's own files, "
                                              'only the licence flags kept them out.',
              author='distpade (NexusMods)')
    b.site(p, 0x23b67b3, h('49c7460800000000'), h('49c7460801010101'))
    return p


RESOLUTIONS = ((640, 360, '16:9'), (960, 540, '16:9'), (1440, 810, '16:9'), (1600, 900, '16:9'),
               (2560, 1440, '16:9'), (3840, 2160, '16:9'), (1280, 800, '16:10'), (1920, 1200, '16:10'),
               (2560, 1600, '16:10'), (2560, 1080, '21:9'), (3440, 1440, '21:9'), (5120, 2160, '21:9'),
               (3840, 1080, '32:9'), (5120, 1440, '32:9'), (1280, 960, '4:3'), (1440, 1080, '4:3'))
HUD_LIMITS = (0x1ffd473, 0x1ffd4cf, 0x1a54dd9, 0x1a54e1a)
HUD_SIZE = 0x4d26ea4
HUD_SCALES = (0x4cf9a10, 0x4cf9a70, 0x4cf9ae0)


HUD_SHAPES = {'16:9': (1920, 1080), '16:10': (1920, 1200), '21:9': (2560, 1080), '32:9': (3840, 1080),
              '4:3': (1920, 1440)}


def resolution_patch(b, name, note, w, height, shape):
    p = Patch(name, note)
    hud_w, hud_h = HUD_SHAPES[shape]
    for at, source, opcode in SCENE_SITES + UI_SITES:
        size_load(b, p, at, source, opcode, w if source == SCREEN_W else height, True)
    for at, source, opcode in HUD_SITES:
        size_load(b, p, at, source, opcode, hud_w if source in (SCREEN_W, RENDER_W) else hud_h, False)
    b.site(p, SCREEN_W, u32(1920) + u32(1080), u32(640) + u32(360))
    if (hud_w, hud_h) != (1920, 1080):
        b.site(p, 0x183a35d, f32(16 / 9), f32(w / height))
        for at in HUD_LIMITS:
            b.site(p, at, b'\x77' + b.eboot.read(at + 1, 1), nop(2))
        if shape != '21:9':
            b.site(p, 0x1a4140a, h('c5f258cc'), nop(4))
        b.site(p, HUD_SIZE, f32(1920.0, 1080.0), f32(hud_w, hud_h))
        for at in HUD_SCALES:
            b.site(p, at, f32(0.5, 0.5), f32(0.5 * 1920 / hud_w, 0.5 * 1080 / hud_h))
    if w * height > 1920 * 1080:
        b.site(p, 0x4b36dd8, struct.pack('<Q', 0x8ef00000), struct.pack('<Q', 0xf7000000))
        b.site(p, 0x4b36de0, struct.pack('<Q', 0x37600000), struct.pack('<Q', 0xa0000000))
    if (w, height) == (1920, 1200):
        for at in (0x1a42531, 0x1a4253c, 0x1a42546):
            b.site(p, at, b.eboot.read(at, 2), nop(2))
        b.site(p, 0x1a44225, h('0f8689010000'), nop(6))
        b.site(p, 0x2417862, h('488b7b084c89f6e892410d0045892f41890424'),
               h('41bd') + u32(1920) + b'\xb8' + u32(1200) + h('45892f41890424') + nop(1))
    return p


def resolutions(b):
    out = []
    for w, height, shape in RESOLUTIONS:
        note = (f'The scene and UI at {w}x{height} ({shape}), the HUD, lock-on and health bars placed for that '
                'shape.' + (' Needs the larger memory pool.' if w * height > 1920 * 1080 else ''))
        out.append(resolution_patch(b, f'Resolution Patch {w}x{height} ({shape})', note, w, height, shape))
    return out


def optimal_1080p(b):
    return resolution_patch(b, 'Optimal 1080p', 'The scene, UI and HUD at 1920x1080 with the screen size the rest '
                                                'of the game sizes its buffers from at 640x360.', 1920, 1080,
                            '16:9')


def thirty(b):
    p = Patch('30 FPS++', "The game at its own 30 frames a second with the frame timer's frame skip, sync and "
                          'buffering settings of the 60 FPS mode and each step taking the measured frame time: '
                          'lower input delay, the same speed.')
    frame_timer(b, p, 30)
    frame_time_cave(b, p, 30)
    frame_count_cave(b, p)
    frame_limiter(b, p, 30)
    b.site(p, 0xfbc40f, h('7f1d'), h('eb1d'))
    for at, length, constant in FRAME_TIME_LOADS:
        b.retarget(p, at, length, constant, FRAME_TIME)
    air_drag(b, p)
    return p


def no_delta_time(b):
    p = Patch('60FPS (no deltatime)', 'The game at 60 frames a second with a fixed step of 1/60 s instead of the '
                                      'measured frame time: cloth and corpses never jump after a stutter, but the '
                                      'game slows down below 60 FPS.')
    frame_timer(b, p, 60)
    frame_limiter(b, p, 60)
    b.site(p, 0x25629f0, b'\xbf' + u32(33000), b'\xbf' + u32(990000 // 60))
    b.site(p, 0xfbc40f, h('7f1d'), h('eb1d'))
    task_steps(b, p)
    for at in STEP_DATA:
        b.site(p, at, ONE_THIRTIETH, f32(1 / 60))
    for at in STEP_THIRTY_CODE + STEP_THIRTY_DATA:
        b.site(p, at, THIRTY, f32(60.0))
    for at, length, constant in FRAME_TIME_LOADS:
        b.retarget(p, at, length, constant, FRAME_TIME)
    b.site(p, FRAME_TIME, bytes(4), f32(1 / 60))
    b.site(p, 0x1929676, u32(5), u32(10))
    b.site(p, 0x192c405, u32(10), u32(20))
    b.site(p, 0x4cf6420, f32(0.1, 0.1, 0.1, 0.1), f32(*[0.05] * 4))
    b.site(p, 0x4d27fd4, f32(-0.1), f32(-0.05))
    b.site(p, 0x4d27fe0, f32(0.1), f32(0.05))
    b.site(p, 0x4d25e68, f32(0.1), f32(1 - 0.9 ** 0.5))
    b.site(p, 0x4d25e94, ONE_THIRTIETH, f32(1 - (1 - 1 / 30) ** 0.5))
    b.site(p, 0xb6e67a, h('8b581885db7504418b5d20'), b'\xbb' + u32(1) + nop(6))
    air_drag(b, p)
    return p


def sleep_cap(at, cap, usleep):
    return h('85ff7e1281ff') + u32(cap) + h('7e05bf') + u32(cap) + jmp32(at + 17, usleep) + h('c3')


def performance(b):
    p = Patch('Performance Patch (perf increase)', 'Lighter settings for the renderer and its debug parameters, '
                                                   'shorter waits in worker threads, sleeps capped at a few '
                                                   'milliseconds and profiling skipped. Changes some visuals.')
    for at, value in PERF_DATA:
        b.same(p, at, h(value))
    for at, length in PERF_NOPS:
        b.same(p, at, nop(length))
    for at in PERF_ALWAYS:
        was = b.eboot.read(at, 1)
        if was[0] not in (0x74, 0x75, 0x7e):
            raise ValueError(f'{p.name}: {at:#x} is not a short conditional jump')
        b.site(p, at, was, h('eb'))
    for at, target in PERF_SKIPS + ((0x2c0118a, 0x2c011da), (0x2c011eb, 0x2c01237), (0x2c012ca, 0x2c01322)):
        b.site(p, at, h('31f6'), jmp(at, target))
    for at, value in PERF_RETURNS:
        b.site(p, at, h('554889'), (h('31c0') if value == 0 else h('b001')) + h('c3'))
    for at, was, now in PERF_EDITS:
        b.site(p, at, h(was), h(now))
    caves = PERF_CAVES
    usleep = 0x2fbfe68
    for at, cap in ((0x248bac0, 8000), (0x247f980, 8000)):
        if b.eboot.disp_target(at + 4, 5) != usleep:
            raise ValueError(f'{p.name}: {at:#x} does not sleep')
        code = sleep_cap(caves, cap, usleep)
        spare(b, p, caves, code)
        b.site(p, at, b.eboot.read(at, 10), jmp32(at, caves) + b'\xcc' * 5)
        caves += 0x20
    spare(b, p, caves, sleep_cap(caves, 4000, usleep))
    b.site(p, 0x2c0137d, call(0x2c0137d, 0x248bac0), call(0x2c0137d, caves))
    caves += 0x20
    wrapper = h('52c60201') + call(caves + 4, 0x2d2f830) + h('5ac60201c3')
    spare(b, p, caves, wrapper)
    for at in (0x2587d8c, 0x2587dbb):
        b.site(p, at, call(at, 0x2d2f830), call(at, caves))
    return p


def build(eboot):
    b = Builder(eboot)
    patches = [skip_intro(b), performance(b)] + effects(b) + [text_scale(b), fmod_fix(b)] + model_detail(b) + [
        graphics_heaps(b), intel_tonemap(b), sfx_ribbon(b), no_http(b)] + physics(b) + [
        thirty(b), no_delta_time(b), frame_rate(b, 60), frame_rate(b, 90), uncapped(b), debug_menu(b),
        camera_auto_turn(b), camera_distance(b), no_rally_decay(b), no_rally(b)] + debug_flags(b) + [
        analog_run(b), region(b), all_languages(b), capture_folder(b), enemy_control(b), messenger_skins(b)] + light_grids(b) + [
        optimal_1080p(b), resolution(b)] + resolutions(b) + [free_camera(b), sprint(b), reflections(b)]
    seen = {}
    for patch in patches:
        if patch.name in seen:
            raise ValueError(f'{patch.name}: twice')
        seen[patch.name] = patch
        spans = sorted((at, at + len(was)) for at, was, _ in patch.writes)
        for (_, end), (start, _) in zip(spans, spans[1:]):
            if start < end:
                raise ValueError(f'{patch.name}: writes overlap at {start:#x}')
    return patches


def render(patches):
    lines = ['<?xml version="1.0"?>', '<Patch>', '    <TitleID>']
    lines += [f'        <ID>{i}</ID>' for i in TITLE_IDS]
    lines.append('    </TitleID>')
    for patch in patches:
        lines.append(f'    <Metadata Title="Bloodborne" Name="{escape(patch.name, {chr(34): "&quot;"})}" '
                     f'Note="{escape(patch.note, {chr(34): "&quot;"})}" Author="{patch.author or AUTHOR}" PatchVer="1.0" '
                     'AppVer="01.09" AppElf="eboot.bin" isEnabled="false">')
        lines.append('        <PatchList>')
        for at, _, now in patch.writes:
            lines.append(f'            <Line Type="bytes" Address="{at:#010x}" Value="{now.hex()}"/>')
        lines.append('        </PatchList>')
        lines.append('    </Metadata>')
    lines.append('</Patch>')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description="Builds Lantern Engine's built-in game patches "
                                                 '(patches/Bloodborne.xml) from the 1.09 eboot, checking every '
                                                 "site's original bytes.")
    parser.add_argument('eboot', type=Path, help='the prepared 1.09 eboot.elf')
    parser.add_argument('--write', action='store_true', help='write patches/Bloodborne.xml')
    parser.add_argument('--xml', type=Path, default=XML)
    parser.add_argument('--list', action='store_true', help='print every write with its original bytes')
    args = parser.parse_args()
    patches = build(Eboot(args.eboot))
    if args.list:
        for patch in patches:
            print(f'{patch.name} ({len(patch.writes)} writes)')
            for at, was, now in patch.writes:
                print(f'  {at:#x}: {was.hex()} -> {now.hex()}')
    text = render(patches)
    if args.write:
        args.xml.write_text(text, encoding='utf-8', newline='\n')
        print(f'{args.xml}: {len(patches)} patches, {sum(len(p.writes) for p in patches)} writes')
        return 0
    if args.xml.read_bytes().decode('utf-8').replace('\r\n', '\n') != text:
        print(f'{args.xml} differs from what the eboot builds; run with --write', file=sys.stderr)
        return 1
    print(f'{args.xml}: {len(patches)} patches, every site checked')
    return 0


if __name__ == '__main__':
    sys.exit(main())
