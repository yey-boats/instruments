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

from tests.system.discovery import (
    DiscoveredDevice,
    discover_devices,
    split_device_specs,
)

ROOT = Path(__file__).resolve().parent
ARTIFACTS = Path(os.environ.get("ARTIFACTS_DIR", ROOT / "artifacts"))
ARTIFACTS.mkdir(parents=True, exist_ok=True)


def _web_auth() -> tuple[str, str] | None:
    username = os.environ.get("ESPDISP_WEB_USERNAME")
    password = os.environ.get("ESPDISP_WEB_PASSWORD")
    if username and password:
        return username, password
    return None


@dataclass
class Device:
    host: str
    port: int = 80
    discovered: DiscoveredDevice | None = None
    base: str = field(init=False)
    auth: tuple[str, str] | None = field(default_factory=_web_auth)

    def __post_init__(self):
        suffix = "" if self.port == 80 else f":{self.port}"
        self.base = f"http://{self.host}{suffix}"

    # ---- HTTP helpers ----
    def get(self, path: str, **kw):
        kw.setdefault("auth", self.auth)
        r = requests.get(f"{self.base}{path}", timeout=10, **kw)
        r.raise_for_status()
        return r

    def post_cmd(self, line: str) -> None:
        """Queue a console command. Returns when accepted (202)."""
        auth_kw = {"auth": self.auth} if self.auth else {}
        r = requests.post(f"{self.base}/api/cmd",
                          data=line, timeout=10,
                          headers={"Content-Type": "text/plain"},
                          **auth_kw)
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
        auth_kw = {"auth": self.auth} if self.auth else {}
        r = requests.post(f"{self.base}/api/screen/{screen_id}",
                          timeout=10, **auth_kw)
        assert r.status_code in (200, 202), f"show_screen {screen_id}: {r.status_code}"

    # ---- Screenshot ----
    def screenshot(self, label: str) -> Path:
        auth_kw = {"auth": self.auth} if self.auth else {}
        r = requests.get(f"{self.base}/api/screenshot.bmp",
                         timeout=10, **auth_kw)
        r.raise_for_status()
        path = ARTIFACTS / f"{label}.bmp"
        path.write_bytes(r.content)
        return path

    # ---- Touch / gesture injection (BLE / USB serial only) ------------
    # The HTTP path deliberately does NOT support injection - /api/cmd
    # 403s these words. Tests need ESPDISP_BLE_NAME or
    # ESPDISP_SERIAL_PORT set; the `console` fixture wraps the
    # appropriate transport.

    def touch(self, console, x: int, y: int, pressed: bool) -> None:
        """Raw touchscreen-manager-level injection."""
        console.send(f"touch {x} {y} {1 if pressed else 0}")
        console.wait_for("[test] touch ok=1", timeout_s=3)

    def tap(self, console, x: int, y: int, hold_ms: int = 50) -> None:
        console.send(f"tap {x} {y} {hold_ms}")
        console.wait_for("[test] tap ok=", timeout_s=3 + hold_ms / 1000)

    def swipe(self, console, x0: int, y0: int, x1: int, y1: int,
              dur_ms: int = 300, steps: int = 8) -> None:
        console.send(f"swipe {x0} {y0} {x1} {y1} {dur_ms} {steps}")
        console.wait_for("[test] swipe ok=", timeout_s=3 + dur_ms / 1000)

    def gesture(self, console, direction: str) -> None:
        console.send(f"gesture {direction}")
        console.wait_for("[test] gesture ok=", timeout_s=3)

    def cmd_via_console(self, console, line: str,
                        wait_for: str | None = None,
                        timeout_s: float = 3.0) -> str | None:
        """Send any console command through the chosen transport (i.e.
        not through /api/cmd). Used by tests asserting BLE/serial paths."""
        console.send(line)
        if wait_for:
            return console.wait_for(wait_for, timeout_s)
        return None

    # ---- Polling helpers ----
    def wait_for_screen(self, screen_id: str,
                        timeout_s: float = 5.0,
                        interval_s: float = 0.25) -> str:
        """Poll /api/state until screen.id == screen_id."""
        deadline = time.time() + timeout_s
        last = None
        while time.time() < deadline:
            last = self.state()["screen"]["id"]
            if last == screen_id:
                return last
            time.sleep(interval_s)
        pytest.fail(f"screen never became {screen_id!r}; last={last!r}")

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


def pytest_addoption(parser):
    group = parser.getgroup("espdisp")
    group.addoption("--espdisp-device", action="append", default=[],
                    help="Explicit espdisp host, host:port, or URL. "
                         "Can be repeated.")
    group.addoption("--espdisp-devices", default=None,
                    help="Comma/space-separated espdisp host list.")
    group.addoption("--espdisp-no-discovery", action="store_true",
                    help="Disable mDNS discovery and use explicit devices only.")
    group.addoption("--espdisp-no-udp-discovery", action="store_true",
                    help="Disable UDP device announcement discovery.")
    group.addoption("--espdisp-scan-cidr", action="append", default=[],
                    help="Actively probe a CIDR for devices.")
    group.addoption("--espdisp-discovery-timeout", type=float, default=2.5,
                    help="mDNS discovery timeout in seconds.")
    group.addoption("--espdisp-udp-timeout", type=float, default=5.5,
                    help="UDP announcement listen timeout in seconds.")


def _configured_device_specs(config) -> list[str]:
    specs: list[str] = []
    specs.extend(config.getoption("--espdisp-device") or [])
    specs.extend(split_device_specs(config.getoption("--espdisp-devices")))
    specs.extend(split_device_specs(os.environ.get("ESPDISP_DEVICES")))
    specs.extend(split_device_specs(os.environ.get("ESPDISP_HOST")))
    return specs


def _configured_scan_cidrs(config) -> list[str]:
    cidrs: list[str] = []
    cidrs.extend(config.getoption("--espdisp-scan-cidr") or [])
    cidrs.extend(split_device_specs(os.environ.get("ESPDISP_DISCOVERY_CIDRS")))
    return cidrs


def _collect_test_devices(config) -> list[DiscoveredDevice]:
    cached = getattr(config, "_espdisp_test_devices", None)
    if cached is not None:
        return cached
    explicit = _configured_device_specs(config)
    no_discovery = bool(config.getoption("--espdisp-no-discovery"))
    udp_listen = not no_discovery and not bool(
        config.getoption("--espdisp-no-udp-discovery"))
    devices = discover_devices(
        explicit=explicit,
        mdns=not no_discovery,
        cidrs=_configured_scan_cidrs(config),
        udp_listen=udp_listen,
        udp_timeout=config.getoption("--espdisp-udp-timeout"),
        auth=_web_auth(),
        mdns_timeout=config.getoption("--espdisp-discovery-timeout"),
    )
    config._espdisp_test_devices = devices
    return devices


def pytest_generate_tests(metafunc):
    if "device" not in metafunc.fixturenames:
        return
    devices = _collect_test_devices(metafunc.config)
    if not devices:
        metafunc.parametrize("device", [None], ids=["no-espdisp-device"],
                             indirect=True)
        return
    metafunc.parametrize("device", devices,
                         ids=[device.pytest_id for device in devices],
                         indirect=True)


@pytest.fixture(scope="session")
def device(request) -> Device:
    target = getattr(request, "param", None)
    if target is None:
        pytest.skip("no espdisp devices discovered; set ESPDISP_HOST, "
                    "ESPDISP_DEVICES, or --espdisp-device")
    d = Device(target.host, port=target.port, discovered=target)
    # Sanity probe.
    try:
        d.state()
    except Exception as e:
        pytest.skip(f"device {target.host} unreachable: {e}")
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


# --- Console transport for BLE/serial injection ----------------------

@pytest.fixture(scope="session", autouse=True)
def _sk_pump_teardown():
    """Close the cached SK injector at end-of-session to avoid leaking
    a WS that the SK server would later count against the IP limiter."""
    yield
    try:
        from tests.system.inject import sk_pump
        sk_pump.close()
    except Exception:
        pass


@pytest.fixture(scope="session")
def manager():
    """Start the spec-18 plugin mock for Lane B tests. Returns the
    ManagerMock instance with .queue_command(), .set_config(),
    .devices etc. Skipped automatically if aiohttp isn't installed."""
    try:
        from tests.system.inject.manager_mock import ManagerMock
    except ImportError as e:
        pytest.skip(f"manager mock unavailable: {e}")
    m = ManagerMock(port=4738)
    m.start()
    yield m
    m.stop()


@pytest.fixture(scope="session")
def console():
    """Open a persistent BLE or USB-serial console for injection tests.

    Skips the test if neither ESPDISP_SERIAL_PORT nor ESPDISP_BLE_NAME
    is set. The HTTP path does NOT carry injection - this fixture is
    the only way to drive tap/swipe/gesture/touch from tests.
    """
    from tests.system.inject.console import make_console
    c = make_console()
    if c is None:
        pytest.skip("set ESPDISP_SERIAL_PORT or ESPDISP_BLE_NAME "
                    "for injection tests")
    yield c
    try:
        c.close()
    except Exception:
        pass


@dataclass
class RealManager:
    """Thin client for the real signalk-espdisp-manager plugin.

    Skipped when ESPDISP_MGR_URL / ESPDISP_MGR_SK_TOKEN aren't set, so
    tests that talk to the real plugin remain opt-in. The device must
    already be provisioned (manager-register / manager-sk-token /
    manager-token via BLE or USB serial) - this fixture only drives the
    plugin side.
    """
    base: str
    sk_token: str

    def _hdrs(self) -> dict:
        return {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.sk_token}",
        }

    def devices(self) -> list[dict]:
        r = requests.get(f"{self.base}/devices",
                         headers=self._hdrs(), timeout=5)
        r.raise_for_status()
        return r.json().get("devices", [])

    def device(self, device_id: str) -> dict:
        r = requests.get(f"{self.base}/devices/{device_id}",
                         headers=self._hdrs(), timeout=5)
        r.raise_for_status()
        return r.json()

    def profiles(self) -> list[dict]:
        r = requests.get(f"{self.base}/profiles",
                         headers=self._hdrs(), timeout=5)
        r.raise_for_status()
        return r.json().get("profiles", [])

    def assign_profile(self, device_id: str, profile_id: str) -> dict:
        r = requests.post(f"{self.base}/devices/{device_id}/profile",
                          headers=self._hdrs(),
                          json={"profile": profile_id}, timeout=5)
        r.raise_for_status()
        return r.json()


@pytest.fixture(scope="session")
def real_manager() -> RealManager:
    base = os.environ.get("ESPDISP_MGR_URL")
    token = os.environ.get("ESPDISP_MGR_SK_TOKEN")
    if not base or not token:
        pytest.skip("set ESPDISP_MGR_URL and ESPDISP_MGR_SK_TOKEN to "
                    "run tests against the real plugin")
    return RealManager(base=base.rstrip("/"), sk_token=token)
