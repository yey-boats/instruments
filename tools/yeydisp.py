#!/usr/bin/env python3
"""Unified CLI for poking a yey-display MFD over BLE NUS, HTTP, and UDP.

Replaces the inline `python3 - <<PY` heredocs that have been sprinkled
through OTA wrappers, lab notes, and debugging sessions. Every operation
has a subcommand; every subcommand is non-interactive and exit-coded so
it composes in shell + Make + CI.

Subcommand summary:

  ble cmd <text>...       Send one or more BLE NUS commands, stream
                          notification lines for `--wait` seconds.
  ble ip                  Issue `ip` over BLE, print just `<ipv4>`.
  ble reboot              Issue `reboot` over BLE, return immediately.
  ble wifi-reconnect      Issue `wifi-reconnect` (recovers half-up STA).

  state [--field K]       GET /api/state, pretty-print or extract field.
  logs [--since SEQ] [--limit N]
                          GET /api/logs once.
  logs tail [--port N]    Bind UDP and stream broadcast log datagrams.

  discover                mDNS + BLE discovery, print device IP.

  recover [--timeout S]   Composite: ble wifi-reconnect, then poll for
                          ping reachability until --timeout.
  watch [--interval S]    Periodic /api/state poll with diff vs previous.

Common options:
  --device-ip IP          Pin HTTP target. Otherwise resolved via discover.
  --remote user@host      Proxy HTTP/ping through SSH (when this host
                          can't reach the device subnet directly).
  --name SUBSTRING        BLE device-name filter (default: any "yey-d*").
  --timeout SECONDS       Per-op timeout (default per-subcommand).

Configuration precedence (highest first):
  1. command-line flag                 --remote compulab@192.168.2.11
  2. process environment               YEYBOATS_REMOTE=compulab@192.168.2.11
  3. `.env.test.local` at repo root    REMOTE_HOST=compulab@192.168.2.11
     (gitignored; same file used by lab-logger/deploy.sh and the
     existing system-test tooling, so one config covers everything)
  4. `.env.test` at repo root          tracked defaults; usually has
                                       DEVICE_IP=10.42.0.67 already

Recognised env keys (and the .env aliases we also accept for
interoperability with the existing scripts):

  YEYBOATS_REMOTE      | REMOTE_HOST            -> --remote
  YEYBOATS_DEVICE_IP   | DEVICE_IP | YEYBOATS_HOST -> --device-ip
  YEYBOATS_BLE_NAME    | YEYBOATS_BLE_NAME       -> --name

Print effective config with `yeydisp config`.

Exit codes: 0 success, 1 not found / not reachable, 2 usage error.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime
import json
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import time
from typing import Optional

NUS_SERVICE = "6e400001-b5a3-f393-e0a3-9f4dd9e3a05a"
NUS_RX = "6e400002-b5a3-f393-e0a3-9f4dd9e3a05a"  # write to device
NUS_TX = "6e400003-b5a3-f393-e0a3-9f4dd9e3a05a"  # notify from device

DEFAULT_BLE_TIMEOUT = 10.0
DEFAULT_HTTP_TIMEOUT = 8.0

# Aliases for env keys we map onto our canonical YEYBOATS_* names. The
# left column wins on first match (process env), then we fall through
# to the right column - which uses the same names other repo tools
# already set in .env.test / .env.test.local. Single source of truth
# for "where does the lab IP come from" across the repo.
ENV_ALIASES: dict[str, tuple[str, ...]] = {
    "YEYBOATS_REMOTE":    ("YEYBOATS_REMOTE", "REMOTE_HOST"),
    "YEYBOATS_DEVICE_IP": ("YEYBOATS_DEVICE_IP", "DEVICE_IP", "YEYBOATS_HOST"),
    "YEYBOATS_BLE_NAME":  ("YEYBOATS_BLE_NAME",),
}

_ENV_FILE_PATTERN = re.compile(
    r"^[[:space:]]*(?:export[[:space:]]+)?([A-Z_][A-Z0-9_]*)=(.*)$"
    .replace("[[:space:]]", "[ \t]")  # POSIX class -> Python char class
)


_PARAM_SUBST = re.compile(r"\$\{([A-Z_][A-Z0-9_]*):-([^}]*)\}")
_VAR_SUBST = re.compile(r"\$\{?([A-Z_][A-Z0-9_]*)\}?")


def _strip_quotes(v: str) -> str:
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in ("'", '"'):
        return v[1:-1]
    # Strip trailing inline comment when value is unquoted.
    hash_at = v.find(" #")
    return v if hash_at < 0 else v[:hash_at].rstrip()


def _expand_value(v: str, scope: dict[str, str]) -> str:
    """Resolve `${VAR:-default}` and `$VAR` against `scope` (and
    falling back to os.environ). Matches what `.env.test` files
    in this repo use - the existing provision_device.py loader
    accepts the same syntax."""
    def repl_default(m: re.Match) -> str:
        name, default = m.group(1), m.group(2)
        return scope.get(name) or os.environ.get(name) or default

    def repl_plain(m: re.Match) -> str:
        name = m.group(1)
        return scope.get(name) or os.environ.get(name) or ""

    v = _PARAM_SUBST.sub(repl_default, v)
    v = _VAR_SUBST.sub(repl_plain, v)
    return v


def _load_env_file(path: str) -> dict[str, str]:
    """Tolerant key=value parser. Accepts `KEY=value`, `export KEY=value`,
    quoted values. Inline `# comment` allowed for unquoted values. Lines
    we don't recognize are skipped silently - this is a best-effort
    loader, not a shell."""
    out: dict[str, str] = {}
    try:
        text = open(path, "r", encoding="utf-8").read()
    except OSError:
        return out
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = _ENV_FILE_PATTERN.match(line)
        if not m:
            continue
        key = m.group(1)
        raw = _strip_quotes(m.group(2))
        # Expand against values already parsed from this file plus the
        # process env, so later lines that reference earlier keys work.
        out[key] = _expand_value(raw, out)
    return out


def load_env_defaults() -> None:
    """Populate os.environ from .env.test and .env.test.local at the
    repo root (.local wins). Only fills keys that aren't already set in
    the process environment - so a one-off `YEYBOATS_REMOTE=foo yeydisp`
    invocation overrides the file just as a CLI flag overrides env.

    Then canonicalize: for each YEYBOATS_* canonical key, if it's unset
    and an alias is set, copy the alias value over so the rest of the
    CLI only has to read the canonical name.
    """
    # tools/yeydisp.py lives in <repo>/tools/, so .. is the repo root.
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    base = _load_env_file(os.path.join(repo, ".env.test"))
    overlay = _load_env_file(os.path.join(repo, ".env.test.local"))
    base.update(overlay)
    for k, v in base.items():
        os.environ.setdefault(k, v)
    for canonical, aliases in ENV_ALIASES.items():
        if os.environ.get(canonical):
            continue
        for alias in aliases:
            val = os.environ.get(alias)
            if val:
                os.environ[canonical] = val
                break


# ---------- BLE helpers ----------

async def _ble_scan(name_filter: Optional[str], timeout: float):
    try:
        from bleak import BleakScanner
    except ImportError:
        die("bleak not installed; pip install bleak")
    needle = (name_filter or "yey-d").lower()
    return await BleakScanner.find_device_by_filter(
        lambda d, _adv: (d.name or "").lower().startswith(needle), timeout=timeout
    )


async def ble_cmd(commands: list[str], name_filter: Optional[str],
                  wait_after: float, timeout: float) -> int:
    """Open a BLE NUS connection, send each command, dump notifications
    for `wait_after` seconds. Returns 0 on connect; 1 on no device."""
    try:
        from bleak import BleakClient
    except ImportError:
        die("bleak not installed; pip install bleak")
    d = await _ble_scan(name_filter, timeout)
    if not d:
        print("no yey-d BLE device found", file=sys.stderr)
        return 1
    print(f"# ble: {d.name} {d.address}", file=sys.stderr)
    async with BleakClient(d) as c:
        out: list[str] = []

        def on_notify(_handle, data: bytearray) -> None:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()

        await c.start_notify(NUS_TX, on_notify)
        for cmd in commands:
            await c.write_gatt_char(NUS_RX, cmd.encode(), response=False)
            await asyncio.sleep(0.3)
        await asyncio.sleep(wait_after)
    sys.stdout.write("\n")
    return 0


async def ble_query_ip(name_filter: Optional[str], timeout: float) -> int:
    """Send `ip`, parse `ip=<ipv4>` from the notification stream, print
    just the IP. Returns 1 if no IP appeared within timeout."""
    try:
        from bleak import BleakClient
    except ImportError:
        die("bleak not installed; pip install bleak")
    d = await _ble_scan(name_filter, timeout)
    if not d:
        print("no yey-d BLE device found", file=sys.stderr)
        return 1
    pat = re.compile(r"ip=(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})")
    async with BleakClient(d) as c:
        got = asyncio.Event()
        ip_holder: dict[str, str] = {}

        def on_notify(_handle, data: bytearray) -> None:
            m = pat.search(data.decode("utf-8", "replace"))
            if m and m.group(1) != "0.0.0.0":
                ip_holder["ip"] = m.group(1)
                got.set()

        await c.start_notify(NUS_TX, on_notify)
        await c.write_gatt_char(NUS_RX, b"ip", response=False)
        try:
            await asyncio.wait_for(got.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            print("ip not reported within timeout", file=sys.stderr)
            return 1
    print(ip_holder["ip"])
    return 0


async def ble_fire_and_forget(cmd: str, name_filter: Optional[str],
                              timeout: float, wait_after: float = 1.0) -> int:
    """Connect, send one command, briefly wait, disconnect. Used for
    `reboot` and `wifi-reconnect` where we don't care about reply."""
    try:
        from bleak import BleakClient
    except ImportError:
        die("bleak not installed; pip install bleak")
    d = await _ble_scan(name_filter, timeout)
    if not d:
        print("no yey-d BLE device found", file=sys.stderr)
        return 1
    async with BleakClient(d) as c:
        await c.write_gatt_char(NUS_RX, cmd.encode(), response=False)
        await asyncio.sleep(wait_after)
    print(f"# ble: {cmd} sent", file=sys.stderr)
    return 0


# ---------- HTTP helpers (with optional --remote SSH proxy) ----------

def _curl_argv(url: str, timeout: float) -> list[str]:
    return ["curl", "-sS", "--max-time", str(timeout), url]


def http_get(url: str, timeout: float, remote: Optional[str]) -> tuple[int, str]:
    """Returns (returncode, stdout). Stderr is forwarded to ours so
    network errors surface. With --remote, runs curl over SSH."""
    if remote:
        argv = ["ssh", "-o", "BatchMode=yes", remote, " ".join(shlex.quote(a) for a in _curl_argv(url, timeout))]
    else:
        argv = _curl_argv(url, timeout)
    res = subprocess.run(argv, capture_output=True, text=True, timeout=timeout + 10)
    if res.stderr:
        sys.stderr.write(res.stderr)
    return res.returncode, res.stdout


def http_get_json(url: str, timeout: float, remote: Optional[str]) -> Optional[dict]:
    rc, body = http_get(url, timeout, remote)
    if rc != 0 or not body:
        return None
    try:
        return json.loads(body)
    except json.JSONDecodeError as e:
        print(f"non-json response: {e}", file=sys.stderr)
        return None


def resolve_ip(args) -> Optional[str]:
    """Return DEVICE_IP -- explicit flag first, then discover."""
    if getattr(args, "device_ip", None):
        return args.device_ip
    # Defer to existing discover_device.py to keep the discovery logic
    # in one place. Strip the resolved IP off its stdout.
    here = os.path.dirname(os.path.abspath(__file__))
    argv = ["python3", os.path.join(here, "discover_device.py")]
    if getattr(args, "name", None):
        argv += ["--name", args.name]
    res = subprocess.run(argv, capture_output=True, text=True, timeout=20)
    if res.returncode != 0:
        sys.stderr.write(res.stderr)
        return None
    return res.stdout.strip() or None


# ---------- Subcommands ----------

def cmd_state(args) -> int:
    ip = resolve_ip(args)
    if not ip:
        return 1
    doc = http_get_json(f"http://{ip}/api/state", args.timeout, args.remote)
    if doc is None:
        return 1
    if args.field:
        # Dotted path: device.firmware_version -> doc["device"]["firmware_version"]
        cur = doc
        for part in args.field.split("."):
            if not isinstance(cur, dict) or part not in cur:
                print("", file=sys.stderr)
                return 1
            cur = cur[part]
        print(cur)
    else:
        json.dump(doc, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    return 0


def cmd_logs(args) -> int:
    ip = resolve_ip(args)
    if not ip:
        return 1
    url = f"http://{ip}/api/logs?since={args.since}&limit={args.limit}"
    rc, body = http_get(url, args.timeout, args.remote)
    if rc != 0:
        return 1
    sys.stdout.write(body)
    if not body.endswith("\n"):
        sys.stdout.write("\n")
    return 0


def cmd_logs_tail(args) -> int:
    """Equivalent to tools/lab-logger/loglistener.py - included here so
    a single `yeydisp logs tail` works without the lab-logger bundle."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", args.port))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    sys.stdout.write(f"# yeydisp UDP log tail on :{args.port}\n")
    sys.stdout.flush()
    while True:
        data, addr = s.recvfrom(4096)
        ts = datetime.datetime.now().isoformat(timespec="milliseconds")
        text = data.decode("utf-8", "replace").rstrip("\r\n")
        sys.stdout.write(f"{ts}  [{addr[0]}]  {text}\n")
        sys.stdout.flush()


def cmd_discover(args) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    argv = ["python3", os.path.join(here, "discover_device.py")]
    if args.name:
        argv += ["--name", args.name]
    if args.method != "auto":
        argv += ["--method", args.method]
    if args.json:
        argv += ["--json"]
    return subprocess.run(argv).returncode


def cmd_recover(args) -> int:
    """BLE wifi-reconnect + wait for ping reachable. Used after the
    OTA wedge or any time the device's STA is half-up. With --remote
    the ping check runs there, since this host typically can't see
    the device subnet directly."""
    rc = asyncio.run(ble_fire_and_forget("wifi-reconnect", args.name, args.timeout))
    if rc != 0:
        return rc
    deadline = time.monotonic() + args.timeout
    ip = args.device_ip or resolve_ip(args)
    if not ip:
        return 1
    while time.monotonic() < deadline:
        if args.remote:
            argv = ["ssh", "-o", "BatchMode=yes", args.remote, f"ping -c 1 -W 1 {ip}"]
        else:
            argv = ["ping", "-c", "1", "-W", "1", ip]
        if subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            print(f"# {ip} reachable", file=sys.stderr)
            return 0
        time.sleep(2)
    print(f"# {ip} still unreachable after {args.timeout}s", file=sys.stderr)
    return 1


def cmd_watch(args) -> int:
    ip = resolve_ip(args)
    if not ip:
        return 1
    prev: dict = {}
    while True:
        doc = http_get_json(f"http://{ip}/api/state", args.timeout, args.remote)
        ts = datetime.datetime.now().isoformat(timespec="seconds")
        if doc is None:
            # /api/state didn't respond - DON'T mistake this for a
            # reboot. The web stack going quiet (TCP wedge / TIME_WAIT
            # pool exhaustion) trivially shows as no JSON; without
            # this guard we'd flag "uptime=0" against the previous
            # poll's uptime and print spurious `!! REBOOTED` lines.
            # Real reboots show a positive-but-smaller uptime in a
            # subsequent successful poll; that path still fires below.
            sys.stdout.write(f"{ts}  (/api/state unreachable)\n")
            sys.stdout.flush()
            time.sleep(args.interval)
            continue
        dev = doc.get("device", {})
        mgr = doc.get("manager", {})
        sk = doc.get("sk", {})
        line = (f"{ts}  heap={dev.get('heap_free', 0) // 1024}k "
                f"psram={dev.get('psram_free', 0) // 1024}k "
                f"sk={sk.get('state')} task_iters={sk.get('task_iters')} "
                f"mgr.hb={mgr.get('lastHeartbeatCode')} "
                f"uptime={dev.get('uptime_ms', 0) // 1000}s")
        # Flag uptime regression (device rebooted) loud. Only meaningful
        # when both prev and current polls succeeded.
        prev_up = prev.get("device", {}).get("uptime_ms", 0)
        cur_up = dev.get("uptime_ms", 0)
        if prev_up and cur_up and cur_up < prev_up:
            sys.stdout.write(f"!! REBOOTED (prev_uptime={prev_up // 1000}s)\n")
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
        prev = doc
        time.sleep(args.interval)


def _soak_sample(doc: Optional[dict], t: int):
    """Map a /api/state doc into a soak_verdict.Sample. A None doc (device
    didn't respond) becomes an uptime_ms=0 row, which analyze() treats as a
    non-responding tick rather than a reboot/stall."""
    from soak_verdict import Sample
    if doc is None:
        return Sample(t=t, uptime_ms=0, heap=0, sk_live=False)
    dev = doc.get("device", {})
    sk = doc.get("sk", {})
    # Prefer an explicit staleness age if the firmware exposes one; the field
    # name has varied, so probe a few before falling back to the state flag.
    age = None
    for key in ("last_update_age_ms", "lastUpdateAgeMs", "age_ms", "data_age_ms"):
        if isinstance(sk.get(key), int):
            age = sk[key]
            break
    return Sample(
        t=t,
        uptime_ms=int(dev.get("uptime_ms", 0) or 0),
        heap=int(dev.get("heap_free", 0) or 0),
        sk_live=(sk.get("state") == "live"),
        sk_age_ms=age,
    )


def cmd_soak(args) -> int:
    """Unattended stability soak: poll /api/state on an interval, append each
    tick to JSONL, and print a pass/fail verdict (reboots, stalls, min heap)
    at the end. Run on a host on the device subnet (e.g. via
    --remote compulab@mythra-nav). Stops on Ctrl-C or after --duration."""
    from soak_verdict import analyze, format_verdict
    ip = resolve_ip(args)
    if not ip:
        return 1
    out_path = args.out or f"soak-{datetime.datetime.now():%Y%m%d-%H%M%S}.jsonl"
    samples = []
    start = time.time()
    sys.stdout.write(f"# soak -> {out_path} (interval {args.interval}s, "
                     f"target http://{ip}/api/state)\n")
    sys.stdout.flush()
    try:
        with open(out_path, "a", buffering=1) as fh:
            while True:
                t = int(time.time() - start)
                doc = http_get_json(f"http://{ip}/api/state", args.timeout, args.remote)
                s = _soak_sample(doc, t)
                samples.append(s)
                ts = datetime.datetime.now().isoformat(timespec="seconds")
                rec = {"ts": ts, "t": t, "uptime_ms": s.uptime_ms,
                       "heap": s.heap, "sk_live": s.sk_live,
                       "sk_age_ms": s.sk_age_ms,
                       "reachable": doc is not None}
                fh.write(json.dumps(rec) + "\n")
                state = "live" if s.sk_live else ("--" if doc is None else "stalled")
                sys.stdout.write(f"{ts}  t={t}s  up={s.uptime_ms // 1000}s  "
                                 f"heap={s.heap // 1024}k  sk={state}\n")
                sys.stdout.flush()
                if args.duration and (time.time() - start) >= args.duration:
                    break
                time.sleep(args.interval)
    except KeyboardInterrupt:
        sys.stdout.write("\n# interrupted\n")
    v = analyze(samples, stall_ms=int(args.stall_ms))
    sys.stdout.write(format_verdict(v) + "\n")
    return 0 if v.passed else 1


# ---------- CLI plumbing ----------

def die(msg: str, code: int = 2) -> None:
    print(msg, file=sys.stderr)
    sys.exit(code)


def add_common_args(p: argparse.ArgumentParser) -> None:
    # Defaults resolved at call time so load_env_defaults() runs first
    # in main() and we pick up any .env.test.local values.
    p.add_argument("--device-ip", default=None,
                   help="Pin HTTP target; otherwise auto-discover. "
                        "Falls back to $YEYBOATS_DEVICE_IP / $DEVICE_IP.")
    p.add_argument("--remote", default=None,
                   help="user@host SSH relay for HTTP/ping operations. "
                        "Falls back to $YEYBOATS_REMOTE / $REMOTE_HOST.")
    p.add_argument("--name", default=None,
                   help="BLE device-name filter (default: any 'yey-d*'). "
                        "Falls back to $YEYBOATS_BLE_NAME.")
    p.add_argument("--timeout", type=float, default=DEFAULT_HTTP_TIMEOUT,
                   help=f"Per-op timeout in seconds (default {DEFAULT_HTTP_TIMEOUT}).")


def apply_env_defaults(args: argparse.Namespace) -> None:
    """Fill unset args from env (after load_env_defaults canonicalized
    them). Keeps the precedence rule simple: CLI flag wins; otherwise
    env (which already includes .env.test*); otherwise None/default."""
    if getattr(args, "device_ip", None) is None:
        args.device_ip = os.environ.get("YEYBOATS_DEVICE_IP")
    if getattr(args, "remote", None) is None:
        args.remote = os.environ.get("YEYBOATS_REMOTE")
    if getattr(args, "name", None) is None:
        args.name = os.environ.get("YEYBOATS_BLE_NAME")


def cmd_config(args) -> int:
    """Dump effective configuration sources + resolved values, so a
    user can verify which .env file fed which knob without grepping."""
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    base = _load_env_file(os.path.join(repo, ".env.test"))
    overlay = _load_env_file(os.path.join(repo, ".env.test.local"))
    print(f"# repo root: {repo}")
    print(f"# .env.test           : {'present' if base else 'missing'}")
    print(f"# .env.test.local     : {'present' if overlay else 'missing'}")
    print()
    for canonical in ENV_ALIASES:
        val = os.environ.get(canonical, "")
        if not val:
            print(f"{canonical:20s} = (unset)")
            continue
        # Best-effort source attribution: did the value come from
        # process env directly, .env.test.local, or .env.test?
        source = "process-env"
        for alias in ENV_ALIASES[canonical]:
            if alias in overlay and overlay[alias] == val:
                source = ".env.test.local"
                break
            if alias in base and base[alias] == val:
                source = ".env.test"
                break
        print(f"{canonical:20s} = {val:30s}   [{source}]")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    # Load .env.test* before argparse so help text already sees any
    # operator-set defaults. CLI flags still win - they're applied
    # later via apply_env_defaults() which only fills unset values.
    load_env_defaults()

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_config = sub.add_parser("config", help="Show resolved config sources")

    # --- ble ---
    p_ble = sub.add_parser("ble", help="BLE NUS operations")
    ble_sub = p_ble.add_subparsers(dest="ble_cmd", required=True)

    p_ble_cmd = ble_sub.add_parser("cmd", help="Send commands, stream notifications")
    add_common_args(p_ble_cmd)
    p_ble_cmd.add_argument("commands", nargs="+", help="Commands to send (one per arg).")
    p_ble_cmd.add_argument("--wait", type=float, default=3.0,
                           help="Seconds to keep listening after sending (default 3).")

    p_ble_ip = ble_sub.add_parser("ip", help="Get current IP from device")
    add_common_args(p_ble_ip)

    p_ble_reboot = ble_sub.add_parser("reboot", help="Reboot the device")
    add_common_args(p_ble_reboot)

    p_ble_wifi = ble_sub.add_parser("wifi-reconnect", help="Force WiFi reconnect")
    add_common_args(p_ble_wifi)

    # --- state / logs ---
    p_state = sub.add_parser("state", help="Fetch /api/state")
    add_common_args(p_state)
    p_state.add_argument("--field", default=None,
                         help="Dotted JSON path (e.g. device.firmware_version) to print.")

    p_logs = sub.add_parser("logs", help="Fetch /api/logs or tail UDP broadcasts")
    logs_sub = p_logs.add_subparsers(dest="logs_cmd", required=False)
    add_common_args(p_logs)
    p_logs.add_argument("--since", type=int, default=0, help="Starting sequence number.")
    p_logs.add_argument("--limit", type=int, default=32, help="Max entries (cap 96).")

    p_logs_tail = logs_sub.add_parser("tail", help="UDP listener (debug FW only)")
    p_logs_tail.add_argument("--port", type=int, default=9999)

    # --- discover ---
    p_disc = sub.add_parser("discover", help="Locate device (mDNS, BLE)")
    p_disc.add_argument("--name", default=None)
    p_disc.add_argument("--method", choices=("auto", "mdns", "ble"), default="auto")
    p_disc.add_argument("--json", action="store_true")

    # --- composite ---
    p_recover = sub.add_parser("recover", help="BLE wifi-reconnect + wait for ping")
    add_common_args(p_recover)

    p_watch = sub.add_parser("watch", help="Periodic /api/state poll with diff")
    add_common_args(p_watch)
    p_watch.add_argument("--interval", type=float, default=5.0)

    p_soak = sub.add_parser("soak", help="Unattended stability soak -> JSONL + verdict")
    add_common_args(p_soak)
    p_soak.add_argument("--interval", type=float, default=30.0,
                        help="Seconds between polls (default 30).")
    p_soak.add_argument("--duration", type=float, default=0,
                        help="Stop after N seconds (0 = until Ctrl-C).")
    p_soak.add_argument("--stall-ms", dest="stall_ms", type=float, default=30000,
                        help="Data-age threshold for a stall (default 30000).")
    p_soak.add_argument("--out", default=None,
                        help="JSONL output path (default soak-<ts>.jsonl).")

    args = parser.parse_args(argv)
    apply_env_defaults(args)

    if args.cmd == "config":
        return cmd_config(args)
    if args.cmd == "ble":
        if args.ble_cmd == "cmd":
            return asyncio.run(ble_cmd(args.commands, args.name, args.wait, args.timeout))
        if args.ble_cmd == "ip":
            return asyncio.run(ble_query_ip(args.name, args.timeout))
        if args.ble_cmd == "reboot":
            return asyncio.run(ble_fire_and_forget("reboot", args.name, args.timeout))
        if args.ble_cmd == "wifi-reconnect":
            return asyncio.run(ble_fire_and_forget("wifi-reconnect", args.name, args.timeout))
    if args.cmd == "state":
        return cmd_state(args)
    if args.cmd == "logs":
        if getattr(args, "logs_cmd", None) == "tail":
            return cmd_logs_tail(args)
        return cmd_logs(args)
    if args.cmd == "discover":
        return cmd_discover(args)
    if args.cmd == "recover":
        return cmd_recover(args)
    if args.cmd == "watch":
        return cmd_watch(args)
    if args.cmd == "soak":
        return cmd_soak(args)
    parser.error(f"unknown command: {args.cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
