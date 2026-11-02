"""OpenXRay master server: the server list for all three games.

Protocol (src/xrGameSpy/GameSpy_MasterServer.cpp):
  TCP  "PING\\n"         -> "PONG\\n"
  TCP  "LIST <game>\\n"  -> "XRAYMASTER 1\\n" + "<ipv4> <queryport>\\n"... + ".\\n"
  UDP  "REGISTER <game> <queryport>\\n"   heartbeat every 30 s from a game server
  UDP  "UNREGISTER <game> <queryport>\\n" on shutdown

The public address is the UDP sender address (what NAT maps the server to).
Clients query each server's details directly over GameSpy QR2; the master
does the same in the background so the web interface can show names, maps
and player counts.
"""
import asyncio
import logging
import os
import socket
import threading
import time
from dataclasses import dataclass, field

from .protocol import GAMES

log = logging.getLogger('master')


@dataclass
class GameServer:
    game: str
    ip: str
    port: int
    first_seen: float
    last_seen: float
    info: dict = field(default_factory=dict)   # QR2 server keys
    info_time: float = 0.0
    ping_ms: int = -1

    @property
    def key(self):
        return (self.game, self.ip, self.port)


class Registry:
    def __init__(self, store):
        self.store = store
        self._servers = {}
        self._lock = threading.Lock()

    def heartbeat(self, game, ip, port):
        now = time.time()
        with self._lock:
            server = self._servers.get((game, ip, port))
            if server:
                server.last_seen = now
                return True
            limit = int(self.store.setting('max_servers_per_ip') or 0)
            if limit and sum(1 for s in self._servers.values() if s.ip == ip) >= limit:
                return False
            self._servers[(game, ip, port)] = GameServer(game, ip, port, now, now)
        self.store.log('server_up', game, f'{ip}:{port}')
        return True

    def remove(self, game, ip, port):
        with self._lock:
            removed = self._servers.pop((game, ip, port), None)
        if removed:
            self.store.log('server_down', game, f'{ip}:{port}')

    def expire(self):
        timeout = int(self.store.setting('server_timeout') or 90)
        cutoff = time.time() - timeout
        with self._lock:
            stale = [key for key, server in self._servers.items() if server.last_seen < cutoff]
            for key in stale:
                del self._servers[key]
        for game, ip, port in stale:
            self.store.log('server_down', game, f'{ip}:{port} (timeout)')

    def servers(self, game=None):
        with self._lock:
            return [s for s in self._servers.values() if game is None or s.game == game]

    def update_info(self, key, info, ping_ms):
        with self._lock:
            server = self._servers.get(key)
            if server:
                server.info = info
                server.info_time = time.time()
                server.ping_ms = ping_ms


class _UdpProtocol(asyncio.DatagramProtocol):
    def __init__(self, registry, store):
        self.registry = registry
        self.store = store

    def datagram_received(self, data, addr):
        ip = addr[0]
        if self.store.ip_banned(ip):
            return
        try:
            command, game, port = data.decode('ascii').split()
            port = int(port)
        except (UnicodeDecodeError, ValueError):
            return
        if game not in GAMES or not 1 <= port <= 65535:
            return
        if command == 'REGISTER':
            if not self.registry.heartbeat(game, ip, port):
                log.warning('refused %s %s:%d (too many servers from this address)', game, ip, port)
        elif command == 'UNREGISTER':
            self.registry.remove(game, ip, port)


async def _handle_tcp(reader, writer, registry, store):
    peer = writer.get_extra_info('peername')
    try:
        if peer and store.ip_banned(peer[0]):
            return
        line = (await asyncio.wait_for(reader.readline(), timeout=5)).decode('ascii', 'replace').strip()
        if line == 'PING':
            writer.write(b'PONG\n')
        elif line.startswith('LIST '):
            game = line[5:].strip()
            lines = ['XRAYMASTER 1']
            if game in GAMES:
                lines += [f'{s.ip} {s.port}' for s in registry.servers(game)]
            lines.append('.')
            writer.write(('\n'.join(lines) + '\n').encode('ascii'))
        await writer.drain()
    except (asyncio.TimeoutError, ConnectionError):
        pass
    finally:
        writer.close()


def query_server(ip, port, timeout=2.0):
    """QR2 query for all server keys (qr2.c, PACKET_QUERY). Returns (info, ping_ms)."""
    request_key = os.urandom(4)
    packet = b'\xfe\xfd\x00' + request_key + b'\xff\x00\x00'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        started = time.monotonic()
        sock.sendto(packet, (ip, port))
        data, _ = sock.recvfrom(4096)
        ping_ms = int((time.monotonic() - started) * 1000)
    if len(data) < 5 or data[0] != 0x00 or data[1:5] != request_key:
        raise ValueError('not a QR2 query reply')
    fields = data[5:].split(b'\x00')
    info = {}
    for index in range(0, len(fields) - 1, 2):
        key = fields[index].decode('latin-1')
        if not key:
            break
        info[key] = fields[index + 1].decode('latin-1', 'replace')
    return info, ping_ms


async def _maintenance(registry, interval=15):
    loop = asyncio.get_running_loop()
    while True:
        registry.expire()
        for server in registry.servers():
            try:
                info, ping_ms = await loop.run_in_executor(None, query_server, server.ip, server.port)
                registry.update_info(server.key, info, ping_ms)
            except (OSError, ValueError):
                registry.update_info(server.key, server.info, -1)
        await asyncio.sleep(interval)


async def start(registry, store, host, port):
    loop = asyncio.get_running_loop()
    tcp = await asyncio.start_server(lambda r, w: _handle_tcp(r, w, registry, store), host, port)
    udp, _ = await loop.create_datagram_endpoint(lambda: _UdpProtocol(registry, store), local_addr=(host, port))
    maintenance = asyncio.create_task(_maintenance(registry))
    log.info('master listening on %s:%d (tcp+udp)', host, port)
    return tcp, udp, maintenance
