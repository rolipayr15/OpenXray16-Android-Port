"""Command line: python -m xray_online {init,run,set-admin-password} [--config FILE]."""
import argparse
import asyncio
import configparser
import getpass
import logging
import os
import secrets
import signal
import sys

from . import gp, master, web
from .store import Store, hash_web_password

DEFAULTS = {
    'bind': '0.0.0.0',
    'master_port': '28910',
    'gpcm_port': '29900',
    'gpsp_port': '29901',
    'web_bind': '0.0.0.0',
    'web_port': '8080',
    'database': 'xray-online.sqlite3',
    'log_level': 'INFO',
}


def load_config(path):
    parser = configparser.ConfigParser()
    if not parser.read(path, encoding='utf-8'):
        sys.exit(f'Config {path} not found. Create it with: python -m xray_online init --config {path}')
    config = dict(DEFAULTS)
    config.update(parser['server'] if parser.has_section('server') else {})
    if not config.get('secret'):
        sys.exit('Config has no secret; run init again or add one to [server].')
    base = os.path.dirname(os.path.abspath(path))
    if not os.path.isabs(config['database']):
        config['database'] = os.path.join(base, config['database'])
    return config


def write_config(path, values):
    parser = configparser.ConfigParser()
    parser['server'] = values
    with open(path, 'w', encoding='utf-8') as f:
        parser.write(f)


def ask_password():
    while True:
        first = getpass.getpass('Admin password: ')
        if len(first) < 8:
            print('Use at least 8 characters.')
            continue
        if first == getpass.getpass('Repeat: '):
            return first
        print('Passwords differ.')


def cmd_init(args):
    if os.path.exists(args.config) and not args.force:
        sys.exit(f'{args.config} exists; pass --force to overwrite it.')
    values = dict(DEFAULTS)
    values['secret'] = secrets.token_hex(32)
    values['admin_password'] = hash_web_password(args.admin_password or ask_password())
    write_config(args.config, values)
    Store(load_config(args.config)['database'])
    print(f'Wrote {args.config}. Start the server with: python -m xray_online run --config {args.config}')


def cmd_set_admin_password(args):
    parser = configparser.ConfigParser()
    parser.read(args.config, encoding='utf-8')
    parser['server']['admin_password'] = hash_web_password(args.admin_password or ask_password())
    with open(args.config, 'w', encoding='utf-8') as f:
        parser.write(f)
    print('Admin password updated; restart the server.')


async def serve(config):
    store = Store(config['database'])
    registry = master.Registry(store)
    sessions = gp.Sessions()
    bind = config['bind']
    tcp, udp, maintenance = await master.start(registry, store, bind, int(config['master_port']))
    gpcm, gpsp = await gp.start(store, sessions, bind, int(config['gpcm_port']), int(config['gpsp_port']))
    httpd = web.start(web.WebApp(store, registry, sessions, config), config['web_bind'], int(config['web_port']))

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for name in ('SIGINT', 'SIGTERM'):
        if hasattr(signal, name):
            try:
                loop.add_signal_handler(getattr(signal, name), stop.set)
            except NotImplementedError:  # Windows: Ctrl+C raises KeyboardInterrupt instead
                pass
    try:
        await stop.wait()
    finally:
        maintenance.cancel()
        for server in (tcp, gpcm, gpsp):
            server.close()
        udp.close()
        httpd.shutdown()


def cmd_run(args):
    config = load_config(args.config)
    logging.basicConfig(level=config['log_level'].upper(), format='%(asctime)s %(name)s %(levelname)s %(message)s')
    try:
        asyncio.run(serve(config))
    except KeyboardInterrupt:
        pass


def main():
    parser = argparse.ArgumentParser(prog='xray_online', description='Online services for X-Ray engine games.')
    parser.add_argument('--config', default='xray-online.ini')
    sub = parser.add_subparsers(dest='command', required=True)
    init = sub.add_parser('init', help='write a config with a random secret and an admin password')
    init.add_argument('--admin-password')
    init.add_argument('--force', action='store_true')
    sub.add_parser('run', help='run all services')
    password = sub.add_parser('set-admin-password')
    password.add_argument('--admin-password')
    args = parser.parse_args()
    {'init': cmd_init, 'run': cmd_run, 'set-admin-password': cmd_set_admin_password}[args.command](args)


if __name__ == '__main__':
    main()
