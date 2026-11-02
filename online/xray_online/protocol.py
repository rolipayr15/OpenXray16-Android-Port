"""Wire-format helpers shared by the services.

The GameSpy Presence (GP) functions mirror the client SDK in
Externals/GameSpy/src/GameSpy/GP (gpiConnect.c, gpiUtility.c) and
common/gsPlatformUtil.c, so that the stock engine client talks to this server
unchanged.
"""
import base64
import hashlib
import random
import secrets
import string

# GP error codes (GP/gp.h, GPErrorCode).
GP_DATABASE = 0x0004
GP_LOGIN_BAD_NICK = 0x0102
GP_LOGIN_BAD_EMAIL = 0x0103
GP_LOGIN_BAD_PASSWORD = 0x0104
GP_LOGIN_BAD_PROFILE = 0x0105
GP_LOGIN_PROFILE_DELETED = 0x0106
GP_LOGIN_BAD_UNIQUENICK = 0x0109
GP_NEWUSER_BAD_NICK = 0x0201
GP_NEWUSER_BAD_PASSWORD = 0x0202
GP_NEWUSER_UNIQUENICK_INVALID = 0x0203
GP_NEWUSER_UNIQUENICK_INUSE = 0x0204
GP_DELPROFILE_LAST_PROFILE = 0x0C01
GP_CHECK_BAD_EMAIL = 0x0E01
GP_CHECK_BAD_NICK = 0x0E02
GP_CHECK_BAD_PASSWORD = 0x0E03
GP_REGISTERUNIQUENICK_TAKEN = 0x1001

GP_NICK_LEN = 31
GP_UNIQUENICK_LEN = 21
GP_LOGIN_TICKET_LEN = 25
_GP_XOR_SEED = 0x79707367  # "gspy"
_LONGRAND_MAX = 2147483647

GAMES = {
    'stalkersc': 'Shadow of Chernobyl',
    'stalkercs': 'Clear Sky',
    'stalkercoppc': 'Call of Pripyat',
}


def parse_kv(message):
    """Parses '\\key\\value\\key\\value...\\final\\' into an ordered list of pairs.

    The first pair's key names the command; its value is often empty.
    """
    message = message.split('\\final\\', 1)[0]
    if message.startswith('\\'):
        message = message[1:]
    parts = message.split('\\')
    if len(parts) % 2:
        parts.append('')
    return [(parts[i], parts[i + 1]) for i in range(0, len(parts), 2)]


def build_kv(*pairs):
    """Builds '\\k\\v...\\final\\'. Values must not contain backslashes."""
    out = []
    for key, value in pairs:
        out.append('\\' + key + '\\' + str(value).replace('\\', ''))
    return ''.join(out) + '\\final\\'


def md5_hex(text):
    return hashlib.md5(text.encode('latin-1')).hexdigest()


def _gs_keystream(length):
    """Util_RandSeed(GP_XOR_SEED) / Util_RandInt(0, 0xFF): Park-Miller."""
    seed = _GP_XOR_SEED & _LONGRAND_MAX or 1
    out = bytearray()
    for _ in range(length):
        lo = 16807 * (seed & 0xFFFF)
        hi = 16807 * (seed >> 16)
        lo += (hi & 0x7FFF) << 16
        if lo > _LONGRAND_MAX:
            lo = (lo & _LONGRAND_MAX) + 1
        lo += hi >> 15
        if lo > _LONGRAND_MAX:
            lo = (lo & _LONGRAND_MAX) + 1
        seed = lo
        out.append((seed % 255) & 0xFF)
    return bytes(out)


def gs_password_decode(encoded):
    """Inverse of gpiEncodeString: GameSpy base64 ([]_) of the XOR stream."""
    data = encoded.replace('[', '+').replace(']', '/').replace('_', '=')
    data += '=' * (-len(data) % 4)
    raw = base64.b64decode(data)
    return bytes(a ^ b for a, b in zip(raw, _gs_keystream(len(raw)))).decode('latin-1')


def gs_password_encode(password):
    """gpiEncodeString, used by tests to act as the client."""
    raw = password.encode('latin-1')
    xored = bytes(a ^ b for a, b in zip(raw, _gs_keystream(len(raw))))
    return base64.b64encode(xored).decode().replace('+', '[').replace('/', ']').replace('=', '_')


def gp_login_user(nick, email, uniquenick, partner_id=0):
    """The 'user' string both sides hash (gpiSendLogin)."""
    prefix = f'{partner_id}@' if partner_id else ''
    if uniquenick:
        return prefix + uniquenick
    return f'{prefix}{nick}@{email}'


def gp_login_response(password_md5, user, user_challenge, server_challenge):
    """Client proof of the password (gpiSendLogin)."""
    return md5_hex(password_md5 + ' ' * 48 + user + user_challenge + server_challenge + password_md5)


def gp_login_proof(password_md5, user, user_challenge, server_challenge):
    """Server proof the client checks (gpiProcessConnect, GPI_LOGIN)."""
    return md5_hex(password_md5 + ' ' * 48 + user + server_challenge + user_challenge + password_md5)


def random_challenge(length=10):
    return ''.join(secrets.choice(string.ascii_uppercase) for _ in range(length))


def login_ticket():
    alphabet = string.ascii_letters + string.digits
    return ''.join(secrets.choice(alphabet) for _ in range(GP_LOGIN_TICKET_LEN - 1))


def session_key():
    return random.SystemRandom().randint(10_000_000, 99_999_999)


def valid_uniquenick(nick):
    """Printable, no separators the protocol or the UI could choke on."""
    if not nick or len(nick) >= GP_UNIQUENICK_LEN or nick[0].isdigit():
        return False
    allowed = set(string.ascii_letters + string.digits + '_-[]{}.')
    return all(ch in allowed for ch in nick)


def valid_email(email):
    return 3 <= len(email) <= 50 and email.count('@') == 1 and '\\' not in email and ' ' not in email
