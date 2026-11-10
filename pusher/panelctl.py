#!/usr/bin/env python3
"""Provision and inspect ff-eink panels on the panel VLAN. Standard library only.

    python3 panelctl.py status 192.168.1.50
    python3 panelctl.py name 192.168.1.50 lobby-r03-c07
    python3 panelctl.py scan 192.168.1.0/24
    python3 panelctl.py update --image ff-eink-v0.2.0.bin 192.168.1.0/24

`scan` asks every address in the network for /api/v1/status and prints one
line per panel (name, hw_id, IP, firmware, state), sorted by name: the
inventory of the wall. A spare swapped in for a dead panel gets the dead
panel's name with `name`.

`update` rolls a firmware image out over the LAN (docs/ota.md): one canary
panel first, then batches, each panel only while its display is idle; a
panel counts as updated only when it reports the new version AND
ota_state "valid" (the image proved itself; otherwise the panel rolls back
on its own). Re-running it finishes whatever is left.
"""
from __future__ import annotations

import argparse
import csv
import http.client
import ipaddress
import json
import re
import struct
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Same rule as main/name_rules.h (DNS label, lower case, 1-32 chars).
NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$")


def request(host: str, method: str, path: str, body: dict | None = None, timeout: float = 3.0):
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        data = json.dumps(body).encode() if body is not None else None
        headers = {"Content-Type": "application/json"} if data else {}
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        return resp.status, json.loads(resp.read() or b"null")
    finally:
        conn.close()


def probe(ip: str, timeout: float) -> dict | None:
    try:
        code, st = request(ip, "GET", "/api/v1/status", timeout=timeout)
    except (OSError, http.client.HTTPException, ValueError):
        return None
    if code != 200 or not isinstance(st, dict) or "hw_id" not in st:
        return None  # something else answers HTTP on this address
    return {**st, "_ip": ip}


# --- firmware image -------------------------------------------------------

ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9
APP_DESC_MAGIC = 0xABCD5432
MAX_IMAGE = 3 * 1024 * 1024  # one OTA slot (partitions.csv)


def image_version(data: bytes) -> str:
    """Validate an ESP-IDF app image for ESP32-S3; return its version.

    Layout: 24-byte image header (magic at 0, chip id u16 at 12), then the
    first segment's 8-byte header; esp_app_desc_t starts the first segment
    (magic u32 at 32, version char[32] at 48).
    """
    if len(data) < 256 or len(data) > MAX_IMAGE:
        raise ValueError(f"image size {len(data)} not in 256..{MAX_IMAGE}")
    if data[0] != ESP_IMAGE_MAGIC:
        raise ValueError("not an ESP app image (bad magic)")
    if struct.unpack_from("<H", data, 12)[0] != ESP32S3_CHIP_ID:
        raise ValueError("image is not for ESP32-S3")
    if struct.unpack_from("<I", data, 32)[0] != APP_DESC_MAGIC:
        raise ValueError("no app description in image")
    # A signed image ends with a 4 KB signature sector whose first block
    # starts 0xE7 0x02 (Secure Boot V2 format). The panel checks the
    # signature itself; this only catches an unsigned build early.
    if len(data) % 4096 or data[-4096] != 0xE7 or data[-4095] != 0x02:
        raise ValueError("image is not signed (build with signing-key.pem; docs/ota.md)")
    return data[48:80].split(b"\0", 1)[0].decode()


def post_image(ip: str, data: bytes) -> tuple[int, dict | None]:
    conn = http.client.HTTPConnection(ip, 80, timeout=90)
    try:
        conn.request("POST", "/api/v1/firmware", body=data,
                     headers={"Content-Type": "application/octet-stream"})
        resp = conn.getresponse()
        return resp.status, json.loads(resp.read() or b"null")
    finally:
        conn.close()


def wait_valid(ip: str, version: str, deadline: float, poll: float, old_boot=None) -> str:
    """Wait until the panel runs `version` with ota_state "valid".

    With old_boot set, only statuses from a newer boot count (the panel has
    restarted into the uploaded image)."""
    while time.time() < deadline:
        time.sleep(poll)
        st = probe(ip, 4.0)
        if st is None or (old_boot is not None and st.get("boot_count") == old_boot):
            continue
        if st.get("fw") == version and st.get("ota_state") == "valid":
            return "updated"
        if st.get("fw") != version:
            return "failed: rolled back" if st.get("rolled_back") else "failed: old image booted"
    return "failed: not valid in time"


def update_one(ip: str, data: bytes, version: str, wait: float, poll: float = 5.0,
               force: bool = False) -> str:
    """Update one panel; return 'updated', 'skipped', or 'failed: <reason>'.

    force re-installs even when the panel already runs `version`: the
    canary does that once to prove the NEW image can itself take an update
    (a release whose update path is broken must never reach the wall)."""
    deadline = time.time() + wait
    st = probe(ip, 4.0)
    if st is None:
        return "failed: unreachable"
    if st.get("fw") == version and not force:
        if st.get("ota_state") == "valid":
            return "skipped"
        # Same image, still proving itself (e.g. an earlier run timed out):
        # wait for it rather than call it done; it may yet roll back.
        return wait_valid(ip, version, deadline, poll)
    old_boot = st.get("boot_count")
    while st is None or st.get("state") != "idle":  # never cut a refresh
        if time.time() > deadline:
            return "failed: never idle"
        time.sleep(1)
        st = probe(ip, 4.0)
    try:
        code, body = post_image(ip, data)
    except (OSError, http.client.HTTPException, ValueError) as e:
        return f"failed: upload {type(e).__name__}"
    if code != 202:
        return f"failed: upload {code} {body}"
    # Restart, 60 s flash grace window, network, then 60 s self-validation.
    return wait_valid(ip, version, deadline, poll, old_boot)


def cmd_update(args) -> int:
    data = Path(args.image).read_bytes()
    version = image_version(data)
    hosts = [str(h) for h in ipaddress.ip_network(args.network, strict=False).hosts()]
    with ThreadPoolExecutor(max_workers=64) as pool:
        panels = [p for p in pool.map(lambda ip: probe(ip, 4.0), hosts) if p]
    panels.sort(key=lambda p: p.get("name", ""))
    todo = [p for p in panels if not (p.get("fw") == version and p.get("ota_state") == "valid")]
    print(f"image {version}: {len(panels)} panel(s), {len(todo)} to update", file=sys.stderr)
    # Every panel gets a row; ones never reached say so.
    results = {p["_ip"]: (p.get("name", "?"), "skipped" if p not in todo else "not attempted")
               for p in panels}

    def run(p, force=False):
        r = update_one(p["_ip"], data, version, args.wait, args.poll, force)
        print(f"{p.get('name', '?'):<33} {p['_ip']:<16} {r}{' (self-update check)' if force else ''}",
              flush=True)
        results[p["_ip"]] = (p.get("name", "?"), r)
        return r

    if todo:
        canary = todo[0]
        # Install, then install again over itself: the second pass is served
        # by the new image, proving it can take the next update.
        ok = run(canary) == "updated" and run(canary, force=True) == "updated"
        if not ok:
            print("canary failed: stopping; the other panels were not touched", file=sys.stderr)
        else:
            with ThreadPoolExecutor(max_workers=args.batch) as pool:
                list(pool.map(run, todo[1:]))

    with open(args.report, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "ip", "result"])
        for ip, (name, r) in sorted(results.items(), key=lambda kv: kv[1][0]):
            w.writerow([name, ip, r])
    bad = sum(r not in ("updated", "skipped") for _, r in results.values())
    print(f"report: {args.report}; {bad} not updated", file=sys.stderr)
    return 1 if bad else 0


def cmd_status(args) -> int:
    code, st = request(args.host, "GET", "/api/v1/status")
    print(json.dumps(st, indent=1))
    return 0 if code == 200 else 1


def cmd_name(args) -> int:
    if not NAME_RE.match(args.name):
        print(f"invalid name {args.name!r}: 1-32 of a-z 0-9 '-', not starting/ending with '-'",
              file=sys.stderr)
        return 2
    code, body = request(args.host, "PUT", "/api/v1/config", {"name": args.name})
    print(json.dumps(body))
    return 0 if code == 200 else 1


def cmd_scan(args) -> int:
    hosts = [str(h) for h in ipaddress.ip_network(args.network, strict=False).hosts()]
    with ThreadPoolExecutor(max_workers=64) as pool:
        found = [p for p in pool.map(lambda ip: probe(ip, args.timeout), hosts) if p]
    found.sort(key=lambda p: p.get("name", ""))
    print(f"{'name':<33} {'hw_id':<13} {'ip':<16} {'fw':<14} state")
    for p in found:
        print(f"{p.get('name', '?'):<33} {p.get('hw_id', '?'):<13} {p['_ip']:<16} "
              f"{p.get('fw', '?'):<14} {p.get('state', '?')}")
    dupes = {p["name"] for p in found if sum(q.get("name") == p.get("name") for q in found) > 1}
    if dupes:
        print(f"DUPLICATE NAMES: {', '.join(sorted(dupes))}", file=sys.stderr)
    print(f"{len(found)} panel(s)", file=sys.stderr)
    return 1 if dupes else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status", help="print one panel's status")
    s.add_argument("host")
    s.set_defaults(fn=cmd_status)
    n = sub.add_parser("name", help="set a panel's wall-position name")
    n.add_argument("host")
    n.add_argument("name")
    n.set_defaults(fn=cmd_name)
    c = sub.add_parser("scan", help="list every panel in a network, e.g. 192.168.1.0/24")
    c.add_argument("network")
    # A panel is silent for ~1-3 s while it receives a frame (single httpd
    # task), so a short timeout would drop live panels during a soak.
    c.add_argument("--timeout", type=float, default=4.0)
    c.set_defaults(fn=cmd_scan)
    u = sub.add_parser("update", help="roll a firmware image out to every panel in a network")
    u.add_argument("network")
    u.add_argument("--image", required=True, help="app image, e.g. ff-eink-v0.2.0.bin")
    u.add_argument("--batch", type=int, default=5, help="panels updated at once after the canary")
    u.add_argument("--wait", type=float, default=360.0, help="seconds per panel, idle to valid")
    u.add_argument("--report", default="update-report.csv")
    u.add_argument("--poll", type=float, default=5.0, help=argparse.SUPPRESS)
    u.set_defaults(fn=cmd_update)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (OSError, http.client.HTTPException, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
