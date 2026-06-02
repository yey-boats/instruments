#!/usr/bin/env python3
"""Persist espdisp UDP log broadcasts to stdout.

Binds UDP 0.0.0.0:<port> (default 9999) and prints one line per
received datagram in the form:

    YYYY-MM-DDTHH:MM:SS.mmm  [SRC_IP]  <log line>

The firmware emits each logf() line as its own datagram only when
built with -D ESPDISP_DEBUG_UDP_LOG=1 (PlatformIO env
esp32-4848s040-debug). Release builds never broadcast, so a
production device on the same LAN produces no output here.

stdout is the persistence transport: systemd captures it into
/var/log/espdisp/device.log, and logrotate handles weekly rotation
+ compression. Stay stdlib-only so the lab computer doesn't need a
venv.
"""
from __future__ import annotations

import argparse
import datetime
import os
import signal
import socket
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("ESPDISP_LOG_PORT", "9999")),
                    help="UDP port to bind (default: 9999 or $ESPDISP_LOG_PORT).")
    ap.add_argument("--bind", default=os.environ.get("ESPDISP_LOG_BIND", "0.0.0.0"),
                    help="Interface to bind (default: 0.0.0.0).")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Allow receiving broadcast datagrams.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind((args.bind, args.port))

    # SIGTERM = clean shutdown for systemd.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    sys.stdout.write(
        f"# espdisp loglistener listening on {args.bind}:{args.port}\n")
    sys.stdout.flush()

    while True:
        try:
            data, addr = s.recvfrom(4096)
        except OSError as e:
            sys.stderr.write(f"recvfrom: {e}\n")
            sys.stderr.flush()
            continue
        ts = datetime.datetime.now().isoformat(timespec="milliseconds")
        text = data.decode("utf-8", "replace").rstrip("\r\n")
        # The firmware appends a trailing newline already; rstrip handles
        # the (rare) case where it doesn't. Multi-line datagrams stay
        # one-line by leaving embedded newlines as literal '\n' in output.
        sys.stdout.write(f"{ts}  [{addr[0]}]  {text}\n")
        sys.stdout.flush()


if __name__ == "__main__":
    sys.exit(main())
