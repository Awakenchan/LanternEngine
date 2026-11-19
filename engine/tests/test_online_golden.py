# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import os
os.environ['BB_ONLINE_PAUSED'] = '0'
from online import configure_online

GOLDEN = Path(__file__).resolve().parent / 'golden'
CASES = GOLDEN / 'online-profile-cases.json'
EXPECTED = GOLDEN / 'online-profile-script.txt'


def normalised(value, data):
    if value.startswith(data):
        return '<data>' + value[len(data):].replace('\\', '/')
    return value


def run_case(cases, case, root):
    data = root / case['name']
    data.mkdir(parents=True)
    env = dict(cases['inherited'])
    env.update(case['env'])
    if 'patches' in case:
        (data / 'patches.json').write_text(json.dumps({'enabled': case['patches']}), encoding='utf-8')
    lines = [f"[{case['name']}]"]
    try:
        lines.append('result: online' if configure_online(dict(case['settings']), data, env) else 'result: offline')
    except ValueError as error:
        lines.append(f'result: error: {error}')
    data_text = str(data.resolve())
    lines.append('env:')
    for key in cases['tracked']:
        lines.append(f'{key}={normalised(env[key], data_text)}' if key in env else f'{key} unset')
    overrides = data / 'network' / 'host_overrides.json'
    lines.append('host_overrides.json:')
    lines.append(overrides.read_text(encoding='utf-8') if overrides.exists() else '(none)')
    return '\n'.join(lines) + '\n'


def profile_text():
    cases = json.loads(CASES.read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory() as temp:
        return ''.join(run_case(cases, case, Path(temp)) for case in cases['cases'])


class OnlineProfileGoldenTests(unittest.TestCase):
    def test_profiles_match_the_recorded_trunk(self):
        self.assertEqual(profile_text(), EXPECTED.read_text(encoding='utf-8'))


if __name__ == '__main__':
    if '--record' in sys.argv:
        EXPECTED.write_text(profile_text(), encoding='utf-8', newline='\n')
        print(f'recorded {EXPECTED}')
    else:
        unittest.main()
