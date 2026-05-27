"""NMEA0183 sentence injection helpers - UDP broadcast or TCP server.

Used by unattended tests to feed synthetic boat data into the device's
nmea_wifi source path. Sentences are built with valid checksums so the
device's parser accepts them.
"""
from __future__ import annotations

import socket
import threading
import time
from typing import Iterable


def checksum(payload: str) -> str:
    h = 0
    for c in payload.encode():
        h ^= c
    return f"{h:02X}"


def sentence(body: str) -> str:
    """Wrap a body (everything between $ and *) in $...*HH<CR><LF>."""
    return f"${body}*{checksum(body)}\r\n"


def rmc(sog_kn: float, cog_deg: float, lat: float = 48.1173,
        lon: float = 11.5167) -> str:
    # Convert decimal degrees back to ddmm.mmmm
    def conv(v, ns: bool) -> tuple[str, str]:
        hemi = ("N" if v >= 0 else "S") if ns else ("E" if v >= 0 else "W")
        v = abs(v)
        d = int(v)
        m = (v - d) * 60
        return f"{d:02d}{m:07.4f}", hemi
    lat_s, ns = conv(lat, True)
    lon_s, ew = conv(lon, False)
    body = (f"GPRMC,123519,A,{lat_s},{ns},{lon_s},{ew},"
            f"{sog_kn:.1f},{cog_deg:.1f},230394,003.1,W")
    return sentence(body)


def vhw(heading_deg: float, stw_kn: float) -> str:
    return sentence(f"IIVHW,{heading_deg:.1f},T,{heading_deg:.1f},M,{stw_kn:.1f},N,,K")


def mwv_apparent(awa_deg: float, aws_kn: float) -> str:
    # MWV wants 0..360
    a = awa_deg % 360
    return sentence(f"IIMWV,{a:.1f},R,{aws_kn:.1f},N,A")


def dpt(depth_m: float) -> str:
    return sentence(f"SDDPT,{depth_m:.1f},0.0,")


def mtw(temp_c: float) -> str:
    return sentence(f"IIMTW,{temp_c:.1f},C")


def xte_left(nm: float) -> str:
    return sentence(f"GPXTE,A,A,{nm:.2f},L,N")


# --- UDP pump ---------------------------------------------------------

def send_udp(target_host: str, port: int, sentences: Iterable[str]) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # macOS + Linux require SO_BROADCAST for 255.255.255.255 / subnet
    # broadcast addresses. Harmless for unicast targets.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        for sent in sentences:
            s.sendto(sent.encode(), (target_host, port))
    finally:
        s.close()


# --- TCP server -------------------------------------------------------

class TcpFeed:
    """Bind a port, accept one client, push sentences from a producer.

    The device's nmea_wifi TCP mode connects to (host, port), so this
    server has to bind a routable address. Use the host's LAN IP and
    point the device at it with `nmea-wifi tcp <ip> <port>`.
    """
    def __init__(self, port: int, sentences: list[str], hold_open_s: float = 5.0):
        self.port = port
        self.sentences = sentences
        self.hold_open_s = hold_open_s
        self._stop = threading.Event()
        self._t: threading.Thread | None = None

    def start(self):
        self._t = threading.Thread(target=self._serve, daemon=True)
        self._t.start()

    def stop(self):
        self._stop.set()
        if self._t:
            self._t.join(timeout=2)

    def _serve(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", self.port))
        s.listen(1)
        s.settimeout(0.5)
        try:
            while not self._stop.is_set():
                try:
                    conn, _ = s.accept()
                except socket.timeout:
                    continue
                conn.settimeout(0.5)
                start = time.time()
                with conn:
                    for sent in self.sentences:
                        if self._stop.is_set():
                            break
                        try:
                            conn.sendall(sent.encode())
                        except OSError:
                            break
                        time.sleep(0.1)
                    # Hold the connection open briefly so the device has
                    # time to parse before EOF.
                    while (not self._stop.is_set()
                           and (time.time() - start) < self.hold_open_s):
                        time.sleep(0.1)
        finally:
            s.close()
