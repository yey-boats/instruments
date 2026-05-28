"""Mock implementation of the spec-18 SignalK ESP Display Manager plugin.

Provides just enough of the HTTP surface that firmware milestones F1-F6
(spec 17) can be authored against it before the real plugin exists.
Test files in tests/system/unattended/test_manager_*.py target these
endpoints; xfail until the firmware side lands.

Run standalone for manual poking:

    python3 -m tests.system.inject.manager_mock --port 4738

Or as a pytest fixture (default port 4738):

    def test_x(manager):
        manager.queue_command(deviceId, "screen.set", {"id": "wind"})
        ...

Endpoints implemented (subset of spec 18):

    GET  /.well-known/espdisp-management   (discovery)
    POST /devices/register                  (S2 milestone)
    POST /devices/:id/status                (S4 heartbeat)
    GET  /devices/:id/config                (S4 config fetch)
    GET  /devices/:id/commands              (S5 command poll)
    POST /devices/:id/commands/:cid/ack     (S5 ack)
    GET  /devices                           (admin readback)
    GET  /audit                             (audit log readback, S9)

State is in-process and ephemeral. Tests instantiate one per session.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import secrets as pysecrets
import threading
import time
import uuid
from dataclasses import dataclass, field, asdict
from typing import Any, Optional

from aiohttp import web


PROTOCOL_VERSION = "1"


@dataclass
class Command:
    id: str
    type: str          # e.g. "screen.set", "brightness.set"
    payload: dict
    state: str = "pending"   # pending | delivered | acknowledged | failed | expired
    created_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    delivered_ms: Optional[int] = None
    ack_ms: Optional[int] = None
    ack_result: Optional[str] = None


@dataclass
class Device:
    id: str
    token: str
    identity: dict = field(default_factory=dict)
    capabilities: dict = field(default_factory=dict)
    last_status: dict = field(default_factory=dict)
    last_seen_ms: Optional[int] = None
    config_version: str = "v1"
    config_hash: str = ""
    config: dict = field(default_factory=dict)
    commands: list[Command] = field(default_factory=list)


class ManagerMock:
    """In-process server that mimics enough of spec 18 to drive
    firmware tests."""

    def __init__(self, host: str = "0.0.0.0", port: int = 4738):
        self._host = host
        self._port = port
        self._app = web.Application()
        self._app.add_routes([
            web.get("/.well-known/espdisp-management", self._discovery),
            web.post("/devices/register", self._register),
            web.post(r"/devices/{did}/status", self._status),
            web.get(r"/devices/{did}/config", self._config),
            web.get(r"/devices/{did}/commands", self._commands_get),
            web.post(r"/devices/{did}/commands/{cid}/ack", self._command_ack),
            web.post(r"/devices/{did}/firmware/jobs/{jid}/progress",
                     self._firmware_progress),
            web.get("/firmware/artifacts/{name}", self._firmware_artifact),
            web.get("/devices", self._devices_list),
            web.get("/audit", self._audit),
        ])
        self.firmware_artifacts: dict[str, bytes] = {}
        self.firmware_progress: dict[str, list[dict]] = {}
        self.devices: dict[str, Device] = {}
        self.audit: list[dict] = []
        self.provisioning_token = "test-provision-" + pysecrets.token_hex(4)
        self._runner: Optional[web.AppRunner] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self.started_event = threading.Event()

    # --- lifecycle -----------------------------------------------------
    def start(self) -> None:
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        self.started_event.wait(timeout=5.0)

    def _serve(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._run_forever())

    async def _run_forever(self):
        self._runner = web.AppRunner(self._app)
        await self._runner.setup()
        site = web.TCPSite(self._runner, self._host, self._port)
        await site.start()
        self.started_event.set()
        while True:
            await asyncio.sleep(3600)

    def stop(self) -> None:
        if self._loop and self._runner:
            async def _cleanup():
                await self._runner.cleanup()
            asyncio.run_coroutine_threadsafe(_cleanup(), self._loop)
            self._loop.call_soon_threadsafe(self._loop.stop)

    # --- helpers -------------------------------------------------------
    @property
    def base_url(self) -> str:
        # When bound to 0.0.0.0 we have to advertise a routable IP so
        # the device can dial back. Pick whichever interface would be
        # used to reach the loopback target as a proxy for "our LAN IP".
        host = self._host
        if host in ("0.0.0.0", "::"):
            import socket as _s
            try:
                s = _s.socket(_s.AF_INET, _s.SOCK_DGRAM)
                s.connect(("8.8.8.8", 80))
                host = s.getsockname()[0]
                s.close()
            except OSError:
                host = "127.0.0.1"
        return f"http://{host}:{self._port}"

    def _log(self, event: str, **extra) -> None:
        self.audit.append({
            "ms": int(time.time() * 1000), "event": event, **extra,
        })

    def queue_command(self, device_id: str, ctype: str, payload: dict) -> str:
        cid = str(uuid.uuid4())
        cmd = Command(id=cid, type=ctype, payload=payload)
        self.devices[device_id].commands.append(cmd)
        self._log("command.created", device_id=device_id, cid=cid, type=ctype)
        return cid

    def set_config(self, device_id: str, cfg: dict) -> None:
        dev = self.devices[device_id]
        dev.config = cfg
        body = json.dumps(cfg, sort_keys=True).encode()
        dev.config_hash = hashlib.sha256(body).hexdigest()[:16]
        # Bump version
        n = int(dev.config_version[1:]) + 1
        dev.config_version = f"v{n}"
        self._log("config.set", device_id=device_id,
                  version=dev.config_version, hash=dev.config_hash)

    def lookup(self, device_id: str) -> Optional[Device]:
        return self.devices.get(device_id)

    def _check_auth(self, request: web.Request, device_id: str) -> bool:
        auth = request.headers.get("Authorization", "")
        dev = self.devices.get(device_id)
        if not dev:
            return False
        return auth == f"Bearer {dev.token}"

    # --- routes --------------------------------------------------------
    async def _discovery(self, request: web.Request) -> web.Response:
        return web.json_response({
            "protocol": PROTOCOL_VERSION,
            "server_id": "manager-mock",
            "base_url": self.base_url,
            "auth_methods": ["provisioning_token", "bearer"],
            "heartbeat_interval_ms": 30000,
            "command_poll_interval_ms": 10000,
            "features": ["registry", "config", "commands"],
        })

    async def _register(self, request: web.Request) -> web.Response:
        body = await request.json()
        device_id = body.get("deviceId")
        if not device_id:
            return web.json_response({"error": "missing deviceId"},
                                     status=400)
        # Allow re-registration: keep token if device exists.
        # Identity may come as a nested object OR as top-level fields
        # (the current firmware uses the flat form). Capture both.
        if "identity" in body:
            identity_blob = body["identity"]
        else:
            identity_blob = {k: v for k, v in body.items()
                             if k != "capabilities"}
        existing = self.devices.get(device_id)
        if existing is None:
            existing = Device(
                id=device_id,
                token=pysecrets.token_urlsafe(24),
                identity=identity_blob,
                capabilities=body.get("capabilities", {}),
            )
            self.devices[device_id] = existing
            self._log("device.registered", device_id=device_id)
        else:
            existing.identity = identity_blob
            existing.capabilities = body.get("capabilities",
                                             existing.capabilities)
            self._log("device.reregistered", device_id=device_id)
        return web.json_response({
            "deviceId": existing.id,
            "deviceToken": existing.token,
            "heartbeat_interval_ms": 30000,
            "command_poll_interval_ms": 10000,
            "config_url": f"/devices/{existing.id}/config",
        })

    async def _status(self, request: web.Request) -> web.Response:
        did = request.match_info["did"]
        if not self._check_auth(request, did):
            return web.json_response({"error": "unauthorized"}, status=401)
        dev = self.devices[did]
        body = await request.json()
        dev.last_status = body
        dev.last_seen_ms = int(time.time() * 1000)
        self._log("device.heartbeat", device_id=did)
        return web.json_response({
            "server_time_ms": int(time.time() * 1000),
            "desired_config_version": dev.config_version,
            "desired_config_hash": dev.config_hash,
            "command_count": sum(1 for c in dev.commands if c.state == "pending"),
        })

    async def _config(self, request: web.Request) -> web.Response:
        did = request.match_info["did"]
        if not self._check_auth(request, did):
            return web.json_response({"error": "unauthorized"}, status=401)
        dev = self.devices[did]
        return web.json_response({
            "version": dev.config_version,
            "hash": dev.config_hash,
            "config": dev.config,
        })

    async def _commands_get(self, request: web.Request) -> web.Response:
        did = request.match_info["did"]
        if not self._check_auth(request, did):
            return web.json_response({"error": "unauthorized"}, status=401)
        dev = self.devices[did]
        pending = [c for c in dev.commands if c.state == "pending"]
        for c in pending:
            c.state = "delivered"
            c.delivered_ms = int(time.time() * 1000)
        return web.json_response({
            "commands": [{
                "id": c.id, "type": c.type, "payload": c.payload,
                "created_ms": c.created_ms,
            } for c in pending],
        })

    async def _command_ack(self, request: web.Request) -> web.Response:
        did = request.match_info["did"]
        cid = request.match_info["cid"]
        if not self._check_auth(request, did):
            return web.json_response({"error": "unauthorized"}, status=401)
        dev = self.devices[did]
        body = await request.json()
        cmd = next((c for c in dev.commands if c.id == cid), None)
        if cmd is None:
            return web.json_response({"error": "unknown command"}, status=404)
        cmd.state = "acknowledged" if body.get("result", "ok") == "ok" \
                    else "failed"
        cmd.ack_ms = int(time.time() * 1000)
        cmd.ack_result = body.get("result")
        self._log("command.ack", device_id=did, cid=cid,
                  result=cmd.ack_result)
        return web.json_response({"ok": True})

    async def _devices_list(self, request: web.Request) -> web.Response:
        return web.json_response({
            "devices": [{
                "id": d.id, "last_seen_ms": d.last_seen_ms,
                "config_version": d.config_version,
                "config_hash": d.config_hash,
                "pending_commands": sum(1 for c in d.commands
                                        if c.state == "pending"),
            } for d in self.devices.values()],
        })

    async def _audit(self, request: web.Request) -> web.Response:
        return web.json_response({"events": self.audit})

    # --- F6 OTA -------------------------------------------------------
    async def _firmware_artifact(self, request: web.Request) -> web.Response:
        name = request.match_info["name"]
        data = self.firmware_artifacts.get(name)
        if data is None:
            return web.Response(status=404, text="unknown artifact")
        return web.Response(body=data, content_type="application/octet-stream")

    async def _firmware_progress(self, request: web.Request) -> web.Response:
        did = request.match_info["did"]
        jid = request.match_info["jid"]
        if not self._check_auth(request, did):
            return web.json_response({"error": "unauthorized"}, status=401)
        body = await request.json()
        self.firmware_progress.setdefault(jid, []).append(body)
        self._log("ota.progress", device_id=did, job_id=jid,
                  state=body.get("state"), pct=body.get("progress_pct"))
        return web.json_response({"ok": True})

    def serve_artifact(self, name: str, data: bytes) -> tuple[str, str]:
        """Register a binary at GET /firmware/artifacts/<name>. Returns
        (url, sha256_hex) for use in a firmware.update command."""
        import hashlib
        self.firmware_artifacts[name] = data
        sha = hashlib.sha256(data).hexdigest()
        return f"{self.base_url}/firmware/artifacts/{name}", sha


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=4738)
    args = ap.parse_args()
    m = ManagerMock(host=args.host, port=args.port)
    m.start()
    print(f"manager-mock listening on http://{args.host}:{args.port}")
    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        pass
    finally:
        m.stop()


if __name__ == "__main__":
    main()
