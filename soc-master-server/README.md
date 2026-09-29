# OpenXRay SoC master server

A small, dependency-free master list for the Shadow of Chernobyl network mode.
It is intentionally separate from the Android launcher/engine source so this
directory can be published as its own repository.

The service listens on one configurable port:

- TCP: health checks and server-list requests.
- UDP: game-server registration heartbeats.

It does not relay game traffic. A hosted game still needs its game and QR2 UDP
ports reachable from the Internet. Registrations expire after 90 seconds by
default, so stopped or disconnected servers disappear automatically.

## Run

Python 3.10 or newer is sufficient; there are no third-party dependencies.

```sh
python3 server.py --host 0.0.0.0 --port 28910
```

Open TCP and UDP for the selected port in the host firewall. In the Android
launcher, enter the server's IP/domain and the same port in the profile.

## Install with systemd

```sh
sudo install -d -m 0755 /opt/openxray-master
sudo install -m 0755 server.py /opt/openxray-master/server.py
sudo install -m 0644 openxray-master.service /etc/systemd/system/openxray-master.service
sudo systemctl daemon-reload
sudo systemctl enable --now openxray-master.service
```

Check it with:

```sh
systemctl status openxray-master.service
printf 'PING\n' | nc SERVER 28910
```

## Protocol

The deliberately small text protocol is versioned by the list header.

- TCP `PING\n` returns `PONG\n.\n`.
- TCP `LIST stalkersc\n` returns `XRAYMASTER 1\n`, zero or more
  `IPv4 query-port\n` rows, and `.\n`.
- UDP `REGISTER stalkersc query-port\n` refreshes an entry using the datagram's
  source IP.
- UDP `UNREGISTER stalkersc query-port\n` removes it.

Run the test suite with `python3 -m unittest -v`.
