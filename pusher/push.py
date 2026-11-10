#!/usr/bin/env python3
"""Push frames to an ff-eink panel on a fixed schedule and log every outcome.

    python3 push.py --host 192.168.1.50 out/*.bin --count 1440 --chart-ends \
        --log run.csv --pull-file PULL

Each tick (default every 60 s): wait for the panel to be idle, POST the next
frame, then poll /api/v1/status until the device reports it shown
(state == idle and last_seq == ours) or the tick ends. A frame not shown
within its tick is a miss, and every miss gets exactly one cause; the first
rule that matches wins (docs/api.md, "Misses"):

    deliberate_pull  the operator had the pull file present (touch it before
                     pulling a cable, remove it once the device is back)
    reboot           boot_count changed
    link_down        the device counted a link-down
    panel_error      the device counted a panel error
    slow_refresh     shown late, still refreshing at the deadline, or refused
                     busy because the previous frame was still refreshing
    http_error       the POST failed or was refused for another reason
    unattributed     none of the above: the pass criteria allow none
    no_evidence      the run ended before the device answered again

When the device is unreachable, misses wait in a queue until it answers
again, because only then can its counters say what happened.
Standard library only.
"""
from __future__ import annotations

import argparse
import csv
import http.client
import json
import os
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dither  # noqa: E402

CAUSES = ("deliberate_pull", "reboot", "link_down", "panel_error", "slow_refresh", "http_error", "unattributed",
          "no_evidence")


@dataclass
class Evidence:
    """What one missed tick saw. Built by push(), consumed by classify()."""

    seq: int = 0
    pulled: bool = False
    accepted: bool = False  # POST returned 202
    http_status: int | None = None  # None: no HTTP response at all
    busy_refreshing: bool = False  # 429 while the previous frame still refreshed
    before: dict | None = None  # last status seen before the POST
    after: list[dict] = field(default_factory=list)  # statuses seen after it


def ready(ev: Evidence) -> bool:
    """Can this miss be classified yet?

    Only once the device has answered with the refresh concluded (idle) or
    shown it rebooted. A miss whose refresh is still running when the tick
    ends would otherwise be judged before a late panel_error (e.g. the 120 s
    drf_timeout) is counted.
    """
    if ev.pulled:
        return True
    ref = ev.before
    return any(s["state"] == "idle" or (ref is not None and s["boot_count"] != ref["boot_count"]) for s in ev.after)


def reboot_detail(ev: Evidence) -> str:
    ref = ev.before or {}
    for s in ev.after:
        if s["boot_count"] != ref.get("boot_count") or s["uptime_s"] < ref.get("uptime_s", 0):
            return f"reset_reason={s.get('reset_reason')} restart_cause={s.get('restart_cause')}"
    return ""


def classify(ev: Evidence) -> str:
    """One cause per miss, first match wins (order is part of the contract)."""
    if ev.pulled:
        return "deliberate_pull"
    ref = ev.before
    later = ev.after
    if ref is not None and any(s["boot_count"] != ref["boot_count"] or s["uptime_s"] < ref["uptime_s"] for s in later):
        return "reboot"
    if ref is not None and any(s["link_downs"] > ref["link_downs"] for s in later):
        return "link_down"
    if ref is not None and any(s["panel_errors"] > ref["panel_errors"] for s in later):
        return "panel_error"
    if ev.busy_refreshing:
        return "slow_refresh"  # the previous frame overran into this tick
    if ev.accepted and any(s["last_seq"] == ev.seq or s["state"] != "idle" for s in later):
        return "slow_refresh"  # shown late, or still refreshing at the deadline
    if not ev.accepted:
        return "http_error"
    return "unattributed"  # accepted, then idle without showing it and no counter moved


class Device:
    def __init__(self, host: str, port: int = 80):
        self.host, self.port = host, port

    def _request(self, method: str, path: str, body: bytes | None, headers: dict, timeout: float):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=headers)
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

    def status(self) -> dict | None:
        try:
            code, data = self._request("GET", "/api/v1/status", None, {}, 3)
            return json.loads(data) if code == 200 else None
        except (OSError, http.client.HTTPException, ValueError):
            return None

    def post_frame(self, frame: bytes, seq: int) -> tuple[int | None, str]:
        headers = {"Content-Type": "application/octet-stream", "X-Frame-Seq": str(seq)}
        try:
            code, data = self._request("POST", "/api/v1/frame", frame, headers, 20)
            return code, data.decode(errors="replace")
        except (OSError, http.client.HTTPException) as e:
            return None, f"{type(e).__name__}: {e}"


def utc(ts: float) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).isoformat(timespec="seconds")


def run(args: argparse.Namespace) -> int:
    frames = [p.read_bytes() for p in args.frames]
    for p, f in zip(args.frames, frames):
        if len(f) != dither.FRAME_BYTES:
            sys.exit(f"{p}: {len(f)} bytes, expected {dither.FRAME_BYTES}")
    chart = dither.chart() if args.chart_ends else None
    if not frames and chart is None:
        sys.exit("no frames: give .bin files and/or --chart-ends")

    dev = Device(args.host, args.port)
    pull = Path(args.pull_file) if args.pull_file else None
    base_seq = int(time.time())  # unique per run, so a stale last_seq never matches
    log = open(args.log, "a", newline="")
    out = csv.writer(log)
    if log.tell() == 0:
        out.writerow(["tick", "seq", "scheduled_utc", "http", "post_s", "shown", "shown_after_s",
                      "refresh_ms", "cause", "detail"])

    counts = {"pushed": 0, "shown": 0, **{c: 0 for c in CAUSES}}
    pending: list[tuple[list, Evidence]] = []  # misses waiting for device evidence
    last_status = dev.status()
    t0 = time.time()

    def remember(s: dict | None) -> None:
        nonlocal last_status
        if s is not None:
            last_status = s
            for row, ev in pending:
                if not ready(ev):  # freeze evidence once sufficient, so a
                    ev.after.append(s)  # later tick's event cannot claim it

    def settle(final: bool = False) -> None:
        """Write queued misses once their evidence is in (see ready()).

        At exit, a miss the device never answered for is `no_evidence`
        rather than a guess."""
        for item in list(pending):
            row, ev = item
            if not (ready(ev) or final):
                continue
            pending.remove(item)
            cause = classify(ev) if (ev.after or ev.pulled) else "no_evidence"
            if cause == "reboot":
                row[9] = (reboot_detail(ev) + " " + row[9]).strip()
            counts[cause] += 1
            row[8] = cause
            out.writerow(row)
            print(f"tick {row[0]} MISS {cause} {row[9]}", flush=True)
        log.flush()

    try:
        for i in range(args.count):
            sched = t0 + i * args.interval
            time.sleep(max(0.0, sched - time.time()))
            deadline = sched + args.interval - min(2.0, args.interval / 10)
            seq = base_seq + i
            if chart is not None and (i == 0 or i == args.count - 1):
                frame = chart
            else:
                frame = frames[i % len(frames)] if frames else chart

            ev = Evidence(seq=seq, pulled=bool(pull and pull.exists()))
            s = dev.status()
            remember(s)
            settle()
            while s is not None and s["state"] != "idle" and time.time() < sched + args.interval / 4:
                time.sleep(args.poll)
                s = dev.status()
                remember(s)
            ev.before = last_status

            tp = time.time()
            code, detail = dev.post_frame(frame, seq)
            post_s = time.time() - tp
            ev.http_status = code
            ev.accepted = code == 202
            ev.busy_refreshing = code == 429 and s is not None and s["state"] == "refreshing"

            shown_after = None
            refresh_ms = ""
            while ev.accepted and time.time() < deadline:
                time.sleep(args.poll)
                s = dev.status()
                if s is None:
                    continue
                ev.after.append(s)
                remember(s)
                if s["state"] == "idle" and s["last_seq"] == seq:
                    shown_after = time.time() - tp
                    refresh_ms = s["last_refresh_ms"]
                    break
                if s["state"] == "idle":
                    break  # finished without showing ours: panel error or reboot
            if ev.after == [] and code is not None:
                s = dev.status()
                if s is not None:
                    ev.after.append(s)
                    remember(s)
            ev.pulled = ev.pulled or bool(pull and pull.exists())

            row = [i, seq, utc(sched), code if code is not None else "", f"{post_s:.2f}",
                   int(shown_after is not None), f"{shown_after:.1f}" if shown_after else "",
                   refresh_ms, "", detail.strip()[:120] if shown_after is None else ""]
            # Counted only here, so a tick cut short by Ctrl-C is neither
            # pushed nor missed (its row would be incomplete).
            counts["pushed"] += 1
            if shown_after is not None:
                counts["shown"] += 1
                out.writerow(row)
                log.flush()
                print(f"tick {i} shown in {shown_after:.1f} s (refresh {refresh_ms} ms)", flush=True)
            else:
                pending.append((row, ev))
                settle()
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
    finally:
        remember(dev.status())
        settle(final=True)
        log.close()

    missed = counts["pushed"] - counts["shown"]
    print(json.dumps({**counts, "missed": missed}, indent=1))
    return 0 if counts["unattributed"] == 0 and counts["no_evidence"] == 0 else 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="*", type=Path, help=".bin frames from dither.py, cycled")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--interval", type=float, default=60.0, help="seconds between frames")
    ap.add_argument("--poll", type=float, default=1.0, help="status poll period, seconds")
    ap.add_argument("--count", type=int, default=1440, help="frames to push (1440 = 24 h at 60 s)")
    ap.add_argument("--chart-ends", action="store_true", help="first and last frame are the chart")
    ap.add_argument("--log", default="push.csv", help="CSV log, appended")
    ap.add_argument("--pull-file", default=os.environ.get("FF_EINK_PULL_FILE"),
                    help="while this file exists, misses count as deliberate_pull")
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
