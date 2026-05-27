"""Parse `bench` output captured from UDP log into a structured dict.

The device's `bench` command emits multiple [bench] lines covering FPS,
flush latency, latency histograms (RenderLatency, FrameInterval,
CommandRtt, SkAge), heap, queues, etc. We capture them by:

  1. Drain the UDP log queue
  2. POST `latency-reset` so the histograms start fresh (optional)
  3. POST `bench` (or send via console)
  4. Collect all [bench] lines for ~1 s
  5. Parse known patterns

The parser tolerates missing lines (older firmware) and returns NaNs
for absent fields.
"""
from __future__ import annotations

import math
import re
import time
from dataclasses import dataclass, field


@dataclass
class LatencyStat:
    count: int = 0
    min_us: float = math.nan
    avg_us: float = math.nan
    max_us: float = math.nan


@dataclass
class BenchSnapshot:
    fps: float = math.nan
    flush_avg_us: float = math.nan
    flush_peak_us: float = math.nan
    loop_peak_us: float = math.nan
    lvgl_peak_us: float = math.nan
    heap_kb: float = math.nan
    heap_low_kb: float = math.nan
    psram_kb: float = math.nan
    ui_queue: int = -1
    net_queue: int = -1
    gestures: int = -1
    gestures_suppressed: int = -1
    last_gesture: str = ""
    latencies: dict[str, LatencyStat] = field(default_factory=dict)


_FPS = re.compile(r"fps=([\d.]+)")
_FLUSH = re.compile(r"flush avg=(\d+)\s*us\s+peak=(\d+)")
_LOOP = re.compile(r"loop peak=(\d+)\s*us.*lvgl peak=(\d+)")
_HEAP = re.compile(r"heap free=(\d+) KB\s+low-water=(\d+) KB")
_PSRAM = re.compile(r"psram free=(\d+) / (\d+) KB")
_QUEUES = re.compile(r"queues ui=(\d+).*net=(\d+)")
_GESTURE = re.compile(r"gestures: (\d+) \(sup (\d+)\)\s+last=(\S+)")
_LAT_EMPTY = re.compile(r"lat (\S+)\s+n=0")
_LAT = re.compile(
    r"lat (\S+)\s+n=(\d+)\s+min=(\d+)\s*us\s+avg=(\d+)\s*us\s+max=(\d+)\s*us")


def collect(udp_logs, device, *,
            reset: bool = False,
            settle_s: float = 1.0) -> BenchSnapshot:
    """Run `bench` via /api/cmd (bench is *not* in the injection set so
    HTTP is allowed) and parse the lines that follow on UDP log."""
    if reset:
        device.post_cmd("latency-reset")
        time.sleep(0.2)
    udp_logs.drain()
    device.post_cmd("bench")
    time.sleep(settle_s)
    lines = [ln for ln in udp_logs.drain() if "[bench]" in ln]
    return parse(lines)


def parse(lines: list[str]) -> BenchSnapshot:
    snap = BenchSnapshot()
    for ln in lines:
        if m := _FPS.search(ln):
            snap.fps = float(m.group(1))
        if m := _FLUSH.search(ln):
            snap.flush_avg_us = float(m.group(1))
            snap.flush_peak_us = float(m.group(2))
        if m := _LOOP.search(ln):
            snap.loop_peak_us = float(m.group(1))
            snap.lvgl_peak_us = float(m.group(2))
        if m := _HEAP.search(ln):
            snap.heap_kb = float(m.group(1))
            snap.heap_low_kb = float(m.group(2))
        if m := _PSRAM.search(ln):
            snap.psram_kb = float(m.group(1))
        if m := _QUEUES.search(ln):
            snap.ui_queue = int(m.group(1))
            snap.net_queue = int(m.group(2))
        if m := _GESTURE.search(ln):
            snap.gestures = int(m.group(1))
            snap.gestures_suppressed = int(m.group(2))
            snap.last_gesture = m.group(3)
        if m := _LAT_EMPTY.search(ln):
            snap.latencies[m.group(1)] = LatencyStat(count=0)
        elif m := _LAT.search(ln):
            snap.latencies[m.group(1)] = LatencyStat(
                count=int(m.group(2)),
                min_us=float(m.group(3)),
                avg_us=float(m.group(4)),
                max_us=float(m.group(5)),
            )
    return snap
