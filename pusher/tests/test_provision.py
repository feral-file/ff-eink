# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Feral File Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
import hashlib
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import provision  # noqa: E402


def test_parse_mac_takes_the_base_mac_line():
    out = "Chip is ESP32-S3 (QFN56) (revision v0.2)\nMAC: 44:b1:76:cb:0c:b4\nStaying in bootloader.\n"
    assert provision.parse_mac(out) == "44b176cb0cb4"
    with pytest.raises(RuntimeError):
        provision.parse_mac("BASE MAC: 44:b1:76:cb:0c:b4\n")


def test_signing_key_digest_reads_the_key_of_block_0():
    key = bytes(range(256)) * 3 + bytes(8)  # 776 bytes: n, e, r_inv, m'
    block = bytes([0xE7, 0x02, 0, 0]) + bytes(32) + key
    app = bytes(8192) + block + bytes(4096 - len(block))
    assert provision.signing_key_digest(app) == hashlib.sha256(key).hexdigest()
    with pytest.raises(ValueError):
        provision.signing_key_digest(bytes(8192))  # unsigned


def test_problems():
    good = {"fw": "v1.0.0", "ota_state": "valid", "usb": "0bda:8152", "panel_errors": 0, "boot_count": 3}
    row = {"fw": "v1.0.0", "boot_count": "3"}
    assert provision.problems(good, row) == []
    assert provision.problems(good, dict(row, boot_count="")) == []  # first check
    # a dev build of the same release elsewhere would match fw; a different one must not
    assert "fw v1.0.0, flashed v1.1.0" in provision.problems(good, dict(row, fw="v1.1.0"))[0]
    bad = dict(good, fw="v0.9", ota_state="pending", usb="0b95:1790", panel_errors=1, boot_count=4)
    found = " | ".join(provision.problems(bad, row))
    for word in ("fw v0.9", "ota_state pending", "adapter 0b95:1790", "panel_errors 1", "rebooted"):
        assert word in found


def test_parse_flash_args_of_a_release():
    text = ("--flash_mode dio --flash_freq 80m --flash_size keep\n0x0 bootloader/bootloader.bin\n"
            "0x20000 ff-eink.bin\n0x8000 partition_table/partition-table.bin\n0xf000 ota_data_initial.bin\n")
    regions = provision.parse_flash_args(text)
    assert set(regions) == provision.FLASH_OFFSETS and regions[0x20000] == "ff-eink.bin"


def test_ready_needs_a_chart_then_white_an_hour_later_in_one_run():
    t0, t30, t61, t62 = ("2026-01-01T00:00:00Z", "2026-01-01T00:30:00Z",
                         "2026-01-01T01:01:00Z", "2026-01-01T01:02:00Z")
    new = {"check": "", "first_ok_utc": "", "chart_utc": ""}
    assert provision.burn_in(new, True, t0, white=False) == (t0, t0, "no")
    run = {"check": "ok", "first_ok_utc": t0, "chart_utc": t0}
    assert provision.burn_in(run, True, t30, white=True) == (t0, t0, "no")  # too early
    assert provision.burn_in(run, True, t61, white=False) == (t0, t61, "no")  # chart: not white
    assert provision.burn_in(run, True, t61, white=True) == (t0, t0, "yes")
    # white checks alone never make a board ready: nobody saw the chart
    assert provision.burn_in({"check": "ok", "first_ok_utc": t0, "chart_utc": ""}, True, t61,
                             white=True) == (t0, "", "no")
    # a failed check ends the run, chart included
    assert provision.burn_in(run, False, t61, white=True) == ("", "", "no")
    assert provision.burn_in({"check": "rebooted", "first_ok_utc": "", "chart_utc": ""}, True,
                             t62, white=True) == (t62, "", "no")


def test_inventory_keeps_one_row_per_board(tmp_path):
    inv = provision.Inventory(tmp_path / "p.csv")
    inv.update("aa", fw="v1", flash="ok")
    inv.update("bb", fw="v1", flash="failed: x")
    inv.update("aa", check="ok")
    raw = (tmp_path / "p.csv").read_bytes()
    (tmp_path / "p.csv").write_bytes(b"\xef\xbb\xbf" + raw)  # a spreadsheet's UTF-8 BOM
    rows = inv.load()
    assert sorted(rows) == ["aa", "bb"]
    assert rows["aa"]["flash"] == "ok" and rows["aa"]["check"] == "ok"
    assert list(rows["aa"]) == provision.FIELDS


class FakePanel:
    """Shows each accepted frame at once; boot_count can be changed."""

    def __init__(self):
        self.st = {"name": "ffeink-cb0cb4", "hw_id": "44b176cb0cb4", "fw": "v1.0.0",
                   "ota_state": "valid", "usb": "0bda:8152", "panel_errors": 0,
                   "boot_count": 5, "brownouts": 1, "state": "idle", "last_seq": 0,
                   "last_refresh_ms": 26000}
        panel = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, code, obj):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                self._send(200, panel.st)

            def do_POST(self):
                n = int(self.headers["Content-Length"])
                assert len(self.rfile.read(n)) == 960000
                panel.st["last_seq"] = int(self.headers["X-Frame-Seq"])
                self._send(202, {"seq": panel.st["last_seq"]})

        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()


def test_check_one_against_a_fake_panel(tmp_path, monkeypatch):
    pytest.importorskip("PIL")
    fake = FakePanel()
    port = fake.srv.server_address[1]
    monkeypatch.setattr(provision, "HTTP_PORT", port)
    monkeypatch.setattr(provision.time, "sleep", lambda s: None)
    inv = provision.Inventory(tmp_path / "p.csv")
    inv.update("44b176cb0cb4", fw="v1.0.0", flash="ok")
    hw = "44b176cb0cb4"
    p = dict(fake.st, _ip="127.0.0.1")

    row, ok = provision.check_one(p, inv, inv.load()[hw], timeout=5)
    assert ok and row["check"] == "ok" and row["boot_count"] == "5" and row["ready"] == "no"
    assert row["first_ok_utc"] and row["chart_utc"] and row["refresh_ms"] == "26000"

    fake.st["boot_count"] = 6  # rebooted during burn-in
    p = dict(fake.st, _ip="127.0.0.1")
    row, ok = provision.check_one(p, inv, inv.load()[hw], timeout=5)
    assert not ok and "rebooted" in row["check"] and row["first_ok_utc"] == ""

    p = dict(fake.st, _ip="127.0.0.1")  # the last check before packing: white
    row, ok = provision.check_one(p, inv, inv.load()[hw], timeout=5, white=True)
    assert ok and row["check"] == "ok" and row["first_ok_utc"]
    assert row["chart_utc"] == "" and row["ready"] == "no"  # the reboot ended the chart's run
    fake.srv.shutdown()
