# SPDX-License-Identifier: GPL-3.0-or-later
import re
from pathlib import Path
import unittest

BBNET = Path(__file__).resolve().parents[1] / 'gpu' / 'bbnet'
SOURCES = sorted(p for p in BBNET.rglob('*') if p.suffix in ('.cpp', '.h') and 'third_party' not in p.parts
                 and 'bbhost' not in p.parts)

LOG_CALL = re.compile(r'\b(?:Log|LOG_[A-Z]+)\s*\(')
DOTTED = re.compile(r'%[ud]\.%[ud]\.%[ud]\.%[ud]|\{\}\.\{\}\.\{\}\.\{\}')
RAW_TEXT = re.compile(r'\binet_ntoa\b|\binet_ntop\s*\(|\bwrite_ipv4\s*\(')
HEX_ADDR = re.compile(r'(?i)\b(?!server)\w*(?:addr|ip|ipv4|endpoint)\w*=\{:#x\}')

FORMATTERS = {
    ('include/ip_mask.h', 'snprintf'): 1,
    ('thd/thd_net.cpp', 'snprintf'): 1,
    ('thd/thd_session.cpp', 'snprintf'): 1,
}


def statements(text):
    for m in LOG_CALL.finditer(text):
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == '(':
                depth += 1
            elif text[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield text.count('\n', 0, m.start()) + 1, text[m.start():i + 1]


class LogPrivacyTests(unittest.TestCase):
    def test_no_log_statement_prints_a_raw_address(self):
        bad = []
        for path in SOURCES:
            text = path.read_text(encoding='utf-8', errors='replace')
            for line, stmt in statements(text):
                if DOTTED.search(stmt) or RAW_TEXT.search(stmt) or HEX_ADDR.search(stmt):
                    bad.append(f'{path.relative_to(BBNET)}:{line}')
        self.assertEqual(bad, [])

    def test_dotted_formatting_outside_logs_is_only_the_known_wire_paths(self):
        found = {}
        for path in SOURCES:
            rel = path.relative_to(BBNET).as_posix()
            for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
                if DOTTED.search(raw) and 'sscanf' not in raw:
                    key = (rel, 'snprintf' if 'snprintf' in raw else raw.strip())
                    found[key] = found.get(key, 0) + 1
        self.assertEqual(found, FORMATTERS)

    def test_thd_log_lines_use_the_mask(self):
        thd = {p.name: p.read_text(encoding='utf-8') for p in (BBNET / 'thd').glob('*.cpp')}
        for needle in ('net: recv', 'net: send', 'net: punch', 'sees our P2P port', 'our address is now',
                       'np: guest_invite'):
            owner = next(t for t in thd.values() if needle in t)
            at = owner.index(needle)
            self.assertIn('BbNet::Mask', owner[at:at + 300], needle)

    def test_vendored_bbhost_logs_go_through_the_mask(self):
        shim = (BBNET / 'vendor' / 'bbhost_shim' / 'log.h').read_text(encoding='utf-8')
        self.assertIn('#define host_log bbhost_np_log', shim)
        glue = (BBNET / 'thd' / 'thd_bbhost_glue.cpp').read_text(encoding='utf-8')
        at = glue.index('void bbhost_np_log(')
        self.assertIn('BbNet::MaskIpv4Host', glue[at:at + 1200])


if __name__ == '__main__':
    unittest.main()
