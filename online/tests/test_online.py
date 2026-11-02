"""End-to-end tests: real sockets against the services, acting like the engine client."""
import asyncio
import http.client
import os
import socket
import sys
import tempfile
import time
import unittest
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from xray_online import gp, master, web  # noqa: E402
from xray_online import protocol as p  # noqa: E402
from xray_online.store import Store, hash_web_password  # noqa: E402


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class ProtocolTest(unittest.TestCase):
    def test_kv_round_trip(self):
        message = p.build_kv(('lc', 1), ('challenge', 'ABC'), ('id', 1))
        self.assertEqual(message, '\\lc\\1\\challenge\\ABC\\id\\1\\final\\')
        self.assertEqual(p.parse_kv(message), [('lc', '1'), ('challenge', 'ABC'), ('id', '1')])

    def test_password_encoding_round_trip(self):
        for password in ('secret', 'p@ss w0rd!', 'x' * 30):
            self.assertEqual(p.gs_password_decode(p.gs_password_encode(password)), password)

    def test_password_encoding_matches_sdk(self):
        # Produced by the SDK's own gpiEncodeString (compiled from GP/gpiUtility.c).
        self.assertEqual(p.gs_password_encode('secret'), 'ZZeXgE6v')
        self.assertEqual(p.gs_password_encode('p@ss w0rd!'), 'ZrKHgQus3N4dLg__')

    def test_uniquenick_rules(self):
        self.assertTrue(p.valid_uniquenick('Strelok_1'))
        self.assertFalse(p.valid_uniquenick('1abc'))
        self.assertFalse(p.valid_uniquenick('a' * 21))
        self.assertFalse(p.valid_uniquenick('bad\\nick'))


class ServicesTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.store = Store(os.path.join(self.tmp.name, 'db.sqlite3'))
        self.registry = master.Registry(self.store)
        self.sessions = gp.Sessions()
        self.master_port, self.gpcm_port, self.gpsp_port, self.web_port = (free_port() for _ in range(4))
        self.tcp, self.udp, self.maintenance = await master.start(
            self.registry, self.store, '127.0.0.1', self.master_port)
        self.gpcm_server, self.gpsp_server = await gp.start(self.store, self.sessions, '127.0.0.1', self.gpcm_port, self.gpsp_port)
        config = {'secret': 'test-secret', 'admin_password': hash_web_password('admin-pass')}
        self.httpd = web.start(web.WebApp(self.store, self.registry, self.sessions, config), '127.0.0.1', self.web_port)

    async def asyncTearDown(self):
        self.maintenance.cancel()
        for server in (self.tcp, self.gpcm_server, self.gpsp_server):
            server.close()
        self.udp.close()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.tmp.cleanup()

    # -- helpers -------------------------------------------------------------

    async def master_request(self, line):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.master_port)
        writer.write(line.encode() + b'\n')
        await writer.drain()
        data = await reader.read()
        writer.close()
        return data.decode()

    def udp_send(self, text):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(text.encode(), ('127.0.0.1', self.master_port))

    async def gpsp(self, *pairs):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.gpsp_port)
        writer.write(p.build_kv(*pairs).encode('latin-1'))
        await writer.drain()
        data = await reader.read()
        writer.close()
        return p.parse_kv(data.decode('latin-1'))

    async def gp_login(self, nick, email, password):
        """Does what gpiSendLogin/gpiProcessConnect do; returns (reply pairs, reader, writer)."""
        reader, writer = await asyncio.open_connection('127.0.0.1', self.gpcm_port)
        hello = dict(p.parse_kv((await reader.readuntil(b'\\final\\')).decode('latin-1')))
        self.assertEqual(hello['lc'], '1')
        client_challenge = 'C' * 32
        user = p.gp_login_user(nick, email, '')
        response = p.gp_login_response(p.md5_hex(password), user, client_challenge, hello['challenge'])
        writer.write(p.build_kv(('login', ''), ('challenge', client_challenge), ('user', f'{nick}@{email}'),
                                ('partnerid', 0), ('response', response), ('firewall', 1), ('port', 0),
                                ('productid', 11994), ('gamename', 'stalkercoppc'), ('namespaceid', 1),
                                ('id', 1)).encode('latin-1'))
        await writer.drain()
        reply = p.parse_kv((await reader.readuntil(b'\\final\\')).decode('latin-1'))
        return reply, hello['challenge'], client_challenge, reader, writer

    # -- master --------------------------------------------------------------

    async def test_master_ping_and_list(self):
        self.assertEqual(await self.master_request('PING'), 'PONG\n')
        self.udp_send('REGISTER stalkercoppc 5445')
        self.udp_send('REGISTER stalkersc 5446')
        await asyncio.sleep(0.2)
        cop = await self.master_request('LIST stalkercoppc')
        self.assertEqual(cop, 'XRAYMASTER 1\n127.0.0.1 5445\n.\n')
        soc = await self.master_request('LIST stalkersc')
        self.assertIn('127.0.0.1 5446', soc)
        self.udp_send('UNREGISTER stalkercoppc 5445')
        await asyncio.sleep(0.2)
        self.assertEqual(await self.master_request('LIST stalkercoppc'), 'XRAYMASTER 1\n.\n')

    async def test_master_ignores_unknown_games_and_banned_ips(self):
        self.udp_send('REGISTER quake3 27960')
        self.store.ban_ip('127.0.0.1')
        self.udp_send('REGISTER stalkercs 5447')
        await asyncio.sleep(0.2)
        self.assertEqual(self.registry.servers(), [])

    async def test_master_expires_silent_servers(self):
        self.store.set_settings({'server_timeout': '30'})
        self.registry.heartbeat('stalkercs', '10.0.0.1', 5445)
        self.registry.servers()[0].last_seen = time.time() - 31
        self.registry.expire()
        self.assertEqual(self.registry.servers(), [])

    # -- GP ------------------------------------------------------------------

    async def test_account_lifecycle(self):
        # gpNewUser (search manager)
        reply = dict(await self.gpsp(('newuser', ''), ('nick', 'Strelok'), ('email', 'strelok@zone.net'),
                                     ('passenc', p.gs_password_encode('monolith')), ('productID', 11994),
                                     ('namespaceid', 1), ('uniquenick', 'Strelok'), ('partnerid', 0),
                                     ('gamename', 'stalkercoppc')))
        self.assertEqual(reply['nur'], '0')
        profile_id = int(reply['pid'])

        # The same uniquenick again is refused.
        again = dict(await self.gpsp(('newuser', ''), ('nick', 'Strelok'), ('email', 'other@zone.net'),
                                     ('passenc', p.gs_password_encode('x1')), ('uniquenick', 'Strelok')))
        self.assertEqual(int(again['nur']), p.GP_NEWUSER_UNIQUENICK_INUSE)

        # gpConnect: login, then verify the server proof like the SDK does.
        reply, server_challenge, client_challenge, reader, writer = await self.gp_login(
            'Strelok', 'strelok@zone.net', 'monolith')
        fields = dict(reply)
        self.assertIn('lc', fields, reply)
        self.assertEqual(fields['lc'], '2')
        self.assertEqual(int(fields['profileid']), profile_id)
        expected_proof = p.gp_login_proof(p.md5_hex('monolith'), 'Strelok@strelok@zone.net',
                                          client_challenge, server_challenge)
        self.assertEqual(fields['proof'], expected_proof)
        self.assertEqual(len(fields['lt']), p.GP_LOGIN_TICKET_LEN - 1)
        self.assertEqual(len(self.sessions.all()), 1)

        # gpRegisterUniqueNick on the logged-in connection.
        writer.write(p.build_kv(('registernick', ''), ('sesskey', fields['sesskey']),
                                ('uniquenick', 'Strelok_2'), ('id', 7)).encode('latin-1'))
        await writer.drain()
        renamed = dict(p.parse_kv((await reader.readuntil(b'\\final\\')).decode('latin-1')))
        self.assertEqual(renamed, {'rn': 'Strelok_2', 'id': '7'})
        writer.close()

        # gpGetUserNicks
        nicks = await self.gpsp(('nicks', ''), ('email', 'strelok@zone.net'),
                                ('passenc', p.gs_password_encode('monolith')), ('namespaceid', 1))
        self.assertEqual(nicks[0][0], 'nr')
        self.assertIn(('uniquenick', 'Strelok_2'), nicks)
        self.assertEqual(nicks[-1][0], 'ndone')

        # gpSuggestUniqueNick
        suggestions = await self.gpsp(('uniquesearch', ''), ('preferrednick', 'Strelok_2'), ('namespaceid', 1))
        self.assertEqual(suggestions[0], ('us', '5'))
        self.assertNotIn(('nick', 'Strelok_2'), suggestions)

        # gpProfileSearch
        found = await self.gpsp(('search', ''), ('sesskey', '1'), ('profileid', profile_id), ('namespaceid', 1),
                                ('uniquenick', 'Strelok_2'), ('gamename', 'stalkercoppc'))
        self.assertEqual(found[0], ('bsr', str(profile_id)))

    async def test_login_failures(self):
        self.store.create_account('a@b.cd', 'right', 'Degtyarev')
        reply = dict((await self.gp_login('Degtyarev', 'a@b.cd', 'wrong'))[0])
        self.assertEqual(int(reply['err']), p.GP_LOGIN_BAD_PASSWORD)
        reply = dict((await self.gp_login('Degtyarev', 'nobody@b.cd', 'right'))[0])
        self.assertEqual(int(reply['err']), p.GP_LOGIN_BAD_EMAIL)
        user = self.store.user_by_email('a@b.cd')
        self.store.set_banned(user['id'], True, 'cheating')
        reply = dict((await self.gp_login('Degtyarev', 'a@b.cd', 'right'))[0])
        self.assertEqual(int(reply['err']), p.GP_LOGIN_BAD_PROFILE)
        self.assertEqual(reply['errmsg'], 'cheating')

    async def test_registration_switch(self):
        self.store.set_settings({'allow_registration': '0'})
        reply = dict(await self.gpsp(('newuser', ''), ('nick', 'Sidor'), ('email', 's@v.ua'),
                                     ('passenc', p.gs_password_encode('pw')), ('uniquenick', 'Sidor')))
        self.assertEqual(int(reply['nur']), p.GP_DATABASE)

    # -- web -----------------------------------------------------------------

    def http(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.web_port, timeout=5)
        data = urllib.parse.urlencode(body).encode() if body is not None else None
        headers = dict(headers or {})
        if data is not None:
            headers['Content-Type'] = 'application/x-www-form-urlencoded'
        connection.request(method, path, data, headers)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read().decode()

    async def test_web_registration_and_admin(self):
        loop = asyncio.get_running_loop()
        status, _, page = await loop.run_in_executor(None, self.http, 'GET', '/')
        self.assertEqual(status, 200)
        self.assertIn('Call of Pripyat', page)

        status, _, page = await loop.run_in_executor(None, lambda: self.http(
            'POST', '/register', {'email': 'web@zone.net', 'nick': 'Webber', 'password': 'pw12345',
                                  'password2': 'pw12345'}))
        self.assertIn('Аккаунт создан', page)
        self.assertIsNotNone(self.store.find_login(nick='Webber', email='web@zone.net'))

        # The web-created account logs in over GP.
        reply = dict((await self.gp_login('Webber', 'web@zone.net', 'pw12345'))[0])
        self.assertEqual(reply.get('lc'), '2')

        status, headers, _ = await loop.run_in_executor(None, self.http, 'GET', '/admin')
        self.assertEqual((status, headers['Location']), (303, '/admin/login'))
        status, headers, _ = await loop.run_in_executor(None, lambda: self.http(
            'POST', '/admin/login', {'password': 'admin-pass'}))
        self.assertEqual(status, 303)
        cookie = headers['Set-Cookie'].split(';')[0]
        status, _, page = await loop.run_in_executor(None, lambda: self.http('GET', '/admin', headers={'Cookie': cookie}))
        self.assertEqual(status, 200)
        self.assertIn('Сводка', page)

        # State changes need the CSRF token.
        status, _, _ = await loop.run_in_executor(None, lambda: self.http(
            'POST', '/admin/settings', {'motd': 'hi'}, {'Cookie': cookie}))
        self.assertEqual(status, 403)
        status, _, page = await loop.run_in_executor(None, lambda: self.http(
            'GET', '/admin/settings', headers={'Cookie': cookie}))
        token = page.split('name="csrf" value="')[1].split('"')[0]
        status, _, _ = await loop.run_in_executor(None, lambda: self.http(
            'POST', '/admin/settings', {'csrf': token, 'motd': 'Добро пожаловать', 'allow_registration': '1',
                                        'server_timeout': '120', 'max_servers_per_ip': '4'}, {'Cookie': cookie}))
        self.assertEqual(status, 200)
        self.assertEqual(self.store.setting('motd'), 'Добро пожаловать')

        status, _, body = await loop.run_in_executor(None, self.http, 'GET', '/api/stats.json')
        self.assertEqual(status, 200)
        self.assertIn('"accounts_online"', body)


if __name__ == '__main__':
    unittest.main()
