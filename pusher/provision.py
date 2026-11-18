#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Feral File Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Provision new ff-eink boards in bulk (docs/provisioning.md).

    python3 provision.py flash --zip ff-eink-v1.0.0-full.zip
    python3 provision.py check 10.20.0.0/24
    python3 provision.py check 10.20.0.0/24 --white

`flash` watches for boards plugged in over USB (one at a time or many
through a hub; no panel attached) and gives each the release: it reads the
chip MAC (the board's hw_id), erases nvs, writes the release's -full.zip
and records the board in the inventory. It refuses a zip whose app is not
signed with the key in keys/release-key.sha256. Needs esptool
(`pip install "esptool>=4.8,<5"`).

`check` finds the inventory's boards on the network, checks firmware, OTA
state, adapter and panel, and shows the colour chart with the panel's own
name on it. `check --white` does the same but leaves the panel white, the
state the vendor says a panel should be stored in. A board is ready to ship
after a passing chart check and a passing white check at least an hour
later, with no failed check or reboot in between. Needs Pillow for the chart.

Both write to the same inventory CSV (default provision.csv), one row per
board, keyed by hw_id. Panels on the network that `flash` did not record
are reported and left alone.
"""
from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import ipaddress
import re
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

import panelctl
from push import Device

REPO = Path(__file__).resolve().parents[1]
RELEASE_KEY_DIGEST = REPO / "keys" / "release-key.sha256"

# macOS and Linux names of the ESP32-S3 USB-Serial/JTAG port.
PORT_GLOBS = ("/dev/cu.usbmodem*", "/dev/ttyACM*")

# What a release -full.zip writes (release.yml, partitions.csv): bootloader,
# partition table, otadata, and the app in ota_0.
FLASH_OFFSETS = {0x0, 0x8000, 0xF000, 0x20000}
APP_OFFSET = 0x20000

# partitions.csv: nvs at 0x9000, 24 KB. Erased so no factory data is left
# in it and the boot counters start from the flash.
NVS = ("0x9000", "0x6000")

EXPECTED_USB = "0bda:8152"  # the RTL8152 PoE adapter (docs/bench.md)
BURN_IN_S = 3600
HTTP_PORT = 80  # tests point this at a fake panel

FIELDS = ["hw_id", "fw", "flashed_utc", "flash", "name", "ip", "checked_utc", "check",
          "refresh_ms", "boot_count", "brownouts", "first_ok_utc", "chart_utc", "ready", "notes"]


def now_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_utc(s: str) -> float:
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()


# --- inventory -------------------------------------------------------------

class Inventory:
    """provision.csv, one row per hw_id; rewritten whole on every change.
    One process at a time: the lock covers this process's threads only."""

    def __init__(self, path: Path):
        self.path, self.lock = path, threading.Lock()

    def load(self) -> dict[str, dict]:
        if not self.path.exists():
            return {}
        # utf-8-sig: a spreadsheet's "CSV UTF-8" export starts with a BOM.
        with self.path.open(newline="", encoding="utf-8-sig") as f:
            return {r["hw_id"]: r for r in csv.DictReader(f)}

    def update(self, hw_id: str, **fields) -> dict:
        with self.lock:
            rows = self.load()
            row = rows.setdefault(hw_id, {k: "" for k in FIELDS} | {"hw_id": hw_id})
            row.update({k: str(v) for k, v in fields.items()})
            tmp = self.path.with_suffix(".tmp")
            with tmp.open("w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, FIELDS, extrasaction="ignore")
                w.writeheader()
                w.writerows(sorted(rows.values(), key=lambda r: r["hw_id"]))
            tmp.replace(self.path)
            return row


# --- flash -----------------------------------------------------------------

def signing_key_digest(app: bytes) -> str:
    """SHA-256 of the public key in the image's first Secure Boot v2 signature
    block: the digest `espsecure.py signature_info_v2` prints and
    keys/release-key.sha256 pins. The signature sector is the last 4 KB;
    the key (n, e, r_inv, m') sits at bytes 36..812 of the block. Only the
    key is compared: it decides which images the board accepts later."""
    block = app[-4096:]
    if len(app) < 4096 or block[0] != 0xE7 or block[1] != 0x02:
        raise ValueError("no Secure Boot v2 signature block")
    return hashlib.sha256(block[36:812]).hexdigest()


def parse_flash_args(text: str) -> dict[int, str]:
    """flash_args: a line of options, then `<offset> <file>` per region."""
    regions = {}
    for line in text.splitlines()[1:]:
        if line.strip():
            off, name = line.split()
            regions[int(off, 0)] = name
    return regions


def open_release(zip_path: Path, want_digest: str) -> tuple[Path, str]:
    """Unpack a release -full.zip; return (dir holding flash_args, version)."""
    out = Path(tempfile.mkdtemp(prefix="ff-eink-provision-"))
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(out)
    args = list(out.glob("*/flash_args")) + list(out.glob("flash_args"))
    if len(args) != 1:
        raise SystemExit(f"{zip_path}: expected one flash_args, found {len(args)}")
    root = args[0].parent
    regions = parse_flash_args(args[0].read_text())
    if set(regions) != FLASH_OFFSETS:
        raise SystemExit(f"{zip_path}: flash_args writes {sorted(hex(o) for o in regions)},"
                         f" expected {sorted(hex(o) for o in FLASH_OFFSETS)}")
    app = (root / regions[APP_OFFSET]).read_bytes()
    version = panelctl.image_version(app)
    digest = signing_key_digest(app)
    if digest != want_digest:
        raise SystemExit(f"{zip_path}: signed with key {digest}, not the release key {want_digest}."
                         " Shipping boards only take a release -full.zip (docs/ota.md).")
    return root, version


def esptool(port: str, *cmd: str, cwd: Path | None = None) -> str:
    # --after no_reset: the board stays in its bootloader until it is
    # unplugged, so nothing starts that the unplug could cut.
    argv = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "-b", "460800",
            "--before", "default_reset", "--after", "no_reset", *cmd]
    r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["?"]
        raise RuntimeError(f"esptool {cmd[0]}: {tail[0]}")
    return r.stdout


def parse_mac(out: str) -> str:
    m = re.search(r"^MAC: ((?:[0-9a-f]{2}:){5}[0-9a-f]{2})$", out, re.M)
    if not m:
        raise RuntimeError("esptool read_mac: no MAC in output")
    return m.group(1).replace(":", "")


def flash_one(port: str, root: Path, version: str, inv: Inventory) -> None:
    # No wait before esptool resets the chip: the board has no panel
    # attached at this station, so no refresh can be cut (AGENTS.md rule 5).
    say = lambda msg: print(f"{time.strftime('%H:%M:%S')} {port}: {msg}", flush=True)
    try:
        hw_id = parse_mac(esptool(port, "read_mac"))
    except Exception as e:  # noqa: BLE001 - report and move on to the next board
        say(f"FAILED before identifying the board: {e}. Unplug it and plug it in again.\a")
        return
    say(f"hw_id {hw_id}: erasing nvs, writing {version}")
    try:
        esptool(port, "erase_region", *NVS)
        esptool(port, "write_flash", "@flash_args", cwd=root)  # verifies each region's hash
    except Exception as e:  # noqa: BLE001
        inv.update(hw_id, fw=version, flashed_utc=now_utc(), flash=f"failed: {e}", ready="no")
        say(f"FAILED: {e}. Unplug it and plug it in again to retry.\a")
        return
    inv.update(hw_id, fw=version, flashed_utc=now_utc(), flash="ok", check="", first_ok_utc="",
               chart_utc="", boot_count="", brownouts="", ready="no")
    say(f"OK {hw_id} ({version}). Unplug it.\a")


def cmd_flash(args) -> int:
    want = RELEASE_KEY_DIGEST.read_text().strip().replace(" ", "")
    root, version = open_release(Path(args.zip), want)
    inv = Inventory(Path(args.inventory))
    print(f"release {version}, signed with the release key. Plug boards in; Ctrl-C to stop.")
    busy: set[str] = set()
    known = set(p for g in PORT_GLOBS for p in glob.glob(g))
    if known:
        print(f"ignoring ports already present: {', '.join(sorted(known))}")
    try:
        while True:
            ports = set(p for g in PORT_GLOBS for p in glob.glob(g))
            known &= ports  # an unplugged port may come back as a new board
            for port in sorted(ports - known - busy):
                busy.add(port)

                def run(port=port):
                    try:
                        flash_one(port, root, version, inv)
                    finally:
                        # Forget the port only once it is gone, so one board
                        # is flashed once per plug-in.
                        while glob.glob(port):
                            time.sleep(1)
                        busy.discard(port)

                threading.Thread(target=run, daemon=True).start()
            time.sleep(1)
    except KeyboardInterrupt:
        if busy:
            print(f"\nstopped with boards still plugged in: {', '.join(sorted(busy))}."
                  " If one had not said OK, its flash was cut: flash it again.")
        print(f"\nstopped; inventory in {args.inventory}")
        return 0


# --- check -----------------------------------------------------------------

def problems(st: dict, row: dict) -> list[str]:
    """What is wrong with a recorded board's status, before a frame is pushed."""
    out = []
    if st.get("fw") != row["fw"]:
        out.append(f"fw {st.get('fw')}, flashed {row['fw']}")
    if st.get("ota_state") != "valid":
        out.append(f"ota_state {st.get('ota_state')}")
    if st.get("usb") != EXPECTED_USB:
        out.append(f"adapter {st.get('usb')}, expected {EXPECTED_USB}")
    if st.get("panel_errors"):
        out.append(f"panel_errors {st.get('panel_errors')}: {st.get('last_error')}")
    if row.get("boot_count") and str(st.get("boot_count")) != row["boot_count"]:
        out.append(f"rebooted since the last check (boot_count {row['boot_count']} -> "
                   f"{st.get('boot_count')}, reset_reason {st.get('reset_reason')})")
    return out


def burn_in(row: dict, ok: bool, t: str, white: bool) -> tuple[str, str, str]:
    """(first_ok_utc, chart_utc, ready) after a check at time t.

    A run is an unbroken series of passing checks; any failed check (a
    reboot counts as one) ends it. Ready: within one run, a chart check
    (a person looked at the chart) and, at least BURN_IN_S after the run
    began, a white check (the panel is left white for storage)."""
    if not ok:
        return "", "", "no"
    same_run = row.get("check") == "ok"
    first = (row.get("first_ok_utc") if same_run else "") or t
    chart = (row.get("chart_utc") if same_run else "") or ""
    if not white:
        chart = t
    ready = white and bool(chart) and parse_utc(t) - parse_utc(first) >= BURN_IN_S
    return first, chart, "yes" if ready else "no"


def show_frame(ip: str, frame: bytes, timeout: float) -> tuple[bool, str, int]:
    """Push one frame; wait until the panel reports it shown."""
    dev, seq = Device(ip, HTTP_PORT), int(time.time())
    deadline = time.monotonic() + timeout
    posted = False
    while time.monotonic() < deadline:
        st = dev.status()
        if st and posted and st.get("last_seq") == seq and st.get("state") == "idle":
            return True, "", st.get("last_refresh_ms", 0)
        if st and not posted and st.get("state") == "idle":
            code, body = dev.post_frame(frame, seq)
            if code == 202:
                posted = True
            elif code != 429:  # 429: busy, try again
                return False, f"frame POST {code}: {body.strip()}", 0
        time.sleep(2)
    return False, "frame not shown in time" if posted else "never idle", 0


def check_one(p: dict, inv: Inventory, row: dict, timeout: float,
              white: bool = False) -> tuple[dict, bool]:
    hw_id, ip, name = p["hw_id"], p["_ip"], p.get("name", "")
    found = problems(p, row)
    if white:
        frame = bytes([0x11]) * 960000  # every pixel white (docs/api.md, frame format)
    else:
        import dither  # Pillow

        frame = dither.chart(name)
    shown, why, refresh = show_frame(ip, frame, timeout)
    if not shown:
        found.append(why)
    st = Device(ip, HTTP_PORT).status() or p
    ok = not found
    t = now_utc()
    first, chart, ready = burn_in(row, ok, t, white)
    new = inv.update(hw_id, name=name, ip=ip, checked_utc=t, check="ok" if ok else "; ".join(found),
                     refresh_ms=refresh, boot_count=st.get("boot_count", ""),
                     brownouts=st.get("brownouts", ""), first_ok_utc=first, chart_utc=chart,
                     ready=ready)
    return new, ok


def cmd_check(args) -> int:
    inv = Inventory(Path(args.inventory))
    rows = inv.load()
    hosts = [str(h) for h in ipaddress.ip_network(args.network, strict=False).hosts()]
    with ThreadPoolExecutor(max_workers=64) as pool:
        found = [p for p in pool.map(lambda ip: panelctl.probe(ip, 4.0), hosts) if p]
    ours = [p for p in found if rows.get(p["hw_id"], {}).get("flash") == "ok"]
    strangers = [p for p in found if p not in ours]
    what = "white" if args.white else "the labelled chart"
    print(f"{len(found)} panel(s) found, {len(ours)} flashed by this inventory; showing {what} on those")
    with ThreadPoolExecutor(max_workers=max(1, len(ours))) as pool:
        results = list(pool.map(
            lambda p: check_one(p, inv, rows[p["hw_id"]], args.timeout, args.white), ours))
    print(f"{'name':<33} {'hw_id':<13} {'ip':<16} {'ready':<6} check")
    for row, _ in sorted(results, key=lambda r: r[0]["name"]):
        print(f"{row['name']:<33} {row['hw_id']:<13} {row['ip']:<16} {row['ready']:<6} {row['check']}")
    for p in strangers:
        print(f"NOT CHECKED, not flashed by this inventory (no frame pushed): {p.get('name')}"
              f" {p['hw_id']} {p['_ip']} fw {p.get('fw')}")
    seen = {p["hw_id"] for p in found}
    missing = sorted(h for h, r in inv.load().items()
                     if r.get("flash") == "ok" and r.get("ready") != "yes" and h not in seen)
    if missing:
        print(f"flashed, not ready, not on this network: {len(missing)} ({', '.join(missing)})")
    dupes = {r["name"] for r, _ in results if sum(q["name"] == r["name"] for q, _ in results) > 1}
    if dupes:
        print(f"DUPLICATE NAMES: {', '.join(sorted(dupes))}", file=sys.stderr)
    if not args.white:
        print("Now look at every panel: the chart must be complete and clean, with the name"
              " matching its label. Write anything wrong in that board's notes column.")
    ok = results and all(r_ok for _, r_ok in results) and not strangers and not dupes
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inventory", default="provision.csv", help="CSV, one row per board")
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("flash", help="flash every board plugged in over USB")
    f.add_argument("--zip", required=True, help="a release ff-eink-vX.Y.Z-full.zip")
    f.set_defaults(func=cmd_flash)
    c = sub.add_parser("check", help="check the inventory's panels and show the labelled chart")
    c.add_argument("network", help="e.g. 10.20.0.0/24")
    c.add_argument("--timeout", type=float, default=180.0, help="seconds per panel")
    c.add_argument("--white", action="store_true",
                   help="show white instead of the chart: the last check before packing")
    c.set_defaults(func=cmd_check)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
