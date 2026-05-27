"""Shared pytest fixtures for esp32-boat-mfd system tests.

The whole suite runs against a real device on the same network. We
deliberately avoid mocking the firmware - these tests exist to catch
regressions in the device-side behavior, not the client-side parsing.
"""
from __future__ import annotations

import os
import socket
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from queue import Queue, Empty

import pytest
import requests

ROOT = Path(__file__).resolve().parent
ARTIFACTS = Path(os.environ.get("ARTIFACTS_DIR", ROOT / "artifacts"))
ARTIFACTS.mkdir(parents=True, exist_ok=True)


def _host() -> str:
    h = os.environ.get("ESPDISP_HOST")
    if not h:
        pytest.skip("ESPDISP_HOST not set; skipping system test")
    return h


@dataclass
class Device:
    host: str
    base: str = field(init=False)

    def __post_init__(self):
        self.base = f"http://{self.host}"

    # ---- HTTP helpers ----
    def get(self, path: str, **kw):
        r = requests.get(f"{self.base}{path}", timeout=10, **kw)
        r.raise_for_status()
        return r

    def post_cmd(self, line: str) -> None:
        """Queue a console command. Returns when accepted (202)."""
        r = requests.post(f"{self.base}/api/cmd",
                          data=line, timeout=10,
                          headers={"Content-Type": "text/plain"})
        assert r.status_code == 202, f"cmd {line!r} -> {r.status_code} {r.text}"

    def state(self) -> dict:
        return self.get("/api/state").json()

    def boat(self) -> dict:
        return self.get("/api/boat").json()

    def sk(self) -> dict:
        return self.get("/api/sk").json()

    def screens(self) -> list[dict]:
        return self.get("/api/screens").json()

    def show_screen(self, screen_id: str) -> None:
        # /api/screen/<id> POST switches; queued, eventual consistency.
        r = requests.post(f"{self.base}/api/screen/{screen_id}", timeout=10)
        assert r.status_code in (200, 202), f"show_screen {screen_id}: {r.status_code}"

    # ---- Screenshot ----
    def screenshot(self, label: str) -> Path:
        r = requests.get(f"{self.base}/api/screenshot.bmp", timeout=10)
        r.raise_for_status()
        path = ARTIFACTS / f"{label}.bmp"
        path.write_bytes(r.content)
        return path

    # ---- Polling helpers ----
    def wait_for_field(self, name: str, expected_source: str,
                       timeout_s: float = 8.0,
                       interval_s: float = 0.4) -> dict:
        """Poll /api/boat until a named field is owned by the given source."""
        deadline = time.time() + timeout_s
        last = None
        while time.time() < deadline:
            b = self.boat()
            f = b["fields"].get(name, {})
            last = f
            if f.get("source") == expected_source and f.get("fresh"):
                return f
            time.sleep(interval_s)
        pytest.fail(f"{name} never reached source={expected_source}; last={last}")


@pytest.fixture(scope="session")
def device() -> Device:
    h = _host()
    d = Device(h)
    # Sanity probe.
    try:
        d.state()
    except Exception as e:
        pytest.skip(f"device {h} unreachable: {e}")
    return d


@pytest.fixture(scope="session")
def artifacts() -> Path:
    return ARTIFACTS


# --- UDP log capture --------------------------------------------------
# Device mirrors all net::logf output to UDP broadcast :9999. We bind a
# socket and stream lines into a thread-safe queue per test session.

class UdpLogTap:
    def __init__(self, port: int = 9999):
        self.port = port
        self.q: Queue[str] = Queue()
        self.stop_flag = threading.Event()
        self.sock: socket.socket | None = None
        self.thread: threading.Thread | None = None

    def start(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind(("", self.port))
        s.settimeout(0.5)
        self.sock = s
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        assert self.sock
        while not self.stop_flag.is_set():
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            for line in data.decode(errors="replace").splitlines():
                self.q.put(line)

    def stop(self):
        self.stop_flag.set()
        if self.thread:
            self.thread.join(timeout=2)
        if self.sock:
            self.sock.close()

    def drain(self) -> list[str]:
        out = []
        while True:
            try:
                out.append(self.q.get_nowait())
            except Empty:
                break
        return out

    def wait_for(self, needle: str, timeout_s: float = 5.0) -> str:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=0.3)
                if needle in line:
                    return line
            except Empty:
                pass
        raise AssertionError(f"never saw {needle!r} in UDP log")


@pytest.fixture(scope="session")
def udp_logs():
    tap = UdpLogTap()
    tap.start()
    yield tap
    tap.stop()
