"""GameSpy Presence: the account service Call of Pripyat logs in with.

gpcm (TCP 29900) is the connection manager: login and the requests of a
logged-in session (register a unique nick, delete a profile, keep-alive).
gpsp (TCP 29901) is the search manager: one request per connection, used for
account creation, nick lookups and suggestions.

Message formats follow the client SDK (Externals/GameSpy/src/GameSpy/GP).
"""
import asyncio
import logging
import threading
import time

from . import protocol as gp

log = logging.getLogger('gp')


class Sessions:
    """Logged-in GP sessions, for statistics and the admin page."""

    def __init__(self):
        self._lock = threading.Lock()
        self._sessions = {}

    def add(self, key, profile_id, uniquenick, ip, game):
        with self._lock:
            self._sessions[key] = {'profile_id': profile_id, 'uniquenick': uniquenick, 'ip': ip,
                                   'game': game, 'since': time.time()}

    def remove(self, key):
        with self._lock:
            self._sessions.pop(key, None)

    def all(self):
        with self._lock:
            return list(self._sessions.values())


def _error(code, message, fatal=True, operation_id=1):
    pairs = [('error', ''), ('err', code)]
    if fatal:
        pairs.append(('fatal', ''))
    pairs += [('errmsg', message), ('id', operation_id)]
    return gp.build_kv(*pairs)


async def _read_message(reader):
    """Reads one '\\final\\'-terminated message; the stream limit bounds its size."""
    data = await reader.readuntil(b'\\final\\')
    return data.decode('latin-1')


class ConnectionManager:
    def __init__(self, store, sessions):
        self.store = store
        self.sessions = sessions

    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername') or ('?', 0)
        ip = peer[0]
        session_key = None
        if self.store.ip_banned(ip):
            writer.close()
            return
        server_challenge = gp.random_challenge()
        writer.write(gp.build_kv(('lc', 1), ('challenge', server_challenge), ('id', 1)).encode('latin-1'))
        await writer.drain()
        profile = None
        try:
            while True:
                try:
                    message = await asyncio.wait_for(_read_message(reader), timeout=300)
                except asyncio.IncompleteReadError:
                    break
                pairs = gp.parse_kv(message)
                if not pairs:
                    continue
                command = pairs[0][0]
                fields = dict(pairs[1:])
                operation_id = int(fields.get('id', '1') or 1)

                if command == 'login':
                    reply, profile = self._login(fields, server_challenge, ip)
                    if profile:
                        session_key = (id(writer), profile['id'])
                        self.sessions.add(session_key, profile['id'], profile['uniquenick'], ip,
                                          fields.get('gamename', ''))
                elif command == 'newuser':
                    reply = self._newuser_then_login_hint(fields, operation_id)
                elif profile is None:
                    reply = _error(gp.GP_LOGIN_BAD_PROFILE, 'Not logged in.', operation_id=operation_id)
                elif command == 'registernick':
                    reply = self._register_nick(profile, fields, operation_id)
                elif command == 'delprofile':
                    reply = self._delete_profile(profile, operation_id)
                elif command == 'getprofile':
                    reply = self._get_profile(fields, operation_id)
                elif command == 'ka':
                    reply = gp.build_kv(('ka', ''))
                elif command == 'logout':
                    break
                else:
                    reply = None  # status, buddy and other optional messages
                if reply:
                    writer.write(reply.encode('latin-1'))
                    await writer.drain()
                if command == 'login' and profile is None:
                    break
        except (asyncio.TimeoutError, ConnectionError, ValueError, asyncio.LimitOverrunError):
            pass
        finally:
            if session_key:
                self.sessions.remove(session_key)
            writer.close()

    def _login(self, fields, server_challenge, ip):
        client_challenge = fields.get('challenge', '')
        partner_id = int(fields.get('partnerid', '0') or 0)
        uniquenick = fields.get('uniquenick', '')
        nick, email = '', ''
        if not uniquenick:
            user = fields.get('user', '')
            if '@' not in user:
                return _error(gp.GP_LOGIN_BAD_NICK, 'Invalid user.'), None
            # The partner prefix only enters the hash, never the field.
            nick, email = user.split('@', 1)

        account = self.store.find_login(nick=nick, email=email, uniquenick=uniquenick)
        if account is None:
            if uniquenick:
                return _error(gp.GP_LOGIN_BAD_UNIQUENICK, 'Unknown nickname.'), None
            if self.store.user_by_email(email) is None:
                return _error(gp.GP_LOGIN_BAD_EMAIL, 'Unknown e-mail.'), None
            return _error(gp.GP_LOGIN_BAD_NICK, 'Unknown profile for this e-mail.'), None
        if account['deleted']:
            return _error(gp.GP_LOGIN_PROFILE_DELETED, 'This profile was deleted.'), None
        if account['banned']:
            reason = account['ban_reason'] or 'This account is banned.'
            return _error(gp.GP_LOGIN_BAD_PROFILE, reason), None

        user = gp.gp_login_user(nick, email, uniquenick, partner_id)
        expected = gp.gp_login_response(account['password_md5'], user, client_challenge, server_challenge)
        if fields.get('response', '') != expected:
            return _error(gp.GP_LOGIN_BAD_PASSWORD, 'Wrong password.'), None

        self.store.record_login(account['id'], ip)
        self.store.log('login', fields.get('gamename', ''), account['uniquenick'])
        proof = gp.gp_login_proof(account['password_md5'], user, client_challenge, server_challenge)
        reply = gp.build_kv(('lc', 2), ('sesskey', gp.session_key()), ('proof', proof),
                            ('userid', account['user_id']), ('profileid', account['id']),
                            ('uniquenick', account['uniquenick']), ('lt', gp.login_ticket()), ('id', 1))
        return reply, account

    def _newuser_then_login_hint(self, fields, operation_id):
        # gpConnectNewUser: create, then the client sends \login\.
        code, user_id, profile_id = create_account(self.store, fields)
        if code:
            return _error(code, _NEWUSER_MESSAGES.get(code, 'Cannot create the account.'), operation_id=operation_id)
        return gp.build_kv(('nur', ''), ('userid', user_id), ('profileid', profile_id), ('id', operation_id))

    def _register_nick(self, profile, fields, operation_id):
        uniquenick = fields.get('uniquenick', '')
        if not gp.valid_uniquenick(uniquenick):
            return _error(gp.GP_NEWUSER_UNIQUENICK_INVALID, 'Invalid nickname.', False, operation_id)
        try:
            self.store.set_uniquenick(profile['id'], uniquenick)
        except ValueError:
            return _error(gp.GP_REGISTERUNIQUENICK_TAKEN, 'This nickname is taken.', False, operation_id)
        profile['uniquenick'] = uniquenick
        return gp.build_kv(('rn', uniquenick), ('id', operation_id))

    def _delete_profile(self, profile, operation_id):
        try:
            self.store.delete_profile(profile['id'])
        except ValueError:
            return _error(gp.GP_DELPROFILE_LAST_PROFILE, 'Cannot delete the last profile.', False, operation_id)
        return gp.build_kv(('dpr', 1), ('id', operation_id))

    def _get_profile(self, fields, operation_id):
        target = self.store.profile(int(fields.get('profileid', '0') or 0))
        if not target:
            return _error(0x0A01, 'Unknown profile.', False, operation_id)
        return gp.build_kv(('pi', ''), ('profileid', target['id']), ('nick', target['nick']),
                           ('uniquenick', target['uniquenick']), ('email', target['email']),
                           ('sig', '0'), ('id', operation_id))


_NEWUSER_MESSAGES = {
    gp.GP_NEWUSER_BAD_NICK: 'This e-mail already has a profile with that nick.',
    gp.GP_NEWUSER_BAD_PASSWORD: 'This e-mail is registered with a different password.',
    gp.GP_NEWUSER_UNIQUENICK_INVALID: 'Invalid nickname.',
    gp.GP_NEWUSER_UNIQUENICK_INUSE: 'This nickname is taken.',
    gp.GP_DATABASE: 'Registration is disabled on this server.',
}


def create_account(store, fields, password=None):
    """GameSpy newuser semantics: a new e-mail creates a user and profile; an
    existing e-mail with the right password gets an extra profile.
    Returns (error_code, user_id, profile_id)."""
    if store.setting('allow_registration') != '1':
        return gp.GP_DATABASE, 0, 0
    email = fields.get('email', '').strip()
    nick = fields.get('nick', '').strip()
    uniquenick = fields.get('uniquenick', '').strip() or nick
    if password is None:
        encoded = fields.get('passwordenc') or fields.get('passenc') or ''
        try:
            password = gp.gs_password_decode(encoded)
        except (ValueError, UnicodeDecodeError):
            return gp.GP_NEWUSER_BAD_PASSWORD, 0, 0
    if not gp.valid_email(email) or not 1 <= len(nick) < gp.GP_NICK_LEN or '\\' in nick:
        return gp.GP_NEWUSER_BAD_NICK, 0, 0
    if not gp.valid_uniquenick(uniquenick):
        return gp.GP_NEWUSER_UNIQUENICK_INVALID, 0, 0
    if not password or len(password) > 30:
        return gp.GP_NEWUSER_BAD_PASSWORD, 0, 0

    existing = store.user_by_email(email)
    try:
        if existing is None:
            user_id, profile_id = store.create_account(email, password, nick, uniquenick)
        else:
            if existing['password_md5'] != gp.md5_hex(password):
                return gp.GP_NEWUSER_BAD_PASSWORD, 0, 0
            if any(p['nick'] == nick for p in store.profiles_of(existing['id'])):
                return gp.GP_NEWUSER_BAD_NICK, 0, 0
            user_id = existing['id']
            profile_id = store.add_profile(user_id, nick, uniquenick)
    except ValueError as error:
        if str(error) == 'uniquenick':
            return gp.GP_NEWUSER_UNIQUENICK_INUSE, 0, 0
        return gp.GP_NEWUSER_BAD_PASSWORD, 0, 0
    store.log('register', fields.get('gamename', ''), f'{uniquenick} <{email}>')
    return 0, user_id, profile_id


class SearchManager:
    def __init__(self, store):
        self.store = store

    async def handle(self, reader, writer):
        try:
            peer = writer.get_extra_info('peername') or ('?', 0)
            if self.store.ip_banned(peer[0]):
                return
            message = await asyncio.wait_for(_read_message(reader), timeout=20)
            pairs = gp.parse_kv(message)
            if pairs:
                reply = self.reply(pairs[0][0], dict(pairs[1:]))
                writer.write(reply.encode('latin-1'))
                await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError,
                asyncio.LimitOverrunError):
            pass
        finally:
            writer.close()

    def reply(self, command, fields):
        if command in ('search', 'searchunique'):
            matches = self.store.search_profiles(nick=fields.get('nick', ''),
                                                 uniquenick=fields.get('uniquenick', ''),
                                                 email=fields.get('email', ''))
            pairs = []
            for match in matches:
                pairs += [('bsr', match['id']), ('nick', match['nick']), ('uniquenick', match['uniquenick']),
                          ('namespaceid', 1), ('firstname', ''), ('lastname', ''), ('email', match['email'])]
            return gp.build_kv(*pairs, ('bsrdone', ''), ('more', 0))
        if command == 'nicks':
            user = self.store.user_by_email(fields.get('email', ''))
            if user is None:
                return _error(gp.GP_CHECK_BAD_EMAIL, 'Unknown e-mail.')
            if not self._password_ok(user, fields):
                return _error(gp.GP_CHECK_BAD_PASSWORD, 'Wrong password.')
            pairs = [('nr', len(self.store.profiles_of(user['id'])))]
            for profile in self.store.profiles_of(user['id']):
                pairs += [('nick', profile['nick']), ('uniquenick', profile['uniquenick'])]
            return gp.build_kv(*pairs, ('ndone', ''))
        if command == 'valid':
            return gp.build_kv(('vr', 1 if self.store.user_by_email(fields.get('email', '')) else 0))
        if command == 'check':
            user = self.store.user_by_email(fields.get('email', ''))
            if user is None:
                return gp.build_kv(('cur', gp.GP_CHECK_BAD_EMAIL))
            if not self._password_ok(user, fields):
                return gp.build_kv(('cur', gp.GP_CHECK_BAD_PASSWORD))
            profile = next((p for p in self.store.profiles_of(user['id']) if p['nick'] == fields.get('nick')), None)
            if profile is None:
                return gp.build_kv(('cur', gp.GP_CHECK_BAD_NICK))
            return gp.build_kv(('cur', 0), ('pid', profile['id']))
        if command == 'newuser':
            code, _, profile_id = create_account(self.store, fields)
            if code:
                return gp.build_kv(('nur', code))
            return gp.build_kv(('nur', 0), ('pid', profile_id))
        if command == 'uniquesearch':
            preferred = fields.get('preferrednick', '')[:gp.GP_UNIQUENICK_LEN - 4] or 'stalker'
            suggestions = []
            for suffix in [''] + [str(n) for n in range(1, 1000)]:
                candidate = preferred + suffix
                if gp.valid_uniquenick(candidate) and not self.store.uniquenick_taken(candidate):
                    suggestions.append(candidate)
                if len(suggestions) == 5:
                    break
            pairs = [('us', len(suggestions))] + [('nick', nick) for nick in suggestions]
            return gp.build_kv(*pairs, ('usdone', ''))
        if command == 'others':
            return gp.build_kv(('others', ''), ('odone', ''))
        if command == 'otherslist':
            return gp.build_kv(('otherslist', ''), ('oldone', ''))
        return _error(0, f'Unsupported request: {command}')

    @staticmethod
    def _password_ok(user, fields):
        encoded = fields.get('passenc') or fields.get('passwordenc') or ''
        try:
            return gp.md5_hex(gp.gs_password_decode(encoded)) == user['password_md5']
        except (ValueError, UnicodeDecodeError):
            return False


async def start(store, sessions, host, gpcm_port, gpsp_port):
    gpcm = await asyncio.start_server(ConnectionManager(store, sessions).handle, host, gpcm_port)
    gpsp = await asyncio.start_server(SearchManager(store).handle, host, gpsp_port)
    log.info('GP listening on %s:%d (gpcm) and %s:%d (gpsp)', host, gpcm_port, host, gpsp_port)
    return gpcm, gpsp
