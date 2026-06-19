"""Pure verdict logic for espdisp soak runs.

No network, no I/O — just analysis of a sequence of polled samples so it can
be unit-tested on the host. The `yeydisp.py soak` subcommand feeds real
/api/state + /api/diag scrapes into Sample rows and calls analyze() at the end.
"""
from dataclasses import dataclass
from typing import List, Optional


@dataclass
class Sample:
    t: int                      # seconds since soak start
    uptime_ms: int              # device.uptime_ms
    heap: int                   # device.heap_free (internal heap bytes)
    sk_live: bool               # sk.state == "live"
    sk_age_ms: Optional[int] = None  # explicit data-staleness age if exposed


@dataclass
class Verdict:
    reboots: int
    stalls: int
    min_heap: int
    samples: int
    usable: int                 # samples where the device actually responded
    passed: bool


# A device that responds is "stalled" when its SignalK data has not advanced.
# Prefer an explicit age field when the firmware exposes one; otherwise fall
# back to the sk.state liveness flag.
def _is_stalled(s: Sample, stall_ms: int) -> bool:
    if s.sk_age_ms is not None:
        return s.sk_age_ms >= stall_ms
    return not s.sk_live


def analyze(samples: List[Sample], stall_ms: int = 30000) -> Verdict:
    reboots = 0
    stalls = 0
    heaps = [s.heap for s in samples if s.heap > 0]
    min_heap = min(heaps) if heaps else 0

    for i in range(1, len(samples)):
        prev, cur = samples[i - 1], samples[i]
        # uptime going backwards (beyond small jitter) means the device
        # rebooted between polls. Only meaningful when both polls have a
        # positive uptime (a non-responding poll carries uptime_ms=0).
        if prev.uptime_ms > 0 and cur.uptime_ms > 0 and \
                cur.uptime_ms + 5000 < prev.uptime_ms:
            reboots += 1

    usable = 0
    for s in samples:
        if s.uptime_ms <= 0:
            continue  # device didn't respond this tick; not a stall signal
        usable += 1
        if _is_stalled(s, stall_ms):
            stalls += 1

    passed = reboots == 0 and stalls == 0 and usable > 0
    return Verdict(reboots=reboots, stalls=stalls, min_heap=min_heap,
                   samples=len(samples), usable=usable, passed=passed)


def format_verdict(v: Verdict) -> str:
    rows = [
        ("samples", str(v.samples)),
        ("usable (responded)", str(v.usable)),
        ("reboots", str(v.reboots)),
        ("stalls", str(v.stalls)),
        ("min heap", f"{v.min_heap // 1024} KiB" if v.min_heap else "n/a"),
        ("verdict", "PASS" if v.passed else "FAIL"),
    ]
    width = max(len(k) for k, _ in rows)
    lines = ["", "==== soak verdict ===="]
    lines += [f"  {k.ljust(width)} : {val}" for k, val in rows]
    return "\n".join(lines)
