from paths import ROOT
import contextlib
import io
import json
import sys
import subprocess
import os
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import patches

from patches import (DEFAULT_OFF, EBOOT_BASE, OUTPUT_SIZE, RESOLUTION_TEMPLATE, SCENE_HEIGHT,
                     SCENE_WIDTH, UI_HEIGHT, UI_WIDTH, compile_patches,
                     render_size, resolution_writes, scaled_sizes, effect_patches,
                     validate_patch_requirements, external_patches, external_selection,
                     compile_external)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]


def immediate(writes, address):
    data = bytearray(9)
    for offset, value in writes:
        if address <= offset < address + 9:
            data[offset-address:offset-address+len(value)] = value
    assert data[5:] == b'\x0f\x1f\x40\x00'
    return data[0], struct.unpack_from('<I', data, 1)[0]


class NativeUiTests(unittest.TestCase):
    def test_presets_keep_ui_native(self):
        for preset, expected in [(1, (1280, 720)), (2, (1130, 636)),
                                 (3, (960, 540)), (4, (640, 360))]:
            with self.subTest(preset=preset):
                size = render_size({'upscaler': 'fsr3', 'preset': str(preset)})
                self.assertEqual(size, expected)
                writes = resolution_writes(XML, size, '01.09', SEGMENTS)
                self.assertEqual(immediate(writes, SCENE_WIDTH), (0xB8, size[0]))
                self.assertEqual(immediate(writes, SCENE_HEIGHT), (0xB8, size[1]))
                self.assertEqual(immediate(writes, UI_WIDTH), (0xB8, OUTPUT_SIZE[0]))
                self.assertEqual(immediate(writes, UI_HEIGHT), (0xB9, OUTPUT_SIZE[1]))

    def test_explicit_scene_resolution_keeps_native_ui(self):
        writes = resolution_writes(XML, (800, 450), '01.09', SEGMENTS)
        self.assertEqual(immediate(writes, SCENE_WIDTH)[1], 800)
        self.assertEqual(immediate(writes, UI_WIDTH)[1], 1920)
        self.assertEqual(immediate(writes, UI_HEIGHT)[1], 1080)

    def test_coordinate_and_aspect_fixes_are_preserved(self):
        original = compile_patches(XML, [RESOLUTION_TEMPLATE], '01.09', SEGMENTS)
        modified = resolution_writes(XML, (960, 540), '01.09', SEGMENTS)
        self.assertEqual(len(original), len(modified))
        changed = {SCENE_WIDTH, SCENE_HEIGHT, UI_WIDTH, UI_HEIGHT}
        self.assertEqual([w for w in original if w[0] not in changed],
                         [w for w in modified if w[0] not in changed])

    def test_unexpected_or_missing_ui_instruction_is_rejected(self):
        for remove in (False, True):
            tree = ET.parse(XML)
            for meta in tree.getroot().iter('Metadata'):
                if meta.get('Name') == RESOLUTION_TEMPLATE and meta.get('AppVer') == '01.09':
                    patch_list = meta.find('PatchList')
                    for line in list(patch_list):
                        if int(line.get('Address'), 0) == UI_WIDTH + EBOOT_BASE:
                            if remove:
                                patch_list.remove(line)
                            else:
                                line.set('Value', '0x000500B9')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'patch.xml'
                tree.write(path)
                with self.assertRaises(ValueError):
                    resolution_writes(path, (960, 540), '01.09', SEGMENTS)

    def test_native_and_disabled_upscaler_need_no_resolution_patch(self):
        self.assertIsNone(render_size({'preset': '0'}))
        self.assertIsNone(render_size({'upscaler': 'off', 'preset': '3'}))

    def test_output_other_than_1080p_scales_the_scene(self):
        self.assertIsNone(scaled_sizes({'output_res': '1920x1080', 'preset': '2'}))
        for preset, expected in [(0, (1280, 720)), (2, (752, 424)), (4, (640, 360))]:
            with self.subTest(preset=preset):
                self.assertEqual(scaled_sizes({'output_res': '1280x720', 'preset': str(preset)}),
                                 (expected, (1280, 720)))
        self.assertEqual(scaled_sizes({'output_res': '3840x2160', 'preset': '3'}),
                         ((1916, 1076), (3840, 2160)))
        self.assertIsNone(scaled_sizes({'output_res': '1280x720', 'upscaler': 'taa', 'preset': '3'}))


class DebugPatchTests(unittest.TestCase):
    def test_debug_menu_is_optional_and_compatible_with_the_frame_rate_patches(self):
        on = {key: '1' for key in DEFAULT_OFF}
        self.assertEqual(effect_patches({'debug_menu': '0', **on}), [])
        menu = compile_patches(XML, effect_patches({'debug_menu': '1'}), '01.09', SEGMENTS)
        self.assertGreater(len(menu), 0)
        menu_bytes = {offset+i: byte for offset, data in menu for i, byte in enumerate(data)}
        for patch in ('60 FPS++', '90 FPS++', 'Uncap FPS++', 'Sprint Fix (High FPS)', 'Skip Intro',
                      'Restore Debug Camera', 'Enable Screen Space Reflections (READ NOTE)'):
            for offset, data in compile_patches(XML, [patch], '01.09', SEGMENTS):
                for i, byte in enumerate(data):
                    self.assertNotIn(offset+i, menu_bytes, patch)

    def test_camera_patch_is_optional_and_compatible_with_fps_and_debug_menu(self):
        camera = compile_patches(XML, effect_patches({'debug_camera': '1'}), '01.09', SEGMENTS)
        self.assertGreater(len(camera), 0)
        camera_bytes = {offset+i for offset, data in camera for i in range(len(data))}
        for patch in ('Uncap FPS++', '60 FPS++', '90 FPS++', 'Restore Debug Menu (READ NOTES)',
                      'Enable Screen Space Reflections (READ NOTE)'):
            for offset, data in compile_patches(XML, [patch], '01.09', SEGMENTS):
                for i in range(len(data)):
                    self.assertNotIn(offset+i, camera_bytes, patch)

    def test_missing_effect_keys_follow_the_engine_defaults(self):
        self.assertEqual(effect_patches({}), ['Disable Chromatic Aberration', 'Disable DoF',
                                              'Disable Motion Blur (perf increase)', 'Disable AA'])
        self.assertNotIn('Disable DoF', effect_patches({'effect_dof': '1'}))

    def test_debug_menu_checks_both_fonts_but_camera_does_not_need_them(self):
        names = effect_patches({'debug_menu': '1'})
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            validate_patch_requirements(['Restore Debug Camera'], game)
            with self.assertRaisesRegex(ValueError, 'DbgFont14h.ccm.*DbgFont14h.tpf'):
                validate_patch_requirements(names, game)
            font = game / 'dvdroot_ps4/font'
            font.mkdir(parents=True)
            (font / 'DbgFont14h.ccm').write_bytes(b'test')
            (font / 'DbgFont14h.tpf').touch()
            with self.assertRaisesRegex(ValueError, 'DbgFont14h.tpf'):
                validate_patch_requirements(names, game)
            (font / 'DbgFont14h.tpf').write_bytes(b'test')
            validate_patch_requirements(names, game)

    def test_conflicting_enemy_control_patch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'conflicts with Enemy Control'):
            validate_patch_requirements(['Enemy Control', 'Restore Debug Camera'], Path('.'))


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA03173</ID><ID>CUSA00207</ID></TitleID>
  <Metadata Title="Bloodborne" Name="On" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Off" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA03173', 'CUSA99999')
                                              .replace('CUSA00207', 'CUSA99998'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(XML.parent), [])

    def test_1_09_is_01_09(self):
        self.assertTrue(patches.same_app_version('1.09', '01.09'))
        self.assertTrue(patches.same_app_version(' 01.09 ', '01.09'))
        self.assertFalse(patches.same_app_version('1.00', '01.09'))
        self.assertFalse(patches.same_app_version(None, '01.09'))
        self.assertFalse(patches.same_app_version('v1.09', '01.09'))


class IntelTonemapTests(unittest.TestCase):
    def cpuinfo(self, vendor):
        path = Path(tempfile.mkdtemp()) / 'cpuinfo'
        path.write_text(f'processor\t: 0\nvendor_id\t: {vendor}\nmodel name\t: x\n')
        return str(path)

    def test_on_for_intel_off_for_amd(self):
        self.assertTrue(patches.intel_tonemap_fix({}, self.cpuinfo('GenuineIntel')))
        self.assertFalse(patches.intel_tonemap_fix({}, self.cpuinfo('AuthenticAMD')))

    def test_environment_forces_it(self):
        amd, intel = self.cpuinfo('AuthenticAMD'), self.cpuinfo('GenuineIntel')
        self.assertTrue(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '1'}, amd))
        self.assertFalse(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '0'}, intel))

    def test_patch_exists_for_109(self):
        xml = ET.parse(ROOT / 'patches/Bloodborne.xml')
        names = [m.get('Name') for m in xml.iter('Metadata')]
        self.assertIn(patches.INTEL_TONEMAP, names)


def fake_elf(code):
    """A one-segment ELF: `code` as file data, then as much zero memory (bss)."""
    phoff = 64
    elf = bytearray(phoff + 56) + bytes(code)
    struct.pack_into('<Q', elf, 0x20, phoff)
    struct.pack_into('<HH', elf, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', elf, phoff, 1, 5, phoff + 56, 0, 0, len(code), 2 * len(code), 0x4000)
    return bytes(elf)


class SiteCheckTests(unittest.TestCase):
    XML = """<Patch>
  <Metadata Name="Fast A" AppVer="01.09"><PatchList><Line Type="bytes" Address="0x400010" Value="9090"/></PatchList></Metadata>
  <Metadata Name="Fast B" AppVer="01.09"><PatchList><Line Type="bytes" Address="0x400090" Value="01"/></PatchList></Metadata>
  <Metadata Name="Other" AppVer="01.09"><PatchList><Line Type="bytes" Address="0x400020" Value="eb"/></PatchList></Metadata>
</Patch>"""

    def test_a_differing_patch_and_its_group_are_left_out(self):
        code = bytes(range(128))
        elf = fake_elf(code)
        segments = patches.eboot_segments(elf)
        with tempfile.TemporaryDirectory() as directory:
            xml = Path(directory) / 'Bloodborne.xml'
            xml.write_text(self.XML)
            table = {name: patches.site_crcs(elf, patches.compile_patches(xml, [name], '01.09', segments))
                     for name in ('Fast A', 'Fast B', 'Other')}
            patches.site_table_path(xml).write_text(json.dumps(table))
            names = ['Fast A', 'Fast B', 'Other']
            self.assertEqual(patches.checked_patches(xml, names, '01.09', segments, elf, ('Fast A', 'Fast B')), names)
            changed = bytearray(elf)
            changed[64 + 56 + 0x11] ^= 0xff
            self.assertEqual(patches.checked_patches(xml, names, '01.09', segments, bytes(changed), ('Fast A', 'Fast B')),
                             ['Other'])
            self.assertEqual(patches.checked_patches(xml, names, '01.09', segments, bytes(changed)), ['Fast B', 'Other'])

    def test_bss_sites_read_as_zero_and_no_table_checks_nothing(self):
        elf = fake_elf(bytes([0x11]) * 16)
        self.assertEqual(patches.original_bytes(elf, 14, 4), bytes([0x11, 0x11, 0, 0]))
        with tempfile.TemporaryDirectory() as directory:
            xml = Path(directory) / 'Bloodborne.xml'
            self.assertEqual(patches.checked_patches(xml, ['Other'], '01.09', [], elf), ['Other'])

    def test_shipped_table_covers_every_built_in_patch(self):
        xml = ROOT / 'patches/Bloodborne.xml'
        table = json.loads(patches.site_table_path(xml).read_text(encoding='utf-8'))
        names = [meta.get('Name') for meta in ET.parse(xml).getroot().iter('Metadata')]
        self.assertEqual(sorted(table), sorted(names))
        for name in names:
            writes = patches.compile_patches(xml, [name], '01.09', [(0, 1 << 40)])
            self.assertEqual(len(table[name]), len(writes), name)

    def test_every_patch_the_engine_names_is_built_in(self):
        names = {meta.get('Name') for meta in ET.parse(XML).getroot().iter('Metadata')}
        used = {name for preset in patches.FPS_PRESETS.values() for name in preset}
        used |= {name for off, on in patches.EFFECTS.values() for name in (off, on) if name}
        used |= set(patches.MODEL_LOD.values()) | {patches.SCALED_BASE, patches.INTEL_TONEMAP, patches.SFX_WORKAROUND,
                                                   patches.HEAP_PATCH, RESOLUTION_TEMPLATE}
        self.assertEqual(sorted(used - names), [])

    def test_the_verified_executable_matches_the_table(self):
        elf_path = ROOT / 'out/eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared eboot.elf')
        elf = elf_path.read_bytes()
        from engine_assets import EBOOT_SHA, sha
        if sha(elf) not in EBOOT_SHA:
            self.skipTest('out/eboot.elf is not the verified 1.09 executable')
        xml = ROOT / 'patches/Bloodborne.xml'
        names = list(json.loads(patches.site_table_path(xml).read_text(encoding='utf-8')))
        segments = patches.eboot_segments(elf)
        self.assertEqual(patches.checked_patches(xml, names, '01.09', segments, elf), names)


class ScaledRateTests(unittest.TestCase):
    WIDE = [(0, 1 << 40)]

    def test_the_formulas_rebuild_90_fps(self):
        writes = patches.compile_patches(XML, [patches.SCALED_BASE], '01.09', self.WIDE)
        self.assertEqual(patches.rate_scaled(writes, 90), writes)

    def test_120_and_144_values(self):
        for fps in (120, 144):
            with self.subTest(fps=fps):
                values = patches.scaled_rate_values(fps)
                self.assertEqual(values[0x2434840][5:], struct.pack('<f', 1 / fps))
                self.assertEqual(struct.unpack('<i', values[0x1929676])[0], fps // 6)
                self.assertEqual(struct.unpack('<I', values[0x25629F0][1:])[0], 990000 // fps)
                self.assertEqual(values[0x1CBDB42][6:10], struct.pack('<f', 0.0125 * 90 / fps))
                writes = patches.compile_patches(XML, [patches.SCALED_BASE], '01.09', self.WIDE)
                self.assertEqual(len(patches.rate_scaled(writes, fps)), len(writes))


CLOTH_SCALE, CLOTH_STIFFNESS = 0x575A90C, 0x4D3BD00
CLOTH_LEFT_OUT = ((0xB6FBD4, 0xB6FBD8), (0xB73F84, 0xB73F8D), (0xB73FA8, 0xB73FC1), (0x4D3BD10, 0x4D3BD20))


def f32(value):
    return struct.pack('<f', value)


def shaped_list():
    """A frame-rate list with every shape the corrections handle, as patch addresses."""
    return [
        (0x26FB39F, f32(60.0)), (0x4CAD7FC, f32(60.0)),
        (0xB6E67A, bytes.fromhex('bb01000000909090909090')),
        (0x183A4D7, f32(0.05)), (0x183A4E1, f32(0.05)), (0x183A649, f32(0.698132)),
        (0x183FF29, bytes.fromhex('41c7859c010000') + f32(0.05)), (0x183FF6B, bytes.fromhex('e90b010000')),
        (0x183DC96, bytes.fromhex('e9d6050000')), (0x183DCFE, b'\0'),
        (0x4D25E68, f32(0.025)), (0x4D25E94, f32(1 / 30)),
        (patches.STATUS_STEP_FIRST, bytes.fromhex('09e4a903')),
        (0x1000, b'\x90'),
    ]


class FrameRateCorrectionTests(unittest.TestCase):
    WIDE = [(0, 1 << 40)]

    def corrected(self, fps, writes=None, elf=None):
        writes = shaped_list() if writes is None else writes
        out = patches.corrected_frame_rate([(a - EBOOT_BASE, d) for a, d in writes], fps, elf)
        return [(o + EBOOT_BASE, d) for o, d in out]

    def weight(self, image, at):
        return struct.unpack('<f', image[at])[0]

    def camera_fix(self, on):
        previous = os.environ.pop('BB_CAMERA_FIX', None)
        if on:
            os.environ['BB_CAMERA_FIX'] = '1'
        self.addCleanup(lambda: os.environ.update(BB_CAMERA_FIX=previous) if previous is not None
                        else os.environ.pop('BB_CAMERA_FIX', None))

    def test_effect_data_keeps_its_30_a_second(self):
        for fps in (60, 90, 144, None):
            with self.subTest(fps=fps):
                self.assertFalse(set(patches.EFFECT_DATA_RATE) & dict(self.corrected(fps)).keys())

    def shipped(self, name, fps):
        writes = patches.compile_patches(XML, [name], '01.09', self.WIDE)
        if fps in (120, 144):
            writes = patches.rate_scaled(writes, fps)
        return dict((o + EBOOT_BASE, d) for o, d in patches.corrected_frame_rate(writes, fps))

    def test_cloth_stiffness_follows_the_frame_time(self):
        for name, fps in (('60 FPS++', 60), (patches.SCALED_BASE, 90), (patches.SCALED_BASE, 120),
                          (patches.SCALED_BASE, 144), ('Uncap FPS++', None)):
            with self.subTest(name=name, fps=fps):
                image = self.shipped(name, fps)
                self.assertIn(image[CLOTH_SCALE], (f32(1.0), f32((60 / 90) ** 2)))
                self.assertNotIn(CLOTH_STIFFNESS, image)
                self.assertFalse([at for at in image for lo, hi in CLOTH_LEFT_OUT if lo <= at < hi])
                self.assertEqual(image[0xB6E67A][:5], bytes.fromhex('bb01000000'))

    def test_the_camera_keeps_the_lists_values_without_the_switch(self):
        self.camera_fix(False)
        listed = dict(shaped_list())
        image = dict(self.corrected(90))
        for at in (0x183A4D7, 0x183FF29, 0x4D25E68, 0x183DC96, 0x183DCFE):
            self.assertEqual(image[at], listed[at])
        self.assertNotIn(patches.CAMERA_LOCK_BLEND[0], image)

    def test_the_switch_converts_the_cameras_weights_to_the_rate(self):
        self.camera_fix(True)
        at60 = dict(self.corrected(60))
        self.assertAlmostEqual(self.weight(at60, 0x183A4D7), 0.05)
        self.assertAlmostEqual(self.weight(at60, 0x4D25E68), 1 - 0.9 ** 0.5, places=6)
        self.assertAlmostEqual(self.weight(at60, 0x4D25E94), 1 - (1 - 1 / 30) ** 0.5, places=6)
        for fps in (90, 120, 144):
            with self.subTest(fps=fps):
                image = dict(self.corrected(fps))
                n = 60 / fps
                self.assertAlmostEqual(self.weight(image, 0x183A4D7), 1 - 0.95 ** n, places=6)
                self.assertAlmostEqual(self.weight(image, 0x183A4E1), 0.05 ** n, places=6)
                self.assertAlmostEqual(self.weight(image, 0x183A649), 0.698132, places=5)
                store = image[0x183FF29]
                self.assertEqual(store[:7], bytes.fromhex('41c7859c010000'))
                self.assertAlmostEqual(struct.unpack('<f', store[7:])[0], 1 - 0.95 ** n, places=6)
                self.assertEqual(image[0x183FF6B], bytes.fromhex('e90b010000'))
                self.assertAlmostEqual(self.weight(image, patches.CAMERA_LOCK_BLEND[0]), 1 - 0.95 ** (30 / fps),
                                       places=6)

    def test_per_frame_weights(self):
        self.assertAlmostEqual(patches.per_frame(0.3, 1), 0.3)
        self.assertAlmostEqual(patches.per_frame(0.5, 2), 0.75)
        self.assertAlmostEqual(patches.per_frame(0.5, 2, patches.CAMERA_PITCH_KEPT), 0.25)
        self.assertEqual(patches.per_frame(0.7, 2, patches.CAMERA_PITCH_ANGLE), 0.7)
        self.assertEqual(patches.per_frame(1.5, 2), 1.5)

    def test_uncapped_loses_only_the_effect_rate_and_gains_the_second_status_step(self):
        at, _game, frame_time = patches.STATUS_STEP_SECOND
        self.assertEqual(self.corrected(None),
                         [w for w in shaped_list() if w[0] not in patches.EFFECT_DATA_RATE] + [(at, frame_time)])

    def test_both_status_steps_read_the_frame_time(self):
        at, game, frame_time = patches.STATUS_STEP_SECOND
        first = (patches.STATUS_STEP_FIRST, bytes.fromhex('09e4a903'))
        self.assertEqual(self.corrected(120, [first]), [first, (at, frame_time)])
        self.assertEqual(self.corrected(60, []), [])
        code = bytearray(at - EBOOT_BASE + 4)
        self.assertEqual(self.corrected(60, [first], fake_elf(bytes(code))), [first])
        code[at - EBOOT_BASE:] = game
        self.assertEqual(self.corrected(60, [first], fake_elf(bytes(code))), [first, (at, frame_time)])

    def test_the_shipped_lists_come_out_as_the_corrections_require(self):
        names = {meta.get('Name') for meta in ET.parse(XML).getroot().iter('Metadata')}
        cases = [(name, fps) for name, fps in (('60 FPS++', 60), (patches.SCALED_BASE, 90), (patches.SCALED_BASE, 120),
                                               (patches.SCALED_BASE, 144), ('Uncap FPS++', None)) if name in names]
        self.assertTrue(cases)
        at, _game, frame_time = patches.STATUS_STEP_SECOND
        for name, fps in cases:
            with self.subTest(name=name, fps=fps):
                writes = patches.compile_patches(XML, [name], '01.09', self.WIDE)
                if fps in (120, 144):
                    writes = patches.rate_scaled(writes, fps)
                image = [(o + EBOOT_BASE, d) for o, d in patches.corrected_frame_rate(writes, fps)]
                keys = {a for a, _ in image}
                self.assertFalse(keys & set(patches.EFFECT_DATA_RATE))
                if patches.STATUS_STEP_FIRST in keys:
                    self.assertIn((at, frame_time), image)

    def test_30_runs_without_a_frame_rate_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            elf = ROOT / 'out/eboot.elf'
            if not elf.is_file():
                self.skipTest('no prepared eboot.elf')
            (out / 'eboot.elf').write_bytes(elf.read_bytes())
            result = subprocess.run([sys.executable, str(Path(patches.__file__)), '--out', str(out), '--fps', '30',
                                     '--settings', str(out / 'none.ini'), '--game-dir', str(out)],
                                    capture_output=True, text=True, env={**os.environ, 'BB_INTEL_TONEMAP_FIX': '0'})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('FPS++', result.stdout)


class OldHuntersTests(unittest.TestCase):
    def dlc(self, value):
        previous = os.environ.pop('BB_DLC', None)
        if value is not None:
            os.environ['BB_DLC'] = value
        self.addCleanup(lambda: os.environ.update(BB_DLC=previous) if previous is not None
                        else os.environ.pop('BB_DLC', None))

    def code(self, call):
        offset = patches.EDITION_CALL[0]
        return fake_elf(bytes(offset) + call + bytes(11))

    def test_the_switch_makes_the_edition_one(self):
        offset, call, edition_one = patches.EDITION_CALL
        self.dlc('1')
        self.assertEqual(patches.old_hunters_writes(self.code(call)), [(offset, edition_one)])
        with contextlib.redirect_stdout(io.StringIO()) as log:
            self.assertEqual(patches.old_hunters_writes(self.code(b'\x90' * 5)), [])
        self.assertIn('left alone', log.getvalue())

    def test_off_without_the_switch(self):
        for value in (None, '0'):
            with self.subTest(value=value):
                self.dlc(value)
                self.assertEqual(patches.old_hunters_writes(self.code(patches.EDITION_CALL[1])), [])

    def test_the_verified_executable_has_the_call(self):
        elf_path = ROOT / 'out/eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared eboot.elf')
        elf = elf_path.read_bytes()
        from engine_assets import EBOOT_SHA, sha
        if sha(elf) not in EBOOT_SHA:
            self.skipTest('out/eboot.elf is not the verified 1.09 executable')
        offset, call, _ = patches.EDITION_CALL
        self.assertEqual(patches.original_bytes(elf, offset, len(call)), call)


class RenderPoolTests(unittest.TestCase):
    def test_the_pools_grow_only_over_the_standard_code(self):
        code = bytearray(patches.RENDER_POOL_SITES[-1] + 16)
        for site in patches.RENDER_POOL_SITES:
            code[site:site + 5] = patches.RENDER_POOL_CODE
        self.assertEqual(patches.render_pool_writes(fake_elf(bytes(code))),
                         [(site + 1, struct.pack('<I', 0x100000)) for site in patches.RENDER_POOL_SITES])
        code[patches.RENDER_POOL_SITES[1] + 3] = 0x08
        self.assertEqual(patches.render_pool_writes(fake_elf(bytes(code))), [])

    def test_the_verified_executable_has_the_pool_code(self):
        elf_path = ROOT / 'out/eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared eboot.elf')
        elf = elf_path.read_bytes()
        from engine_assets import EBOOT_SHA, sha
        if sha(elf) not in EBOOT_SHA:
            self.skipTest('out/eboot.elf is not the verified 1.09 executable')
        self.assertEqual(len(patches.render_pool_writes(elf)), 2)


def segments_elf(parts):
    """An ELF with one loaded segment per (image offset, bytes) part."""
    phoff = 64
    elf = bytearray(phoff + 56 * len(parts))
    struct.pack_into('<Q', elf, 0x20, phoff)
    struct.pack_into('<HH', elf, 0x36, 56, len(parts))
    for i, (vaddr, data) in enumerate(parts):
        struct.pack_into('<IIQQQQQQ', elf, phoff + 56 * i, 1, 5, len(elf), vaddr, vaddr, len(data), len(data), 0x4000)
        elf += data
    return bytes(elf)


class GxRegionTests(unittest.TestCase):
    STANDARD = [(patches.GX_REGION_SITE, patches.GX_REGION_CODE),
                (patches.GX_HEAP_SITE, struct.pack('<QQ', 0x8ef00000, 0x37600000))]

    def writes(self, parts=None, heaps_written=False, **env):
        with contextlib.redirect_stdout(io.StringIO()):
            return patches.gx_region_writes(segments_elf(parts or self.STANDARD), heaps_written, env)

    def test_the_region_and_the_heap_grow_together(self):
        self.assertEqual(self.writes(), [(patches.GX_REGION_SITE + 6, struct.pack('<I', 256 << 20)),
                                         (patches.GX_HEAP_SITE, struct.pack('<Q', 0x8ef00000 + (128 << 20)))])

    def test_enlarged_heaps_keep_their_size(self):
        self.assertEqual(self.writes(heaps_written=True),
                         [(patches.GX_REGION_SITE + 6, struct.pack('<I', 256 << 20))])

    def test_the_switch_sizes_it_within_bounds(self):
        self.assertEqual(self.writes(BB_GX_REGION_MB='0'), [])
        self.assertEqual(self.writes(BB_GX_REGION_MB='160'), [])
        self.assertEqual(self.writes(BB_GX_REGION_MB='224')[0][1], struct.pack('<I', 192 << 20))
        self.assertEqual(self.writes(BB_GX_REGION_MB='9999'),
                         [(patches.GX_REGION_SITE + 6, struct.pack('<I', 384 << 20)),
                          (patches.GX_HEAP_SITE, struct.pack('<Q', 0x8ef00000 + (256 << 20)))])

    def test_a_differing_executable_is_left_alone(self):
        other = bytearray(patches.GX_REGION_CODE)
        other[-1] = 0x04
        self.assertEqual(self.writes([(patches.GX_REGION_SITE, bytes(other)), self.STANDARD[1]]), [])
        self.assertEqual(self.writes([self.STANDARD[0], (patches.GX_HEAP_SITE, bytes(16))]), [])

    def test_the_verified_executable_has_the_standard_bytes(self):
        elf_path = ROOT / 'out/eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared eboot.elf')
        elf = elf_path.read_bytes()
        from engine_assets import EBOOT_SHA, sha
        if sha(elf) not in EBOOT_SHA:
            self.skipTest('out/eboot.elf is not the verified 1.09 executable')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(len(patches.gx_region_writes(elf, False, {})), 2)


class HeapPoolTests(unittest.TestCase):
    """An external resolution patch above 1080p enlarges the game's heaps with a 1080p output:
    patches-info.json tells run_windows.py to give the game the larger memory pool."""

    def run_patches(self, directory, external):
        import shutil, subprocess, sys
        root = Path(directory)
        (root / 'xml').mkdir()
        shutil.copy(ROOT / 'patches/Bloodborne.xml', root / 'xml/Bloodborne.xml')
        (root / 'out').mkdir()
        elf = bytearray(fake_elf(bytes(0x100)))
        struct.pack_into('<Q', elf, 64 + 40, 0x6000000)
        (root / 'out/eboot.elf').write_bytes(bytes(elf))
        (root / 'ext').mkdir()
        if external:
            (root / 'ext/community.xml').write_text(
                '<Patch><TitleID><ID>CUSA03173</ID></TitleID><Metadata Title="Bloodborne" Name="Wide" '
                'AppVer="01.09" AppElf="eboot.bin"><PatchList>'
                '<Line Type="bytes32" Address="0x02196A6B" Value="0x000A00B8"/>'
                '<Line Type="bytes" Address="0x04B36DDA" Value="00f7"/></PatchList></Metadata></Patch>')
        (root / 'patches.json').write_text(json.dumps({'enabled': ['community.xml/Wide']}))
        (root / 'lantern.ini').write_text('')
        subprocess.run([sys.executable, str(ROOT / 'scripts/patches.py'), '--xml', str(root / 'xml/Bloodborne.xml'),
                        '--out', str(root / 'out'), '--fps', '30', '--settings', str(root / 'lantern.ini'),
                        '--game-dir', str(root), '--patches-dir', str(root / 'ext'),
                        '--patches-config', str(root / 'patches.json')], check=True, capture_output=True)
        return json.loads((root / 'out/patches-info.json').read_text())['heap_patch']

    def test_an_external_resolution_patch_asks_for_the_larger_pool(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.run_patches(directory, True))
        with tempfile.TemporaryDirectory() as directory:
            self.assertFalse(self.run_patches(directory, False))


class WidescreenUiTests(unittest.TestCase):
    def test_the_ui_viewport_takes_the_screens_shape_with_widescreen(self):
        on = {}
        self.assertEqual(patches.ui_size(on, (3840, 2160)), (1920, 1080))
        self.assertEqual(patches.ui_size(on, (3440, 1440)), (2580, 1080))
        self.assertEqual(patches.ui_size(on, (2560, 1080)), (2560, 1080))
        self.assertEqual(patches.ui_size(on, (5120, 1440)), (3840, 1080))
        self.assertEqual(patches.ui_size(on, (2560, 1600)), (1920, 1200))
        self.assertEqual(patches.ui_size({'widescreen': '0'}, (3440, 1440)), (1920, 1080))

    def test_the_resolution_patch_writes_the_widened_ui(self):
        writes = patches.resolution_writes(XML, (2580, 1080), '01.09', [(0, 1 << 40)], (2580, 1080))
        data = dict(writes)
        self.assertEqual(data[patches.UI_WIDTH], bytes([0xB8]) + (2580).to_bytes(3, 'little'))
        self.assertEqual(data[patches.UI_HEIGHT], bytes([0xB9]) + (1080).to_bytes(3, 'little'))


class BuiltInSelectionTests(unittest.TestCase):
    def test_switched_on_builtins_apply_and_managed_ones_never(self):
        xml = Path(patches.__file__).resolve().parent.parent / 'patches/Bloodborne.xml'
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'patches.json'
            config.write_text(json.dumps({'enabled': ['Bloodborne.xml/No Rally Decay', 'Bloodborne.xml/60 FPS++',
                                                      'Bloodborne.xml/Disable DoF', 'Bloodborne.xml/Not a patch',
                                                      'other.xml/No Rally Decay'],
                                          'disabled': ['Bloodborne.xml/Increased camera distance']}))
            self.assertEqual(patches.builtin_selection(xml, config), ['No Rally Decay'])
            self.assertEqual(patches.builtin_selection(xml, None), [])
        self.assertTrue(patches.managed_builtin('Resolution Patch 2560x1080 (21:9)'))
        self.assertTrue(patches.managed_builtin('Intel 12th Gen+ SFX workaround'))
        self.assertFalse(patches.managed_builtin('Unlock Game Region'))
        self.assertFalse(patches.managed_builtin(patches.HEAP_PATCH))

    def test_a_switched_on_builtin_past_the_eboot_is_left_out_not_fatal(self):
        segments = [(0, 0x06000000 - patches.EBOOT_BASE)]
        with tempfile.TemporaryDirectory() as directory:
            xml = Path(directory) / 'Bloodborne.xml'
            xml.write_text('<Patch><Metadata Name="Past" AppVer="01.09"><PatchList>'
                           '<Line Type="bytes" Address="0x0603AD1A" Value="00"/></PatchList></Metadata></Patch>')
            with contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertFalse(patches.placeable(xml, 'Past', '01.09', segments))
            self.assertIn('0x0603AD1A is outside the eboot', err.getvalue())
        for name in ('Unlock Game Region', 'Disable Camera Auto Rotation via Movement', 'Player No Dead (Read note)',
                     'Performance Patch (perf increase)', 'Enemy Control'):
            self.assertTrue(patches.placeable(XML, name, '01.09', segments), name)


COMMUNITY_NAMES = (
    'Skip Intro', 'Lamp2Lamp (warp between lamps)', 'Lamp2Lamp: warp to bosses',
    'Performance Patch (perf increase)', 'Disable Motion Blur (perf increase)',
    'Disable Dynamic Light Shadows (perf increase)', 'Disable Chromatic Aberration', 'Disable AA', 'Disable DoF',
    'Disable SSAO', 'Enable Screen Space Reflections (READ NOTE)', '50% Text scale', 'FMOD Crash Fix',
    'Model LOD 2 (Lowest)', 'Model LOD 1 (Lower)', 'Model LOD -2 (Highest)', 'Increased Graphics Heap Sizes',
    'Intel 12th Gen+ SFX workaround', 'Intel Black Tonemap Fix', 'Disable HTTP Requests', 'DS1-like physics',
    'Lower object/corpse physics', '30 FPS++', '60FPS (no deltatime)', '60 FPS++', '90 FPS++', 'Uncap FPS++',
    'Restore Debug Menu (READ NOTES)', 'Disable Camera Auto Rotation via Movement', 'Increased camera distance',
    'No Rally Decay', 'Disable Rally (HP Regain)', 'Player No Dead (Read note)', 'Player Stealth (Read note)',
    'Player Silent (Read note)', 'Sensitive Analog Input (easier to run)', 'Unlock Game Region',
    'Unlock All Languages (Works Online)',
    'Bookmark and Capture outputs', 'Enemy Control', 'Enable Extra Messenger Skins',
    '1280x800 Light Grid For SteamDeck (READ NOTES)',
    '1080p Light Grid (READ NOTES)', '1440p Light Grid (READ NOTES)', '4k Light Grid (READ NOTES)', 'Optimal 1080p',
    'Resolution Patch 640x360 (16:9)', 'Resolution Patch 960x540 (16:9)', 'Resolution Patch 1280x720 (16:9)',
    'Resolution Patch 1440x810 (16:9)', 'Resolution Patch 1600x900 (16:9)', 'Resolution Patch 2560x1440 (16:9)',
    'Resolution Patch 3840x2160 (16:9)', 'Resolution Patch 1280x800 (16:10)', 'Resolution Patch 1920x1200 (16:10)',
    'Resolution Patch 2560x1600 (16:10)', 'Resolution Patch 2560x1080 (21:9)', 'Resolution Patch 3440x1440 (21:9)',
    'Resolution Patch 5120x2160 (21:9)', 'Resolution Patch 3840x1080 (32:9)', 'Resolution Patch 5120x1440 (32:9)',
    'Resolution Patch 1280x960 (4:3)', 'Resolution Patch 1440x1080 (4:3)', 'Restore Debug Camera',
    'Sprint Fix (High FPS)')


class OwnPatchTests(unittest.TestCase):
    def test_every_community_name_is_built_in(self):
        names = [meta.get('Name') for meta in ET.parse(XML).getroot().iter('Metadata')]
        self.assertEqual(len(COMMUNITY_NAMES), 65)
        self.assertEqual(sorted(names), sorted(COMMUNITY_NAMES))

    def test_the_movement_steps_follow_the_frame_time(self):
        for name in ('30 FPS++', '60 FPS++', '90 FPS++', 'Uncap FPS++', '60FPS (no deltatime)'):
            with self.subTest(name=name):
                writes = {o + EBOOT_BASE: d for o, d in compile_patches(XML, [name], '01.09', [(0, 1 << 40)])}
                for load in (0x1915FAB, 0x1916030):
                    self.assertEqual(load + 8 + struct.unpack('<i', writes[load + 4])[0], 0x575A908, hex(load))

    def test_the_air_drag_follows_the_step(self):
        for name in ('30 FPS++', '60 FPS++', '90 FPS++', 'Uncap FPS++', '60FPS (no deltatime)'):
            with self.subTest(name=name):
                writes = {o + EBOOT_BASE: d for o, d in compile_patches(XML, [name], '01.09', [(0, 1 << 40)])}
                self.assertNotIn(0x4D0B5B0, writes)
                self.assertEqual(writes[0x1E4ABCA][:1], bytes.fromhex('e9'))
                cave = 0x1E4ABCF + struct.unpack('<i', writes[0x1E4ABCA][1:5])[0]
                self.assertEqual(writes[cave][:8], bytes.fromhex('c5fa109578f6ffff'))
                k, sixth, half, one = struct.unpack('<4f', writes[0x3ABC560])
                for fps in (30, 60, 90, 120, 144, 240):
                    x = k / fps
                    self.assertAlmostEqual(((x * sixth + half) * x - one) * x + one, 0.95 ** (30 / fps), places=5)
        others = [name for name in COMMUNITY_NAMES if name not in
                  ('30 FPS++', '60 FPS++', '90 FPS++', 'Uncap FPS++', '60FPS (no deltatime)')]
        for name in others:
            writes = {o + EBOOT_BASE for o, _ in compile_patches(XML, [name], '01.09', [(0, 1 << 40)])}
            self.assertFalse(writes & {0x1E4ABCA, 0x3ABC500, 0x3ABC560}, name)

    def test_every_built_in_patch_is_lantern_engines_or_credited(self):
        metas = list(ET.parse(XML).getroot().iter('Metadata'))
        self.assertTrue({meta.get('Author') for meta in metas} <= {'Lantern Engine', 'distpade (NexusMods)'})
        names = [meta.get('Name') for meta in metas]
        self.assertEqual(len(names), len(set(names)))
        for meta in metas:
            self.assertTrue(meta.get('Note'), meta.get('Name'))

    def test_the_built_in_file_is_what_the_eboot_builds(self):
        elf_path = ROOT / 'out/eboot.elf'
        if not elf_path.is_file():
            self.skipTest('no prepared eboot.elf')
        from engine_assets import EBOOT_SHA, sha
        if sha(elf_path.read_bytes()) not in EBOOT_SHA:
            self.skipTest('out/eboot.elf is not the verified 1.09 executable')
        sys.path.insert(0, str(ROOT / 'tools/re'))
        import game_patches
        built = game_patches.render(game_patches.build(game_patches.Eboot(elf_path)))
        self.assertEqual(XML.read_bytes().decode('utf-8').replace('\r\n', '\n'), built)

    def test_the_90_fps_patch_has_every_site_the_rate_scaling_rewrites(self):
        writes = patches.compile_patches(XML, [patches.SCALED_BASE], '01.09', [(0, 1 << 40)])
        at = {offset + EBOOT_BASE: data for offset, data in writes}
        for address, value in patches.scaled_rate_values(90).items():
            self.assertEqual(at.get(address), value, hex(address))


if __name__ == '__main__':
    unittest.main()
