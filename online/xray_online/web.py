"""Web interface: public server list and registration, admin panel, JSON API.

Runs on the standard library's threading HTTP server next to the asyncio
services and shares their Store, Registry and Sessions.
"""
import hashlib
import hmac
import html
import json
import logging
import threading
import time
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from . import gp
from . import protocol as codes
from .protocol import GAMES
from .store import check_web_password

log = logging.getLogger('web')

SESSION_TTL = 12 * 3600

STYLE = '''
:root{--bg:#101410;--panel:#181f19;--line:#2c3a2e;--text:#e3eae3;--muted:#9aaa9c;--accent:#9cc48a;--bad:#e07a6a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,sans-serif}
header{border-bottom:1px solid var(--line);padding:14px 20px;display:flex;gap:18px;align-items:baseline;flex-wrap:wrap}
header b{font-size:18px}header a{color:var(--muted);text-decoration:none}header a:hover{color:var(--text)}
main{max-width:1100px;margin:0 auto;padding:20px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px;margin-bottom:18px;overflow-x:auto}
h1{font-size:22px;margin:0 0 12px}h2{font-size:17px;margin:0 0 10px}
table{width:100%;border-collapse:collapse;font-size:14px}th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
th{color:var(--muted);font-weight:600}.muted{color:var(--muted)}.bad{color:var(--bad)}.ok{color:var(--accent)}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}
.stat{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px}.stat b{display:block;font-size:24px}
input,select,textarea{background:#0c100c;color:var(--text);border:1px solid var(--line);border-radius:6px;padding:7px 9px;font:inherit}
button{background:var(--accent);color:#102010;border:0;border-radius:6px;padding:7px 14px;font-weight:600;cursor:pointer}
button.danger{background:var(--bad);color:#200}form.inline{display:inline}
label{display:block;margin:8px 0 3px;color:var(--muted)}.notice{padding:10px 12px;border-radius:8px;margin-bottom:14px;border:1px solid var(--line)}
'''

ERROR_TEXT = {
    codes.GP_DATABASE: 'Регистрация на сервере отключена.',
    codes.GP_NEWUSER_BAD_NICK: 'Неверный e-mail или ник (или у этого e-mail уже есть такой ник).',
    codes.GP_NEWUSER_BAD_PASSWORD: 'Этот e-mail уже зарегистрирован с другим паролем, или пароль недопустим.',
    codes.GP_NEWUSER_UNIQUENICK_INVALID: 'Ник: до 20 символов, латиница, цифры и _-[]{}. , не с цифры.',
    codes.GP_NEWUSER_UNIQUENICK_INUSE: 'Этот ник уже занят.',
}


def _fmt_time(timestamp):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(timestamp)) if timestamp else '—'


def _e(value):
    return html.escape(str(value), quote=True)


class WebApp:
    def __init__(self, store, registry, sessions, config):
        self.store = store
        self.registry = registry
        self.sessions = sessions
        self.config = config
        self.secret = config['secret'].encode()
        self.started = time.time()

    # -- data shared by HTML and JSON --------------------------------------

    def server_rows(self, game=None):
        rows = []
        for server in sorted(self.registry.servers(game), key=lambda s: (s.game, s.ip, s.port)):
            info = server.info
            rows.append({
                'game': server.game,
                'address': f'{server.ip}:{server.port}',
                'name': info.get('hostname', ''),
                'map': info.get('mapname', ''),
                'gametype': info.get('gametype', '') or info.get('gametypename', ''),
                'players': int(info.get('numplayers', 0) or 0),
                'max_players': int(info.get('maxplayers', 0) or 0),
                'password': info.get('password', '0') not in ('', '0'),
                'ping_ms': server.ping_ms,
                'online_since': int(server.first_seen),
            })
        return rows

    def stats(self):
        servers = self.server_rows()
        day_ago = int(time.time()) - 86400
        per_game = {}
        for game, title in GAMES.items():
            game_servers = [s for s in servers if s['game'] == game]
            per_game[game] = {'title': title, 'servers': len(game_servers),
                              'players': sum(s['players'] for s in game_servers)}
        return {
            'games': per_game,
            'servers': len(servers),
            'players': sum(s['players'] for s in servers),
            'accounts_online': len(self.sessions.all()),
            'accounts': self.store.account_totals(),
            'logins_24h': self.store.count_events('login', day_ago),
            'registrations_24h': self.store.count_events('register', day_ago),
            'uptime_s': int(time.time() - self.started),
        }

    # -- admin sessions ----------------------------------------------------

    def make_session(self):
        expires = str(int(time.time()) + SESSION_TTL)
        signature = hmac.new(self.secret, b'admin:' + expires.encode(), hashlib.sha256).hexdigest()
        return f'{expires}.{signature}'

    def valid_session(self, token):
        try:
            expires, signature = token.split('.', 1)
            expected = hmac.new(self.secret, b'admin:' + expires.encode(), hashlib.sha256).hexdigest()
            return hmac.compare_digest(signature, expected) and int(expires) > time.time()
        except (ValueError, AttributeError):
            return False

    def csrf_token(self, session):
        return hmac.new(self.secret, b'csrf:' + session.encode(), hashlib.sha256).hexdigest()[:32]


class Handler(BaseHTTPRequestHandler):
    app: WebApp = None
    server_version = 'xray-online'

    def log_message(self, fmt, *args):
        log.debug('%s %s', self.address_string(), fmt % args)

    # -- plumbing ----------------------------------------------------------

    def _send(self, body, status=HTTPStatus.OK, content_type='text/html; charset=utf-8', headers=None):
        data = body.encode('utf-8') if isinstance(body, str) else body
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'DENY')
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def _redirect(self, location, headers=None):
        self.send_response(HTTPStatus.SEE_OTHER)
        self.send_header('Location', location)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def _form(self):
        length = min(int(self.headers.get('Content-Length', 0) or 0), 64 * 1024)
        raw = self.rfile.read(length).decode('utf-8', 'replace')
        return {key: values[0] for key, values in parse_qs(raw, keep_blank_values=True).items()}

    def _session(self):
        cookie = SimpleCookie(self.headers.get('Cookie', ''))
        token = cookie['xo_admin'].value if 'xo_admin' in cookie else ''
        return token if self.app.valid_session(token) else None

    def _page(self, title, body, admin=False):
        nav = ('<a href="/">Серверы</a><a href="/register">Регистрация</a>' if not admin else
               '<a href="/admin">Сводка</a><a href="/admin/servers">Серверы</a><a href="/admin/accounts">Аккаунты</a>'
               '<a href="/admin/settings">Настройки</a><a href="/">Сайт</a><a href="/admin/logout">Выход</a>')
        return (f'<!doctype html><html lang="ru"><head><meta charset="utf-8">'
                f'<meta name="viewport" content="width=device-width,initial-scale=1">'
                f'<title>{_e(title)} — X-Ray Online</title><style>{STYLE}</style></head>'
                f'<body><header><b>X-Ray Online</b>{nav}</header><main>{body}</main></body></html>')

    # -- routing -----------------------------------------------------------

    def do_GET(self):
        url = urlparse(self.path)
        query = {k: v[0] for k, v in parse_qs(url.query).items()}
        routes = {
            '/': self.public_index, '/register': self.register_form,
            '/api/servers.json': self.api_servers, '/api/stats.json': self.api_stats,
            '/admin/login': self.admin_login_form, '/admin/logout': self.admin_logout,
        }
        if url.path in routes:
            return routes[url.path](query)
        if url.path.startswith('/admin'):
            session = self._session()
            if not session:
                return self._redirect('/admin/login')
            admin_routes = {'/admin': self.admin_dashboard, '/admin/servers': self.admin_servers,
                            '/admin/accounts': self.admin_accounts, '/admin/settings': self.admin_settings}
            if url.path in admin_routes:
                return admin_routes[url.path](query, session)
        self._send(self._page('Не найдено', '<h1>404</h1>'), HTTPStatus.NOT_FOUND)

    def do_POST(self):
        url = urlparse(self.path)
        form = self._form()
        if url.path == '/register':
            return self.register_submit(form)
        if url.path == '/admin/login':
            return self.admin_login_submit(form)
        session = self._session()
        if not session or not hmac.compare_digest(form.get('csrf', ''), self.app.csrf_token(session)):
            return self._send('Forbidden', HTTPStatus.FORBIDDEN, 'text/plain')
        actions = {
            '/admin/accounts': self.admin_accounts_action,
            '/admin/servers': self.admin_servers_action,
            '/admin/settings': self.admin_settings_save,
        }
        if url.path in actions:
            return actions[url.path](form)
        self._send('Not found', HTTPStatus.NOT_FOUND, 'text/plain')

    # -- public ------------------------------------------------------------

    def public_index(self, query):
        stats = self.app.stats()
        motd = self.app.store.setting('motd')
        body = []
        if motd:
            body.append(f'<div class="notice">{_e(motd)}</div>')
        body.append('<div class="stats">' + ''.join(
            f'<div class="stat"><span class="muted">{_e(g["title"])}</span><b>{g["servers"]}</b>'
            f'<span class="muted">серверов, игроков: {g["players"]}</span></div>' for g in stats['games'].values())
            + '</div><br>')
        for game, title in GAMES.items():
            rows = self.app.server_rows(game)
            body.append(f'<div class="panel"><h2>{_e(title)}</h2>')
            if not rows:
                body.append('<p class="muted">Сейчас нет серверов.</p></div>')
                continue
            body.append('<table><tr><th>Название</th><th>Карта</th><th>Режим</th><th>Игроки</th>'
                        '<th>Адрес</th><th>Пинг</th></tr>')
            for row in rows:
                lock = ' 🔒' if row['password'] else ''
                ping = f'{row["ping_ms"]} мс' if row['ping_ms'] >= 0 else '<span class="muted">нет ответа</span>'
                body.append(f'<tr><td>{_e(row["name"] or "—")}{lock}</td><td>{_e(row["map"])}</td>'
                            f'<td>{_e(row["gametype"])}</td><td>{row["players"]}/{row["max_players"]}</td>'
                            f'<td>{_e(row["address"])}</td><td>{ping}</td></tr>')
            body.append('</table></div>')
        body.append('<p class="muted">Аккаунт нужен для сетевой игры в Зов Припяти. '
                    '<a href="/register" style="color:var(--accent)">Зарегистрироваться</a></p>')
        self._send(self._page('Серверы', '<h1>Серверы</h1>' + ''.join(body)))

    def register_form(self, query, message='', ok=False, values=None):
        values = values or {}
        if self.app.store.setting('allow_registration') != '1':
            return self._send(self._page('Регистрация', '<h1>Регистрация</h1>'
                                         '<div class="notice">Регистрация отключена администратором.</div>'))
        notice = f'<div class="notice {"ok" if ok else "bad"}">{_e(message)}</div>' if message else ''
        body = (f'<h1>Регистрация</h1>{notice}<div class="panel"><form method="post" action="/register">'
                f'<label>E-mail</label><input name="email" type="email" required maxlength="50" value="{_e(values.get("email", ""))}">'
                f'<label>Ник в игре</label><input name="nick" required maxlength="20" value="{_e(values.get("nick", ""))}">'
                f'<label>Пароль (до 30 символов)</label><input name="password" type="password" required maxlength="30">'
                f'<label>Пароль ещё раз</label><input name="password2" type="password" required maxlength="30"><br><br>'
                f'<button>Создать аккаунт</button></form>'
                f'<p class="muted">В игре войдите с этим e-mail, ником и паролем. Пароль хранится так, как требует '
                f'протокол GameSpy (MD5), поэтому не используйте пароль от других сервисов.</p></div>')
        self._send(self._page('Регистрация', body))

    def register_submit(self, form):
        values = {'email': form.get('email', '').strip(), 'nick': form.get('nick', '').strip()}
        if form.get('password', '') != form.get('password2', ''):
            return self.register_form({}, 'Пароли не совпадают.', values=values)
        fields = dict(values, uniquenick=values['nick'], gamename='web')
        code, _, _ = gp.create_account(self.app.store, fields, password=form.get('password', ''))
        if code:
            return self.register_form({}, ERROR_TEXT.get(code, 'Не удалось создать аккаунт.'), values=values)
        self.register_form({}, 'Аккаунт создан. Теперь войдите в игре.', ok=True)

    def api_servers(self, query):
        game = query.get('game')
        self._send(json.dumps(self.app.server_rows(game if game in GAMES else None), ensure_ascii=False),
                   content_type='application/json; charset=utf-8', headers={'Access-Control-Allow-Origin': '*'})

    def api_stats(self, query):
        stats = self.app.stats()
        stats.pop('accounts', None)
        self._send(json.dumps(stats, ensure_ascii=False), content_type='application/json; charset=utf-8',
                   headers={'Access-Control-Allow-Origin': '*'})

    # -- admin -------------------------------------------------------------

    def admin_login_form(self, query, message=''):
        notice = f'<div class="notice bad">{_e(message)}</div>' if message else ''
        body = (f'<h1>Вход администратора</h1>{notice}<div class="panel"><form method="post" action="/admin/login">'
                '<label>Пароль</label><input name="password" type="password" required autofocus><br><br>'
                '<button>Войти</button></form></div>')
        self._send(self._page('Вход', body))

    def admin_login_submit(self, form):
        stored = self.app.config.get('admin_password', '')
        if stored and check_web_password(form.get('password', ''), stored):
            token = self.app.make_session()
            self.app.store.log('admin', '', f'login from {self.client_address[0]}')
            return self._redirect('/admin', {'Set-Cookie': f'xo_admin={token}; Path=/; HttpOnly; SameSite=Strict; '
                                                           f'Max-Age={SESSION_TTL}'})
        time.sleep(1)
        self.admin_login_form({}, 'Неверный пароль.')

    def admin_logout(self, query):
        self._redirect('/', {'Set-Cookie': 'xo_admin=; Path=/; Max-Age=0'})

    def _csrf_field(self, session):
        return f'<input type="hidden" name="csrf" value="{self.app.csrf_token(session)}">'

    def admin_dashboard(self, query, session):
        stats = self.app.stats()
        accounts = stats['accounts']
        tiles = [('Серверов', stats['servers']), ('Игроков на серверах', stats['players']),
                 ('Аккаунтов в сети', stats['accounts_online']), ('Аккаунтов всего', accounts['users']),
                 ('Входов за сутки', stats['logins_24h']), ('Регистраций за сутки', stats['registrations_24h']),
                 ('Забанено', accounts['banned']), ('Аптайм, ч', round(stats['uptime_s'] / 3600, 1))]
        body = ['<h1>Сводка</h1><div class="stats">'] + [
            f'<div class="stat"><span class="muted">{_e(n)}</span><b>{_e(v)}</b></div>' for n, v in tiles]
        body.append('</div><br><div class="panel"><h2>По играм</h2><table><tr><th>Игра</th><th>Серверы</th><th>Игроки</th></tr>')
        for game in stats['games'].values():
            body.append(f'<tr><td>{_e(game["title"])}</td><td>{game["servers"]}</td><td>{game["players"]}</td></tr>')
        body.append('</table></div><div class="panel"><h2>В сети (аккаунты)</h2><table>'
                    '<tr><th>Ник</th><th>IP</th><th>Игра</th><th>С</th></tr>')
        for s in self.app.sessions.all():
            body.append(f'<tr><td>{_e(s["uniquenick"])}</td><td>{_e(s["ip"])}</td><td>{_e(s["game"])}</td>'
                        f'<td>{_fmt_time(s["since"])}</td></tr>')
        body.append('</table></div><div class="panel"><h2>События</h2><table><tr><th>Время</th><th>Тип</th><th>Игра</th><th></th></tr>')
        for event in self.app.store.recent_events(60):
            body.append(f'<tr><td>{_fmt_time(event["time"])}</td><td>{_e(event["kind"])}</td>'
                        f'<td>{_e(event["game"])}</td><td>{_e(event["detail"])}</td></tr>')
        body.append('</table></div>')
        self._send(self._page('Сводка', ''.join(body), admin=True))

    def admin_servers(self, query, session):
        csrf = self._csrf_field(session)
        body = ['<h1>Серверы</h1><div class="panel"><table><tr><th>Игра</th><th>Название</th><th>Карта</th>'
                '<th>Игроки</th><th>Адрес</th><th>Пинг</th><th>В сети с</th><th></th></tr>']
        for row in self.app.server_rows():
            ip = row['address'].split(':')[0]
            body.append(f'<tr><td>{_e(GAMES[row["game"]])}</td><td>{_e(row["name"])}</td><td>{_e(row["map"])}</td>'
                        f'<td>{row["players"]}/{row["max_players"]}</td><td>{_e(row["address"])}</td>'
                        f'<td>{row["ping_ms"]}</td><td>{_fmt_time(row["online_since"])}</td>'
                        f'<td><form class="inline" method="post">{csrf}<input type="hidden" name="action" value="ban">'
                        f'<input type="hidden" name="ip" value="{_e(ip)}"><button class="danger">Бан IP</button></form></td></tr>')
        body.append(f'</table></div><div class="panel"><h2>Баны по IP</h2><form method="post">{csrf}'
                    '<input type="hidden" name="action" value="ban"><input name="ip" placeholder="IP" required> '
                    '<input name="reason" placeholder="Причина"> <button class="danger">Забанить</button></form><br>'
                    '<table><tr><th>IP</th><th>Причина</th><th>Когда</th><th></th></tr>')
        for ban in self.app.store.ip_bans():
            body.append(f'<tr><td>{_e(ban["ip"])}</td><td>{_e(ban["reason"])}</td><td>{_fmt_time(ban["created"])}</td>'
                        f'<td><form class="inline" method="post">{csrf}<input type="hidden" name="action" value="unban">'
                        f'<input type="hidden" name="ip" value="{_e(ban["ip"])}"><button>Снять</button></form></td></tr>')
        body.append('</table></div>')
        self._send(self._page('Серверы', ''.join(body), admin=True))

    def admin_servers_action(self, form):
        ip = form.get('ip', '').strip()
        if form.get('action') == 'ban' and ip:
            self.app.store.ban_ip(ip, form.get('reason', ''))
            for server in self.app.registry.servers():
                if server.ip == ip:
                    self.app.registry.remove(server.game, server.ip, server.port)
            self.app.store.log('admin', '', f'ban ip {ip}')
        elif form.get('action') == 'unban' and ip:
            self.app.store.unban_ip(ip)
            self.app.store.log('admin', '', f'unban ip {ip}')
        self._redirect('/admin/servers')

    def admin_accounts(self, query, session):
        csrf = self._csrf_field(session)
        search = query.get('q', '')
        body = [f'<h1>Аккаунты</h1><div class="panel"><form method="get"><input name="q" value="{_e(search)}" '
                'placeholder="e-mail или ник"> <button>Найти</button></form><br><table><tr><th>ID</th><th>E-mail</th>'
                '<th>Ники</th><th>Создан</th><th>Последний вход</th><th>Входов</th><th>Статус</th><th></th></tr>']
        for account in self.app.store.list_accounts(search):
            status = f'<span class="bad">бан: {_e(account["ban_reason"])}</span>' if account['banned'] else '<span class="ok">ок</span>'
            ban_action = 'unban' if account['banned'] else 'ban'
            ban_label = 'Разбанить' if account['banned'] else 'Забанить'
            body.append(
                f'<tr><td>{account["id"]}</td><td>{_e(account["email"])}</td><td>{_e(account["nicks"] or "")}</td>'
                f'<td>{_fmt_time(account["created"])}</td><td>{_fmt_time(account["last_login"])}</td>'
                f'<td>{account["logins"] or 0}</td><td>{status}</td><td>'
                f'<form class="inline" method="post">{csrf}<input type="hidden" name="user_id" value="{account["id"]}">'
                f'<input type="hidden" name="action" value="{ban_action}">'
                + ('' if account['banned'] else '<input name="reason" placeholder="причина" size="10"> ')
                + f'<button>{ban_label}</button></form> '
                f'<form class="inline" method="post" onsubmit="return confirm(\'Удалить аккаунт?\')">{csrf}'
                f'<input type="hidden" name="user_id" value="{account["id"]}"><input type="hidden" name="action" value="delete">'
                f'<button class="danger">Удалить</button></form></td></tr>')
        body.append('</table></div>')
        self._send(self._page('Аккаунты', ''.join(body), admin=True))

    def admin_accounts_action(self, form):
        try:
            user_id = int(form.get('user_id', '0'))
        except ValueError:
            return self._redirect('/admin/accounts')
        action = form.get('action')
        if action == 'ban':
            self.app.store.set_banned(user_id, True, form.get('reason', '').strip() or 'Забанен администратором')
        elif action == 'unban':
            self.app.store.set_banned(user_id, False)
        elif action == 'delete':
            self.app.store.delete_user(user_id)
        self.app.store.log('admin', '', f'{action} account {user_id}')
        self._redirect('/admin/accounts')

    def admin_settings(self, query, session, saved=False):
        settings = self.app.store.settings()
        checked = 'checked' if settings.get('allow_registration') == '1' else ''
        notice = '<div class="notice ok">Сохранено.</div>' if saved else ''
        body = (f'<h1>Настройки</h1>{notice}<div class="panel"><form method="post">{self._csrf_field(session)}'
                f'<label>Сообщение на главной (MOTD)</label><textarea name="motd" rows="3" cols="60">{_e(settings.get("motd", ""))}</textarea>'
                f'<label><input type="checkbox" name="allow_registration" value="1" {checked}> Разрешить регистрацию (сайт и игра)</label>'
                f'<label>Сервер пропадает из списка без heartbeat через, сек</label>'
                f'<input name="server_timeout" type="number" min="30" max="3600" value="{_e(settings.get("server_timeout", "90"))}">'
                f'<label>Максимум серверов с одного IP (0 — без ограничения)</label>'
                f'<input name="max_servers_per_ip" type="number" min="0" max="1000" value="{_e(settings.get("max_servers_per_ip", "8"))}">'
                f'<br><br><button>Сохранить</button></form></div>')
        self._send(self._page('Настройки', body, admin=True))

    def admin_settings_save(self, form):
        def bounded(name, low, high, default):
            try:
                return str(min(max(int(form.get(name, default)), low), high))
            except ValueError:
                return str(default)
        self.app.store.set_settings({
            'motd': form.get('motd', '')[:500],
            'allow_registration': '1' if form.get('allow_registration') == '1' else '0',
            'server_timeout': bounded('server_timeout', 30, 3600, 90),
            'max_servers_per_ip': bounded('max_servers_per_ip', 0, 1000, 8),
        })
        self.app.store.log('admin', '', 'settings changed')
        self.admin_settings({}, self._session(), saved=True)


def start(app, host, port):
    handler = type('BoundHandler', (Handler,), {'app': app})
    httpd = ThreadingHTTPServer((host, port), handler)
    thread = threading.Thread(target=httpd.serve_forever, name='web', daemon=True)
    thread.start()
    log.info('web interface on http://%s:%d/', host, port)
    return httpd
