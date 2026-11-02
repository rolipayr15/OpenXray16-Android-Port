"""SQLite persistence: accounts (GameSpy users and profiles), bans, settings, events.

One connection per thread; every method takes the store lock, so the asyncio
services and the web threads can share a Store.
"""
import hashlib
import hmac
import os
import sqlite3
import threading
import time

SCHEMA = '''
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY,
    email TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_md5 TEXT NOT NULL,      -- GP login needs md5(password); protocol-mandated
    password_web TEXT NOT NULL,      -- pbkdf2 for the web interface
    created INTEGER NOT NULL,
    banned INTEGER NOT NULL DEFAULT 0,
    ban_reason TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS profiles (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    nick TEXT NOT NULL,
    uniquenick TEXT NOT NULL UNIQUE COLLATE NOCASE,
    created INTEGER NOT NULL,
    last_login INTEGER NOT NULL DEFAULT 0,
    last_ip TEXT NOT NULL DEFAULT '',
    logins INTEGER NOT NULL DEFAULT 0,
    deleted INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS ip_bans (
    ip TEXT PRIMARY KEY,
    reason TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY,
    time INTEGER NOT NULL,
    kind TEXT NOT NULL,     -- login, register, server_up, server_down, admin
    game TEXT NOT NULL DEFAULT '',
    detail TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS events_time ON events(time);
'''

# Profile ids start high so they never look like the SDK's "0 = none".
FIRST_ID = 100000

DEFAULT_SETTINGS = {
    'motd': '',
    'allow_registration': '1',          # web and in-game registration
    'server_timeout': '90',             # seconds without heartbeat before a server is dropped
    'max_servers_per_ip': '8',
    'require_account_soc_cs': '0',      # informational; SoC/CS have no GP login
}

_PBKDF2_ROUNDS = 200_000


def hash_web_password(password, salt=None):
    salt = salt or os.urandom(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, _PBKDF2_ROUNDS)
    return f'pbkdf2${_PBKDF2_ROUNDS}${salt.hex()}${digest.hex()}'


def check_web_password(password, stored):
    try:
        _, rounds, salt, digest = stored.split('$')
        candidate = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), int(rounds))
        return hmac.compare_digest(candidate.hex(), digest)
    except ValueError:
        return False


class Store:
    def __init__(self, path):
        self.path = path
        self._lock = threading.RLock()
        self._local = threading.local()
        with self._lock:
            db = self._db()
            db.executescript(SCHEMA)
            for key, value in DEFAULT_SETTINGS.items():
                db.execute('INSERT OR IGNORE INTO settings(key, value) VALUES (?, ?)', (key, value))
            db.commit()

    def _db(self):
        db = getattr(self._local, 'db', None)
        if db is None:
            db = sqlite3.connect(self.path, timeout=10)
            db.row_factory = sqlite3.Row
            db.execute('PRAGMA foreign_keys = ON')
            db.execute('PRAGMA journal_mode = WAL')
            self._local.db = db
        return db

    # -- settings ------------------------------------------------------------

    def settings(self):
        with self._lock:
            return {row['key']: row['value'] for row in self._db().execute('SELECT key, value FROM settings')}

    def setting(self, key):
        return self.settings().get(key, DEFAULT_SETTINGS.get(key, ''))

    def set_settings(self, values):
        with self._lock:
            db = self._db()
            for key, value in values.items():
                if key in DEFAULT_SETTINGS:
                    db.execute('INSERT OR REPLACE INTO settings(key, value) VALUES (?, ?)', (key, str(value)))
            db.commit()

    # -- events --------------------------------------------------------------

    def log(self, kind, game='', detail=''):
        with self._lock:
            db = self._db()
            db.execute('INSERT INTO events(time, kind, game, detail) VALUES (?, ?, ?, ?)',
                       (int(time.time()), kind, game, detail[:300]))
            db.commit()

    def recent_events(self, limit=50):
        with self._lock:
            return [dict(row) for row in self._db().execute(
                'SELECT time, kind, game, detail FROM events ORDER BY id DESC LIMIT ?', (limit,))]

    def count_events(self, kind, since):
        with self._lock:
            return self._db().execute(
                'SELECT COUNT(*) FROM events WHERE kind = ? AND time >= ?', (kind, since)).fetchone()[0]

    # -- accounts ------------------------------------------------------------

    def create_account(self, email, password, nick, uniquenick=None):
        """Returns (user_id, profile_id); raises ValueError('email'|'uniquenick')."""
        uniquenick = uniquenick or nick
        now = int(time.time())
        with self._lock:
            db = self._db()
            if db.execute('SELECT 1 FROM users WHERE email = ?', (email,)).fetchone():
                raise ValueError('email')
            if db.execute('SELECT 1 FROM profiles WHERE uniquenick = ?', (uniquenick,)).fetchone():
                raise ValueError('uniquenick')
            user_id = max(FIRST_ID, (db.execute('SELECT MAX(id) FROM users').fetchone()[0] or 0) + 1)
            db.execute('INSERT INTO users(id, email, password_md5, password_web, created) VALUES (?, ?, ?, ?, ?)',
                       (user_id, email, hashlib.md5(password.encode('latin-1')).hexdigest(),
                        hash_web_password(password), now))
            profile_id = self._insert_profile(db, user_id, nick, uniquenick, now)
            db.commit()
            return user_id, profile_id

    def _insert_profile(self, db, user_id, nick, uniquenick, now):
        profile_id = max(FIRST_ID, (db.execute('SELECT MAX(id) FROM profiles').fetchone()[0] or 0) + 1)
        db.execute('INSERT INTO profiles(id, user_id, nick, uniquenick, created) VALUES (?, ?, ?, ?, ?)',
                   (profile_id, user_id, nick, uniquenick, now))
        return profile_id

    def add_profile(self, user_id, nick, uniquenick):
        with self._lock:
            db = self._db()
            if db.execute('SELECT 1 FROM profiles WHERE uniquenick = ?', (uniquenick,)).fetchone():
                raise ValueError('uniquenick')
            profile_id = self._insert_profile(db, user_id, nick, uniquenick, int(time.time()))
            db.commit()
            return profile_id

    def user_by_email(self, email):
        with self._lock:
            row = self._db().execute('SELECT * FROM users WHERE email = ?', (email,)).fetchone()
            return dict(row) if row else None

    def profiles_of(self, user_id, include_deleted=False):
        query = 'SELECT * FROM profiles WHERE user_id = ?' + ('' if include_deleted else ' AND deleted = 0')
        with self._lock:
            return [dict(row) for row in self._db().execute(query + ' ORDER BY id', (user_id,))]

    def find_login(self, nick=None, email=None, uniquenick=None):
        """Profile joined with its user for a GP login, or None."""
        with self._lock:
            db = self._db()
            if uniquenick:
                row = db.execute('''SELECT p.*, u.email, u.password_md5, u.banned, u.ban_reason FROM profiles p
                    JOIN users u ON u.id = p.user_id WHERE p.uniquenick = ?''', (uniquenick,)).fetchone()
            else:
                row = db.execute('''SELECT p.*, u.email, u.password_md5, u.banned, u.ban_reason FROM profiles p
                    JOIN users u ON u.id = p.user_id WHERE u.email = ? AND p.nick = ?''', (email, nick)).fetchone()
            return dict(row) if row else None

    def profile(self, profile_id):
        with self._lock:
            row = self._db().execute('''SELECT p.*, u.email FROM profiles p JOIN users u ON u.id = p.user_id
                WHERE p.id = ?''', (profile_id,)).fetchone()
            return dict(row) if row else None

    def search_profiles(self, nick='', uniquenick='', email='', limit=50):
        clauses, args = ['p.deleted = 0'], []
        if nick:
            clauses.append('p.nick = ?')
            args.append(nick)
        if uniquenick:
            clauses.append('p.uniquenick = ?')
            args.append(uniquenick)
        if email:
            clauses.append('u.email = ?')
            args.append(email)
        with self._lock:
            return [dict(row) for row in self._db().execute(
                'SELECT p.*, u.email FROM profiles p JOIN users u ON u.id = p.user_id WHERE '
                + ' AND '.join(clauses) + ' ORDER BY p.id LIMIT ?', (*args, limit))]

    def uniquenick_taken(self, uniquenick):
        with self._lock:
            return self._db().execute('SELECT 1 FROM profiles WHERE uniquenick = ?', (uniquenick,)).fetchone() is not None

    def set_uniquenick(self, profile_id, uniquenick):
        with self._lock:
            db = self._db()
            if db.execute('SELECT 1 FROM profiles WHERE uniquenick = ? AND id != ?',
                          (uniquenick, profile_id)).fetchone():
                raise ValueError('uniquenick')
            db.execute('UPDATE profiles SET uniquenick = ? WHERE id = ?', (uniquenick, profile_id))
            db.commit()

    def delete_profile(self, profile_id):
        """Soft delete; refuses the last live profile of a user."""
        with self._lock:
            db = self._db()
            row = db.execute('SELECT user_id FROM profiles WHERE id = ?', (profile_id,)).fetchone()
            if not row:
                return False
            live = db.execute('SELECT COUNT(*) FROM profiles WHERE user_id = ? AND deleted = 0',
                              (row['user_id'],)).fetchone()[0]
            if live <= 1:
                raise ValueError('last')
            db.execute('UPDATE profiles SET deleted = 1 WHERE id = ?', (profile_id,))
            db.commit()
            return True

    def record_login(self, profile_id, ip):
        with self._lock:
            db = self._db()
            db.execute('UPDATE profiles SET last_login = ?, last_ip = ?, logins = logins + 1 WHERE id = ?',
                       (int(time.time()), ip, profile_id))
            db.commit()

    # -- administration ------------------------------------------------------

    def list_accounts(self, query='', limit=200):
        like = f'%{query}%'
        with self._lock:
            return [dict(row) for row in self._db().execute('''
                SELECT u.id, u.email, u.created, u.banned, u.ban_reason,
                       GROUP_CONCAT(p.uniquenick, ', ') AS nicks, MAX(p.last_login) AS last_login,
                       SUM(p.logins) AS logins
                FROM users u LEFT JOIN profiles p ON p.user_id = u.id AND p.deleted = 0
                WHERE u.email LIKE ? OR p.uniquenick LIKE ?
                GROUP BY u.id ORDER BY u.id DESC LIMIT ?''', (like, like, limit))]

    def set_banned(self, user_id, banned, reason=''):
        with self._lock:
            db = self._db()
            db.execute('UPDATE users SET banned = ?, ban_reason = ? WHERE id = ?',
                       (1 if banned else 0, reason if banned else '', user_id))
            db.commit()

    def delete_user(self, user_id):
        with self._lock:
            db = self._db()
            db.execute('DELETE FROM users WHERE id = ?', (user_id,))
            db.commit()

    def ip_bans(self):
        with self._lock:
            return [dict(row) for row in self._db().execute('SELECT * FROM ip_bans ORDER BY created DESC')]

    def ip_banned(self, ip):
        with self._lock:
            return self._db().execute('SELECT 1 FROM ip_bans WHERE ip = ?', (ip,)).fetchone() is not None

    def ban_ip(self, ip, reason=''):
        with self._lock:
            db = self._db()
            db.execute('INSERT OR REPLACE INTO ip_bans(ip, reason, created) VALUES (?, ?, ?)',
                       (ip, reason, int(time.time())))
            db.commit()

    def unban_ip(self, ip):
        with self._lock:
            db = self._db()
            db.execute('DELETE FROM ip_bans WHERE ip = ?', (ip,))
            db.commit()

    def account_totals(self):
        with self._lock:
            db = self._db()
            return {
                'users': db.execute('SELECT COUNT(*) FROM users').fetchone()[0],
                'profiles': db.execute('SELECT COUNT(*) FROM profiles WHERE deleted = 0').fetchone()[0],
                'banned': db.execute('SELECT COUNT(*) FROM users WHERE banned = 1').fetchone()[0],
            }
