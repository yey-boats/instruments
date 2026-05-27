"""SignalK delta injector. Holds ONE authenticated WS connection per
process so the test suite doesn't trip SK 2.27's per-IP concurrent-WS
limiter (HTTP 429) with rapid open/close cycles.

Usage from tests:
    from tests.system.inject import sk_pump
    sk_pump.send(host, port, "navigation.speedOverGround", 3.5)

The first call authenticates and opens the WS. Later calls reuse it.
Each call adds ~5 ms of overhead (one frame send + small sleep).
"""
from __future__ import annotations

import asyncio
import json
import threading
import time
import urllib.request
from typing import Optional


class _Session:
    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.ws = None
        self.host = None
        self.port = None

    def _call(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result()

    async def _open(self, host: str, port: int):
        import websockets
        token = _token(host, port)
        uri = (f"ws://{host}:{port}/signalk/v1/stream"
               f"?subscribe=none&token={token}")
        # SK 2.27 returns HTTP 429 when too many concurrent WS connections
        # come from the same IP. Back off and retry a few times - the
        # limiter window is short and once we hold the connection it
        # stays valid for the rest of the test session.
        last_err = None
        for delay in (0, 2, 5, 10):
            if delay:
                await asyncio.sleep(delay)
            try:
                self.ws = await websockets.connect(uri, open_timeout=8,
                                                   max_size=2 ** 20)
                break
            except Exception as e:
                last_err = e
                if "429" not in str(e):
                    raise
        else:
            raise RuntimeError(f"SK rejected WS after retries: {last_err}")
        # Drain hello so future recv() (if any) doesn't see it
        try:
            await asyncio.wait_for(self.ws.recv(), timeout=1.0)
        except Exception:
            pass

    def ensure(self, host: str, port: int) -> None:
        if self.ws is not None and self.host == host and self.port == port:
            return
        if self.ws is not None:
            try:
                self._call(self.ws.close())
            except Exception:
                pass
            self.ws = None
        self.host, self.port = host, port
        self._call(self._open(host, port))

    async def _send(self, delta: dict) -> None:
        await self.ws.send(json.dumps(delta))

    def send_delta(self, path: str, value) -> None:
        delta = {
            "context": "vessels.self",
            "updates": [{
                "values": [{"path": path, "value": value}],
            }],
        }
        self._call(self._send(delta))
        # Tiny pause so SK can fan out before the next call.
        time.sleep(0.05)

    def close(self):
        if self.ws is not None:
            try:
                self._call(self.ws.close())
            except Exception:
                pass
            self.ws = None
        self.loop.call_soon_threadsafe(self.loop.stop)


def _token(host: str, port: int, user="admin", password="admin") -> str:
    req = urllib.request.Request(
        f"http://{host}:{port}/signalk/v1/auth/login",
        data=json.dumps({"username": user, "password": password}).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())["token"]


_session: Optional[_Session] = None
_lock = threading.Lock()


def _get(host: str, port: int) -> _Session:
    global _session
    with _lock:
        if _session is None:
            _session = _Session()
        _session.ensure(host, port)
        return _session


def send(host: str, port: int, path: str, value) -> None:
    """Send one delta over the cached WS. Backwards-compatible with the
    old single-shot signature."""
    s = _get(host, port)
    s.send_delta(path, value)


def close() -> None:
    """Tear down the session (called by pytest's session-end teardown)."""
    global _session
    with _lock:
        if _session is not None:
            _session.close()
            _session = None
