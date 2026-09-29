import asyncio
import socket
import unittest

from server import MasterService


class MasterServiceTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.service = MasterService("127.0.0.1", 0, 30.0, 16)
        self.port = await self.service.start()

    async def asyncTearDown(self) -> None:
        await self.service.stop()

    async def request(self, line: str) -> str:
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        writer.write(line.encode("ascii"))
        await writer.drain()
        result = await reader.read()
        writer.close()
        await writer.wait_closed()
        return result.decode("ascii")

    async def udp_request(self, line: str) -> str:
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)
        try:
            await loop.sock_sendto(sock, line.encode("ascii"), ("127.0.0.1", self.port))
            data = await asyncio.wait_for(loop.sock_recv(sock, 64), timeout=1.0)
            return data.decode("ascii")
        finally:
            sock.close()

    async def test_ping(self) -> None:
        self.assertEqual(await self.request("PING\n"), "PONG\n.\n")

    async def test_register_list_and_unregister(self) -> None:
        self.assertEqual(await self.udp_request("REGISTER stalkersc 5445\n"), "OK\n")
        listing = await self.request("LIST stalkersc\n")
        self.assertEqual(listing, "XRAYMASTER 1\n127.0.0.1 5445\n.\n")
        self.assertEqual(await self.udp_request("UNREGISTER stalkersc 5445\n"), "OK\n")
        self.assertEqual(await self.request("LIST stalkersc\n"), "XRAYMASTER 1\n.\n")

    async def test_rejects_other_games_and_invalid_ports(self) -> None:
        self.assertEqual(await self.udp_request("REGISTER stalkercoppc 5445\n"), "ERR\n")
        self.assertEqual(await self.udp_request("REGISTER stalkersc 70000\n"), "ERR\n")
        self.assertEqual(await self.request("LIST stalkercoppc\n"), "ERR\n")


if __name__ == "__main__":
    unittest.main()
