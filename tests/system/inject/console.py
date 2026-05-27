"""Console transport for injection commands - BLE NUS or USB serial.

Touch/gesture injection (`tap`, `swipe`, `gesture`, `touch`) lives on
the BLE Nordic UART service and on the USB serial console. It is
DELIBERATELY NOT reachable over HTTP - the device's /api/cmd handler
refuses these words. Tests therefore drive injection through one of
the two transports here.

Selection (in order):
    ESPDISP_SERIAL_PORT=/dev/cu.usbserial-XXXX  -> USB serial (fast, CI-friendly)
    ESPDISP_BLE_NAME=espdisp                    -> BLE NUS (no cables)

If neither is set, tests requiring console injection skip with a clear
message.
"""
from __future__ import annotations

import asyncio
import os
import queue
import threading
import time
from typing import Optional

NUS_SERVICE = "6e400001-b5a3-f393-e0a3-9f4dd9e3a05a"
NUS_RX      = "6e400002-b5a3-f393-e0a3-9f4dd9e3a05a"
NUS_TX      = "6e400003-b5a3-f393-e0a3-9f4dd9e3a05a"


class Console:
    """Sync interface over a console transport. Sends a line, optionally
    waits for a log substring to appear in returned notifications.
    """

    def send(self, line: str) -> None: raise NotImplementedError
    def wait_for(self, needle: str, timeout_s: float = 5.0) -> str:
        raise NotImplementedError
    def close(self) -> None: raise NotImplementedError


# --- BLE -------------------------------------------------------------

class BleConsole(Console):
    """Persistent BLE NUS connection driven by a background asyncio
    loop so the pytest (sync) thread can call send / wait_for naturally.
    """

    def __init__(self, name: str):
        self._name = name
        self._loop = asyncio.new_event_loop()
        self._t = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._t.start()
        self._client = None
        self._rx: "queue.Queue[str]" = queue.Queue()
        self._call(self._connect())

    def _call(self, coro):
        fut = asyncio.run_coroutine_threadsafe(coro, self._loop)
        return fut.result()

    async def _connect(self):
        from bleak import BleakClient, BleakScanner  # lazy import
        dev = await BleakScanner.find_device_by_name(self._name, timeout=10)
        if not dev:
            raise RuntimeError(f"BLE device '{self._name}' not found")
        self._client = BleakClient(dev)
        await self._client.connect()
        await self._client.start_notify(NUS_TX, self._on_notify)

    def _on_notify(self, _, data: bytearray):
        try:
            text = data.decode("utf-8", "replace")
        except Exception:
            return
        for line in text.splitlines():
            self._rx.put(line)

    def send(self, line: str) -> None:
        self._call(self._client.write_gatt_char(NUS_RX, line.encode(),
                                                response=False))

    def wait_for(self, needle: str, timeout_s: float = 5.0) -> str:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                line = self._rx.get(timeout=0.3)
                if needle in line:
                    return line
            except queue.Empty:
                pass
        raise AssertionError(f"BLE: never saw {needle!r}")

    def close(self) -> None:
        try:
            if self._client:
                self._call(self._client.disconnect())
        finally:
            self._loop.call_soon_threadsafe(self._loop.stop)


# --- Serial ----------------------------------------------------------

class SerialConsole(Console):
    """USB serial transport using pyserial. A background thread reads
    incoming lines into a queue."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # lazy import
        self._ser = serial.Serial(port, baud, timeout=0.2)
        self._rx: "queue.Queue[str]" = queue.Queue()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._reader, daemon=True)
        self._t.start()

    def _reader(self):
        buf = b""
        while not self._stop.is_set():
            chunk = self._ser.read(256)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._rx.put(line.decode("utf-8", "replace").rstrip())

    def send(self, line: str) -> None:
        self._ser.write(line.encode() + b"\n")

    def wait_for(self, needle: str, timeout_s: float = 5.0) -> str:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                line = self._rx.get(timeout=0.3)
                if needle in line:
                    return line
            except queue.Empty:
                pass
        raise AssertionError(f"serial: never saw {needle!r}")

    def close(self) -> None:
        self._stop.set()
        self._ser.close()


# --- Factory ---------------------------------------------------------

def make_console() -> Optional[Console]:
    """Return whichever transport is configured, or None if neither.
    Tests should pytest.skip when this returns None.
    """
    port = os.environ.get("ESPDISP_SERIAL_PORT")
    if port:
        return SerialConsole(port)
    name = os.environ.get("ESPDISP_BLE_NAME")
    if name:
        return BleConsole(name)
    return None
