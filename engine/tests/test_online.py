# SPDX-License-Identifier: GPL-2.0-or-later
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import os
os.environ['BB_ONLINE_PAUSED'] = '0'
from online import (configure_online, game_server, http_base, ONLINE_ENV, HUNTERS_DREAM, PLAY_LOG_HOSTS,
                    DEFAULT_P2P_PORT, DEFAULT_SIGN_TIMEOUT)


class OnlineProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.settings = dict(online_enabled='1', online_connected='1', online_pool='huntersdream')
        self.env = {'BB_THD_TOKEN': 'test-token', 'BB_THD_ONLINE_ID': 'Hunter', 'UNRELATED': 'keep'}

    def offline(self, settings):
        env = dict.fromkeys(ONLINE_ENV, 'inherited')
        env['UNRELATED'] = 'keep'
        said = io.StringIO()
        with contextlib.redirect_stderr(said):
            self.assertIsNone(configure_online(settings, self.data, env))
        self.assertEqual(env, {'BB_ONLINE': '0', 'UNRELATED': 'keep'})
        self.assertFalse((self.data / 'network').exists())
        return said.getvalue()

    def test_offline_clears_inherited_online_credentials_and_override(self):
        for settings in ({}, {'online_enabled': '0'}, {'online_enabled': '1', 'online_connected': '0'}):
            self.assertEqual(self.offline(settings), '')

    def test_old_shadnet_and_supermedo_profiles_start_offline_with_one_line(self):
        shadnet = dict(online_enabled='1', online_connected='1', online_host='server.example', online_port='31413',
                       online_webapi='http://server.example:31415', online_username='Hunter', online_upnp='0')
        supermedo = dict(online_enabled='1', online_connected='1', online_server='supermedo',
                         online_host='game.supermedo.com', online_port='31313',
                         online_webapi='http://game.supermedo.com:31315/',
                         online_game_api='https://game.supermedo.com/', online_username='Hunter')
        for settings in (shadnet, supermedo, {'online_enabled': '1'}):
            with self.subTest(settings=settings):
                said = self.offline(settings)
                self.assertEqual(len(said.splitlines()), 1)
                self.assertIn('the game starts offline', said)

    def test_huntersdream_custom_ports_and_play_logs(self):
        self.settings['online_game_api'] = 'https://game.example:31443'
        self.settings['online_p2p_port'] = '32000'
        path = configure_online(self.settings, self.data, self.env)
        self.assertEqual(json.loads(path.read_text()),
                         {'*.scej-network.jp': 'https://game.example',
                          **{h: 'https://game.example:18671' for h in PLAY_LOG_HOSTS}})
        self.assertEqual(self.env['BB_P2P_PORT'], '32000')
        self.assertEqual(self.env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'], str(path.resolve()))
        self.assertEqual(self.env['UNRELATED'], 'keep')
        self.assertNotIn('test-token', path.read_text())
        self.assertFalse(path.with_suffix('.json.tmp').exists())

    def test_huntersdream_defaults(self):
        self.env['BB_P2P_PORT'] = 'inherited'
        configure_online(self.settings, self.data, self.env)
        self.assertEqual(self.env['BB_THD_AUTH'], HUNTERS_DREAM)
        self.assertEqual(self.env['BB_P2P_PORT'], str(DEFAULT_P2P_PORT))
        self.assertEqual(self.env['BB_SIGN_TIMEOUT'], str(DEFAULT_SIGN_TIMEOUT))

    def test_ruleset_from_patches_as_the_start_program(self):
        (self.data / 'patches.json').write_text(json.dumps({'enabled': [
            'Bloodborne.xml/Lamp2Lamp: warp to bosses', 'Bloodborne.xml/Lamp2Lamp (warp between lamps)',
            'Bloodborne.xml/Lamp2Lamp (warp between lamps)', 'Other.xml/Lamp2Lamp (warp between lamps)', 7]}),
            encoding='utf-8')
        for typed, expected in (('', 'patch.lamp2lamp,patch.lamp2lamp-bosses'),
                                ('Not Valid!', 'patch.lamp2lamp,patch.lamp2lamp-bosses'),
                                ('patch.cheats', 'patch.cheats')):
            with self.subTest(typed=typed):
                configure_online(dict(self.settings, online_thd_ruleset=typed), self.data, self.env)
                self.assertEqual(self.env['BB_THD_RULESET'], expected)
        (self.data / 'patches.json').write_text('not json', encoding='utf-8')
        configure_online(self.settings, self.data, self.env)
        self.assertEqual(self.env['BB_THD_RULESET'], 'vanilla')

    def test_sign_timeout_setting(self):
        for typed, expected in (('0', '0'), ('180', '180'), ('9999', '600'), ('abc', '30'), ('', '30')):
            self.settings['online_sign_timeout'] = typed
            configure_online(self.settings, self.data, self.env)
            self.assertEqual(self.env['BB_SIGN_TIMEOUT'], expected)

    def test_game_server_bare_name_and_old_shadnet_default(self):
        self.assertEqual(game_server('thehuntersdream.com'), 'https://thehuntersdream.com')
        self.assertEqual(game_server('http://srv.shadps4.net:31315'), HUNTERS_DREAM)
        self.assertEqual(game_server('http://game.example:8080'), 'http://game.example:8080')

    def test_invalid_huntersdream_profile_fails_before_writing_cache(self):
        for key, values in {
            'online_game_api': ['ftp://server.example', 'http://user:pass@server.example',
                                'http://server.example:0', 'http://server.example/api']}.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    settings = dict(self.settings, **{key: value})
                    with self.assertRaises(ValueError): configure_online(settings, self.data, dict(self.env))
                    self.assertFalse((self.data / 'network').exists())

    def test_invalid_p2p_port_fails(self):
        for value in ('0', '65536', 'bad'):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    configure_online(dict(self.settings, online_p2p_port=value), self.data, dict(self.env))

    def test_no_sign_in_no_cache(self):
        with self.assertRaises(ValueError): configure_online(self.settings, self.data, {})
        self.assertFalse((self.data / 'network').exists())

    def test_url_does_not_drop_port(self):
        self.assertEqual(http_base('https://server.example:34443/api/'), 'https://server.example:34443/api')
        for url in ('http://server.example?x=1', 'http://server.example#x', 'http://server.example\n'):
            with self.assertRaises(ValueError): http_base(url)


class OnlinePausedTests(unittest.TestCase):
    def test_paused_starts_offline(self):
        import online
        env = {'BB_SHADNET_SERVER': 'x:1'}
        paused = online.ONLINE_PAUSED
        online.ONLINE_PAUSED = True
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertIsNone(configure_online({'online_enabled': '1'}, '.', env))
        finally:
            online.ONLINE_PAUSED = paused
        self.assertEqual(env, {'BB_ONLINE': '0'})


if __name__ == '__main__': unittest.main()
