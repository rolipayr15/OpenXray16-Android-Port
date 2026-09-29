#!/usr/bin/env python3
"""Small master-list service for OpenXRay Shadow of Chernobyl servers."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import signal
import time
from dataclasses import dataclass


LOG = logging.getLogger("openxray-master")
PROTOCOL_GAME = "stalkersc"
MAX_LINE = 256


@dataclass
class ServerEntry:
    address: str
    query_port: int
    refreshed_at: float


class Registry:
    def __init__(self, ttl_seconds: float = 90.0, max_servers: int = 2048) -> None:
        self.ttl_seconds = ttl_seconds
        self.max_servers = max_servers
        self._servers: dict[tuple[str, int], ServerEntry] = {}

    def _prune(self) -> None:
        oldest_allowed = time.monotonic() - self.ttl_seconds
        expired = [key for key, value in self._servers.items() if value.refreshed_at < oldest_allowed]
        for key in expired:
            del self._servers[key]

    def register(self, address: str, query_port: int) -> bool:
        self._prune()
        key = (address, query_port)
        if key not in self._servers and len(self._servers) >= self.max_servers:
            return False
        is_new = key not in self._servers
        self._servers[key] = ServerEntry(address, query_port, time.monotonic())
        if is_new:
            LOG.info("registered %s:%d", address, query_port)
        return True

    def unregister(self, address: str, query_port: int) -> None:
        if self._servers.pop((address, query_port), None) is not None:
            LOG.info("unregistered %s:%d", address, query_port)

    def list(self) -> list[ServerEntry]:
        self._prune()
        return sorted(self._servers.values(), key=lambda item: (item.address, item.query_port))


class RegistrationProtocol(asyncio.DatagramProtocol):
    def __init__(self, registry: Registry) -> None:
        self.registry = registry
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        response = b"ERR\n"
        try:
            if len(data) > MAX_LINE:
                raise ValueError("packet too long")
            parts = data.decode("ascii", "strict").strip().split()
            if len(parts) != 3 or parts[1] != PROTOCOL_GAME:
                raise ValueError("invalid command")
            query_port = int(parts[2])
            if not 1 <= query_port <= 65535:
                raise ValueError("invalid port")
            if parts[0] == "REGISTER":
                response = b"OK\n" if self.registry.register(addr[0], query_port) else b"FULL\n"
            elif parts[0] == "UNREGISTER":
                self.registry.unregister(addr[0], query_port)
                response = b"OK\n"
        except (UnicodeDecodeError, ValueError):
            pass
        if self.transport is not None:
            self.transport.sendto(response, addr)


class MasterService:
    def __init__(self, host: str, port: int, ttl_seconds: float, max_servers: int) -> None:
        self.host = host
        self.port = port
        self.registry = Registry(ttl_seconds, max_servers)
        self.tcp_server: asyncio.Server | None = None
        self.udp_transport: asyncio.DatagramTransport | None = None

    async def start(self) -> int:
        self.tcp_server = await asyncio.start_server(self.handle_client, self.host, self.port)
        socket = self.tcp_server.sockets[0]
        actual_port = int(socket.getsockname()[1])
        loop = asyncio.get_running_loop()
        transport, _ = await loop.create_datagram_endpoint(
            lambda: RegistrationProtocol(self.registry), local_addr=(self.host, actual_port)
        )
        self.udp_transport = transport  # type: ignore[assignment]
        self.port = actual_port
        LOG.info("listening on %s:%d (TCP list/health, UDP registration)", self.host, self.port)
        return actual_port

    async def stop(self) -> None:
        if self.udp_transport is not None:
            self.udp_transport.close()
            self.udp_transport = None
        if self.tcp_server is not None:
            self.tcp_server.close()
            await self.tcp_server.wait_closed()
            self.tcp_server = None

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        response = b"ERR\n"
        try:
            raw_line = await asyncio.wait_for(reader.readline(), timeout=3.0)
            if not raw_line or len(raw_line) > MAX_LINE:
                raise ValueError("invalid request")
            parts = raw_line.decode("ascii", "strict").strip().split()
            if parts == ["PING"]:
                response = b"PONG\n.\n"
            elif parts == ["LIST", PROTOCOL_GAME]:
                lines = ["XRAYMASTER 1"]
                lines.extend(f"{entry.address} {entry.query_port}" for entry in self.registry.list())
                lines.append(".")
                response = ("\n".join(lines) + "\n").encode("ascii")
        except (asyncio.TimeoutError, UnicodeDecodeError, ValueError):
            pass
        writer.write(response)
        with contextlib.suppress(ConnectionError):
            await writer.drain()
        writer.close()
        with contextlib.suppress(ConnectionError):
            await writer.wait_closed()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=28910, help="shared TCP/UDP port")
    parser.add_argument("--ttl", type=float, default=90.0, help="registration lifetime in seconds")
    parser.add_argument("--max-servers", type=int, default=2048)
    parser.add_argument("--log-level", default="INFO", choices=("DEBUG", "INFO", "WARNING", "ERROR"))
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.ttl <= 0 or args.max_servers <= 0:
        parser.error("--ttl and --max-servers must be positive")
    return args


async def run(args: argparse.Namespace) -> None:
    service = MasterService(args.host, args.port, args.ttl, args.max_servers)
    await service.start()
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for name in ("SIGINT", "SIGTERM"):
        with contextlib.suppress(NotImplementedError):
            loop.add_signal_handler(getattr(signal, name), stop_event.set)
    try:
        await stop_event.wait()
    finally:
        await service.stop()


def main() -> None:
    args = parse_arguments()
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
