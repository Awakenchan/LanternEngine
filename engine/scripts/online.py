# SPDX-License-Identifier: GPL-2.0-or-later
"""The single INI online profile and automatically generated game host override."""
import json
import os
import sys
from pathlib import Path
from urllib.parse import urlsplit

GAME_STATUS_ORIGIN = 'https://ss4.scej-network.jp:20443'
# Messages, bloodstains and ghosts: Bloodborne's own game server, The Hunter's Dream by default.
HUNTERS_DREAM = 'https://thehuntersdream.com'
# The game PUTs its play logs to FromSoftware's S3 buckets (bucket names nobody here owns); the game
# server takes them on its API port instead (PUT /<date>/<file>, plain HTTP), as bbhost sends them
# (droogie/bbhost src/hle/http.cpp).
PLAY_LOG_HOSTS = ('bb-playlog-prod.s3.amazonaws.com', 'bb-playlog-test.s3.amazonaws.com')
GAME_API_PORT = 18671
SHADNET_HOST = 'srv.shadps4.net'
# P2P (co-op) port when none is set: the PS4's own, so firewall rules stay the same between games.
DEFAULT_P2P_PORT = 3658
# Seconds a summon sign entry waits for its host (the game's 180; engine/online_fixes.cpp).
DEFAULT_SIGN_TIMEOUT = 30
SEAMLESS_SERVER = 'https://185-244-28-175.sslip.io'
SEAMLESS_DIRECT_PORT = '38765'
RULE_PATCH_PREFIX = 'Bloodborne.xml/'
GAMEPLAY_RULES = (('Lamp2Lamp (warp between lamps)', 'patch.lamp2lamp'),
                  ('Lamp2Lamp: warp to bosses', 'patch.lamp2lamp-bosses'))
RULESET_CHARS = frozenset('abcdefghijklmnopqrstuvwxyz0123456789_.;=,-')
RULESET_MAX = 256
ROOM_ENV = ('BB_ROOM_SERVER', 'BB_ROOM_NAME', 'BB_ROOM_PLAYER', 'BB_ROOM_HOST', 'BB_ROOM_HTTP_PORT',
            'BB_ROOM_LOCAL_PORT', 'BB_ROOM_VISITOR')
THD_ENV = ('BB_NET_BACKEND', 'BB_THD_SERVER', 'BB_THD_AUTH', 'BB_THD_TOKEN', 'BB_THD_ONLINE_ID',
           'BB_THD_RULESET', 'BB_THD_REFUSED')
ONLINE_ENV = ('BB_ONLINE', 'BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID',
              'BB_SHADNET_PASSWORD', 'BB_SHADNET_TOKEN', 'BB_UPNP', 'BB_P2P_PORT',
              'SHADPS4_HTTP_HOST_OVERRIDES_JSON', 'BB_SIGN_TIMEOUT') + THD_ENV

def read_settings(path):
    settings = {}
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        if line.lstrip().startswith(('#', ';')):
            continue
        key, sep, value = line.partition('=')
        if sep:
            settings[key.strip()] = value.strip()
    return settings

def game_server(text):
    """The game server as typed: empty is The Hunter's Dream, a bare name gets https://. A
    server without a port keeps the game's own (20443)."""
    text = (text or '').strip()
    if not text:
        return HUNTERS_DREAM
    if '://' not in text:
        text = 'https://' + text
    # Earlier versions defaulted to the shadNet Web API, which has no game server.
    if urlsplit(text).hostname == SHADNET_HOST:
        return HUNTERS_DREAM
    return text

def http_base(text):
    if not text or any(ord(c) < 32 for c in text):
        raise ValueError('Online requires an explicit WebAPI / game API URL')
    value = urlsplit(text)
    if value.scheme not in ('http', 'https') or not value.hostname or value.username is not None or value.password is not None or value.query or value.fragment:
        raise ValueError('Online API must be an HTTP(S) URL without credentials, query or fragment')
    if value.port is not None and not 1 <= value.port <= 65535:
        raise ValueError('Invalid online API port')
    return text.rstrip('/')

# BB_ONLINE_PAUSED=1 keeps the game offline whatever lantern.ini says.
ONLINE_PAUSED = os.environ.get('BB_ONLINE_PAUSED', '0') == '1'


def plate_name(text):
    name = ''
    for c in text.strip():
        if 0x20 <= ord(c) < 0x7f and c not in '"\\' and not (c == ' ' and (not name or name[-1] == ' ')):
            name += c
        if len(name) == 16:
            break
    return name.rstrip() or 'Hunter'


def configure_seamless(settings, data, env):
    """Seamless co-op: the room backend (BB_NET_BACKEND=room). The game's servers go to the room's
    local server; the password comes from the launcher."""
    get = lambda key: settings.get(key, '').strip()
    server = get('seamless_server') or SEAMLESS_SERVER
    host = get('seamless_role') == 'host'
    room = get('seamless_host_room' if host else 'seamless_room')
    if not room:
        raise ValueError("Open the launcher's Online page once to make your room" if host
                         else "Paste the host's invite code on the launcher's Online page")
    p2p = int(p2p_port(settings))
    http = p2p + 1 if p2p < 65535 else p2p - 1
    local = f'http://127.0.0.1:{http}'
    overrides = {'*.scej-network.jp': local, GAME_STATUS_ORIGIN: local}
    overrides.update({h: local for h in PLAY_LOG_HOSTS})
    destination = write_overrides(data, overrides)
    for key in ONLINE_ENV:
        env.pop(key, None)
    direct_port = get('seamless_direct_port') or SEAMLESS_DIRECT_PORT
    direct = get('seamless_direct') == '1'
    env.pop('BB_ROOM_LOCAL_PORT', None)
    if direct and not host:
        address = get('seamless_direct_host')
        if not address:
            raise ValueError("Paste the host's invite code, or type the host's address on the launcher's Online page")
        server = 'ws://' + (address if ':' in address else f'{address}:{direct_port}')
    elif direct:
        port = int(direct_port) if direct_port.isdigit() else 0
        if not 1024 <= port <= 65535:
            raise ValueError("The direct room's port must be between 1024 and 65535")
        server = f'ws://127.0.0.1:{port}'
        env['BB_ROOM_LOCAL_PORT'] = str(port)
    elif server.startswith('ws://'):
        server = SEAMLESS_SERVER
    env.pop('BB_ROOM_VISITOR', None)
    if not host and not direct and get('seamless_visitor') == '1':
        env['BB_ROOM_VISITOR'] = '1'
    if host:
        env['BB_ROOM_PASSWORD'] = env.get('BB_ROOM_HOST_PASSWORD', '')
    env.update(BB_ONLINE='1', BB_NET_BACKEND='room', BB_ROOM_SERVER=server, BB_ROOM_NAME=room,
               BB_ROOM_PLAYER=plate_name(get('seamless_player')), BB_ROOM_HOST='1' if host else '0',
               BB_ROOM_HTTP_PORT=str(http), BB_P2P_PORT=str(p2p),
               SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(destination.resolve()))
    timeout = get('online_sign_timeout')
    env['BB_SIGN_TIMEOUT'] = str(min(max(int(timeout), 0), 600)) if timeout.isdigit() else str(DEFAULT_SIGN_TIMEOUT)
    return destination


def configure_online(settings, data, env):
    if settings.get('seamless_enabled') == '1' and settings.get('online_enabled') == '1':
        return configure_seamless(settings, data, env)
    for key in ROOM_ENV:
        env.pop(key, None)
    if (settings.get('online_pool') == 'huntersdream' and settings.get('online_enabled') == '1' and
            settings.get('online_connected', '1') == '1'):
        return configure_huntersdream(settings, data, env)
    if ONLINE_PAUSED and settings.get('online_enabled') == '1':
        print("Online: paused in this version while we rework the connection together with The Hunter's "
              "Dream team; the game starts offline", flush=True)
    elif settings.get('online_enabled') == '1' and settings.get('online_connected', '1') == '1':
        print("Online: no The Hunter's Dream sign-in or seamless room is chosen on the launcher's Online page; "
              "the game starts offline", file=sys.stderr, flush=True)
    for key in ONLINE_ENV:
        env.pop(key, None)
    env['BB_ONLINE'] = '0'
    return None


def p2p_port(settings):
    p2p = settings.get('online_p2p_port', '').strip()
    if p2p and not 1 <= int(p2p) <= 65535:
        raise ValueError('Local P2P port must be between 1 and 65535, or empty for the default (3658)')
    return str(int(p2p)) if p2p else str(DEFAULT_P2P_PORT)


def write_overrides(data, overrides):
    destination = Path(data) / 'network/host_overrides.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(overrides, indent=2), encoding='utf-8')
    os.replace(temporary, destination)
    return destination


def valid_ruleset(rules):
    return 0 < len(rules) <= RULESET_MAX and all(c in RULESET_CHARS for c in rules)


def enabled_patches(data, env):
    path = env.get('BB_PATCHES_CONFIG') or Path(data) / 'patches.json'
    try:
        chosen = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return []
    enabled = chosen.get('enabled', []) if isinstance(chosen, dict) else []
    return [key for key in enabled if isinstance(key, str)] if isinstance(enabled, list) else []


def ruleset(enabled):
    tokens = {token for key in enabled if key.startswith(RULE_PATCH_PREFIX)
              for patch, token in GAMEPLAY_RULES if key[len(RULE_PATCH_PREFIX):] == patch}
    return ','.join(sorted(tokens)) or 'vanilla'


def configure_huntersdream(settings, data, env):
    """The Hunter's Dream pool: the engine's own backend for it (BB_NET_BACKEND=thd), bbhost's
    protocol. The game's servers (*.scej-network.jp, its own ports) and play logs (the API port)
    go to the game server; the account comes from the launcher's Discord sign-in."""
    game_api = http_base(game_server(settings.get('online_game_api', '')))
    origin = urlsplit(game_api)
    if origin.path not in ('', '/'):
        raise ValueError('The game server needs an origin without a path')
    if not env.get('BB_THD_TOKEN') or not env.get('BB_THD_ONLINE_ID'):
        raise ValueError("Sign in to The Hunter's Dream with Discord on the launcher's Online page")
    scheme, host = origin.scheme, origin.hostname
    base = f'{scheme}://{host}'
    overrides = {'*.scej-network.jp': base}
    overrides.update({h: f'{base}:{GAME_API_PORT}' for h in PLAY_LOG_HOSTS})
    destination = write_overrides(data, overrides)
    for key in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD',
                'BB_SHADNET_TOKEN', 'BB_UPNP'):
        env.pop(key, None)
    typed = settings.get('online_thd_ruleset', '').strip()
    rules = typed if valid_ruleset(typed) else ruleset(enabled_patches(data, env))
    env.update(BB_ONLINE='1', BB_NET_BACKEND='thd', BB_THD_SERVER=f'{base}:{GAME_API_PORT}',
               BB_THD_AUTH=base, BB_THD_RULESET=rules,
               BB_P2P_PORT=p2p_port(settings), SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(destination.resolve()))
    timeout = settings.get('online_sign_timeout', '').strip()
    env['BB_SIGN_TIMEOUT'] = str(min(max(int(timeout), 0), 600)) if timeout.isdigit() else str(DEFAULT_SIGN_TIMEOUT)
    return destination


def main(argv):
    import argparse
    import sys
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    parser.add_argument('--data', required=True)
    args = parser.parse_args(argv)
    config = Path(args.config)
    env = dict(os.environ)
    try:
        override = configure_online(read_settings(config) if config.is_file() else {}, args.data, env)
    except (OSError, ValueError) as error:
        print(f'Online configuration: {error}', file=sys.stderr)
        return 1
    print('Online: configured server profile and automatic game redirect' if override else 'Online: offline mode',
          file=sys.stderr)
    for key in sorted(set(os.environ) - set(env)):
        print(f'unset {key}')
    for key, value in sorted(env.items()):
        if os.environ.get(key) != value and '\n' not in value:
            print(f'{key}={value}')
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv[1:]))
