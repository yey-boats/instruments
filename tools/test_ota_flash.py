"""instruments#46: tools/ota_flash.sh post-flash verification.

Runs the real script against a local stub device: a fake espota.py, a `ping`
shim, and a `curl` shim that points http://127.0.0.1/ at a local HTTP server
on an ephemeral port. The server enforces Basic Auth on /api/state the way a
provisioned device does (#21/#30).

Run: python3 -m unittest discover -s tools -p 'test_ota_flash.py'
"""

import base64
import json
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

TOOLS = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(TOOLS, "ota_flash.sh")
MARKER = "Nov 13 2026 12:34:56"
USER, PASSWORD = "espdisp", "s3cret \"pa\\ss\": with colon"


class _Device(BaseHTTPRequestHandler):
    require_auth = True
    seen_auth = []

    def do_GET(self):  # noqa: N802 (http.server API)
        if self.path != "/api/state":
            self.send_response(404)
            self.end_headers()
            return
        got = self.headers.get("Authorization", "")
        type(self).seen_auth.append(got)
        want = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()
        if self.require_auth and got != want:
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="espdisp"')
            self.end_headers()
            self.wfile.write(b"auth required")
            return
        body = json.dumps({"device": {"build": MARKER}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class OtaFlashTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        _Device.seen_auth = []
        _Device.require_auth = True
        self.srv = HTTPServer(("127.0.0.1", 0), _Device)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        port = self.srv.server_address[1]

        bindir = os.path.join(self.tmp, "bin")
        os.mkdir(bindir)
        real_curl = shutil.which("curl")
        self.assertIsNotNone(real_curl, "curl is required")
        self._shim(bindir, "curl",
                   f'args=(); for a in "$@"; do args+=("${{a//http:\\/\\/127.0.0.1\\//http:\\/\\/127.0.0.1:{port}\\/}}"); done\n'
                   f'exec {real_curl} "${{args[@]}}"\n')
        self.ping_ok = os.path.join(self.tmp, "ping_ok")
        open(self.ping_ok, "w").close()
        self._shim(bindir, "ping", f'test -e {self.ping_ok}\n')
        self._shim(bindir, "sleep", "exit 0\n")

        self.espota = os.path.join(self.tmp, "espota.py")
        with open(self.espota, "w") as f:
            f.write("print('Uploading 100%')\n")
        self.fw = os.path.join(self.tmp, "firmware.bin")
        with open(self.fw, "wb") as f:
            f.write(b"\0\0junk\0" + MARKER.encode() + b"\0more\0")

        self.env = dict(os.environ)
        self.env["PATH"] = bindir + os.pathsep + self.env["PATH"]
        for k in ("YEYBOATS_WEB_USER", "YEYBOATS_WEB_PASSWORD",
                  "YEYBOATS_OTA_PASSWORD", "OTA_PASSWORD"):
            self.env.pop(k, None)

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    @staticmethod
    def _shim(bindir, name, body):
        path = os.path.join(bindir, name)
        with open(path, "w") as f:
            f.write("#!/usr/bin/env bash\n" + body)
        os.chmod(path, 0o755)

    def _run(self, *extra, env=None):
        return subprocess.run(
            ["bash", SCRIPT, "127.0.0.1", self.fw, "--espota", self.espota, *extra],
            env=env or self.env, capture_output=True, text=True, timeout=60)

    def test_sends_web_credentials_from_env(self):
        self.env["YEYBOATS_WEB_PASSWORD"] = PASSWORD
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK - device is running the just-flashed firmware", r.stdout)

    def test_password_never_on_command_line(self):
        self.env["YEYBOATS_WEB_PASSWORD"] = PASSWORD
        log = os.path.join(self.tmp, "argv.log")
        port_shim = shutil.which("curl", path=self.env["PATH"])
        bin2 = os.path.join(self.tmp, "bin2")
        os.mkdir(bin2)
        self._shim(bin2, "curl", f'printf "%s\\n" "$*" >> {log}\nexec {port_shim} "$@"\n')
        self.env["PATH"] = bin2 + os.pathsep + self.env["PATH"]
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(log) as f:
            self.assertNotIn("s3cret", f.read())

    def test_401_without_credentials_is_a_clear_error(self):
        r = self._run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("401", r.stderr)
        self.assertIn("YEYBOATS_WEB_PASSWORD", r.stderr)

    def test_open_device_needs_no_credentials(self):
        _Device.require_auth = False
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(_Device.seen_auth, [""])

    def test_ble_fallback_without_bleak_says_so(self):
        os.remove(self.ping_ok)  # device never comes back
        # A python3 that cannot import bleak, whatever the host has installed.
        stub = os.path.join(self.tmp, "nobleak")
        os.mkdir(stub)
        with open(os.path.join(stub, "bleak.py"), "w") as f:
            f.write("raise ImportError('No module named bleak')\n")
        self.env["PYTHONPATH"] = stub
        self.env["YEYBOATS_OTA_BOOT_SOFT"] = "1"
        self.env["YEYBOATS_OTA_BOOT_HARD"] = "2"
        r = self._run()
        self.assertNotEqual(r.returncode, 0)
        out = r.stdout + r.stderr
        self.assertIn("pip install bleak", out)
        self.assertNotIn("Traceback", out)


if __name__ == "__main__":
    unittest.main()
