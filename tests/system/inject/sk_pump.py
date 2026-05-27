"""Minimal SignalK delta injector. Used by tests that need to put a
specific value on a specific path without spinning up fake_boat.py.

Authenticates against the demo server (admin/admin) and sends one
delta. For continuous synthetic data, use tools/fake_boat.py from the
project root.
"""
from __future__ import annotations

import asyncio
import json
import urllib.request


def _token(host: str, port: int, user="admin", password="admin") -> str:
    req = urllib.request.Request(
        f"http://{host}:{port}/signalk/v1/auth/login",
        data=json.dumps({"username": user, "password": password}).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())["token"]


async def _send_one(host: str, port: int, path: str, value) -> None:
    import websockets
    token = _token(host, port)
    uri = (f"ws://{host}:{port}/signalk/v1/stream"
           f"?subscribe=none&token={token}")
    async with websockets.connect(uri) as ws:
        await ws.recv()  # hello
        delta = {
            "context": "vessels.self",
            "updates": [{
                "values": [{"path": path, "value": value}],
            }],
        }
        await ws.send(json.dumps(delta))
        # Give the server time to fan out to the device's subscription
        await asyncio.sleep(0.5)


def send(host: str, port: int, path: str, value) -> None:
    asyncio.run(_send_one(host, port, path, value))
