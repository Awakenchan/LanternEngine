from paths import ROOT
import re
import struct
import sys
import unittest
import xml.etree.ElementTree as ET

import patches as P

BASE = 0x400000
EFFECT_SECOND_STEP = 0x4D27C4C
EASES = ((0x4D25E68, 0.1), (0x4D25E94, 1 / 30))
STATUS_SECOND = (0x1CBC511, bytes.fromhex('f3e3a903'))


def f32(v):
    return struct.pack('<f', v)


def i32(v):
    return struct.pack('<i', v)


class Image:
    def __init__(self, eboot):
        self.eboot, self.bytes = eboot, {}

    def put(self, at, data):
        for i, b in enumerate(data):
            self.bytes[at + i] = b

    def get(self, at, n):
        return bytes(self.bytes.get(at + i, self.eboot.read(at + i, 1)[0]) for i in range(n))


def lists():
    root = ET.parse(ROOT / 'patches/Bloodborne.xml').getroot()
    return {m.get('Name'): [(int(l.get('Address'), 16), bytes.fromhex(l.get('Value').replace('0x', '')))
                            for l in m.iter('Line')] for m in root.iter('Metadata')}


def sites():
    text = (ROOT / 'gpu/shim/engine/frame_rate_sites.inc').read_text(encoding='utf-8')
    return [(int(m.group(1), 16) + BASE, m.group(2)) for m in re.finditer(r'\{0x([0-9a-f]+), SiteKind::(\w+),', text)]


def values(r):
    cap = 16000 if r <= 60 else 10000 if r <= 90 else 8000 if r <= 120 else 6500
    step = f32(1 / r)
    return {'Step': step, 'ContextStep': step, 'TargetStep': step, 'FrameModeTime': step,
            'HighStep': f32(1 / r if r > 60 else 1 / 30), 'EffectStep': f32(1.125 / r),
            'CountSixth': i32(max(5, int(r / 6))), 'CountThird': i32(max(10, int(r / 3))), 'Three': f32(3 / r),
            'NegThree': f32(-3 / r), 'EaseDistance': f32(1 - 0.9 ** (30 / r)),
            'EaseChase': f32(1 - (1 - 1 / 30) ** (30 / r)), 'WaitSleep': i32(int(990000 / r)), 'LimiterCap': i32(cap)}


class NativeFrameRateTests(unittest.TestCase):
    def setUp(self):
        elf = ROOT / 'out/eboot.elf'
        if not elf.is_file():
            self.skipTest('no prepared eboot.elf')
        sys.path.insert(0, str(ROOT / 'tools/re'))
        from game_patches import Eboot
        self.eboot = Eboot(elf)
        self.lists = lists()

    def start_writes(self, fps):
        names = ['90 FPS++', 'Sprint Fix (High FPS)'] if fps > 60 else ['60 FPS++']
        writes = [(a, d) for n in names for a, d in self.lists[n]]
        if fps in P.SCALED_FPS.values():
            scaled = P.scaled_rate_values(fps)
            writes = [(a, scaled.get(a, f32(1 / fps) if d == f32(1 / 60) and a != EFFECT_SECOND_STEP else d))
                      for a, d in writes]
        image = Image(self.eboot)
        for a, d in writes:
            image.put(a, d)
        for at, weight in EASES:
            image.put(at, f32(1 - (1 - weight) ** (30 / fps)))
        image.put(*STATUS_SECOND)
        return image

    def engine_writes(self, fps):
        names = ['90 FPS++', 'Sprint Fix (High FPS)'] if fps > 60 else ['60 FPS++', 'Sprint Fix (High FPS)']
        image = Image(self.eboot)
        for n in names:
            for a, d in self.lists[n]:
                image.put(a, d)
        image.put(*STATUS_SECOND)
        table = values(float(fps))
        for at, kind in sites():
            if kind == 'EffectStep' and image.get(at - 6, 6) != bytes.fromhex('c7857cffffff'):
                continue
            image.put(at, table[kind])
        return image

    def test_the_engine_writes_what_the_start_wrote_at_the_rates_both_had(self):
        for fps in (60, 120, 144):
            start, engine = self.start_writes(fps), self.engine_writes(fps)
            touched = sorted(set(start.bytes) | set(engine.bytes))
            wall = {a + i for a, d in self.lists['Sprint Fix (High FPS)'] for i in range(len(d))} if fps <= 60 else set()
            differ = [hex(a) for a in touched if a not in wall and start.get(a, 1) != engine.get(a, 1)]
            self.assertEqual(differ, [], f'{fps} FPS')

    def test_the_engine_measures_wall_speed_at_60(self):
        engine = self.engine_writes(60)
        for a, d in self.lists['Sprint Fix (High FPS)']:
            self.assertEqual(engine.get(a, len(d)), d)


if __name__ == '__main__':
    unittest.main()
