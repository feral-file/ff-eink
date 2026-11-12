# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Feral File
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
import json
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import panelctl  # noqa: E402


def make_image(version: str, chip: int = 9, signed: bool = True) -> bytes:
    img = bytearray(8192)
    img[0] = 0xE9
    struct.pack_into("<H", img, 12, chip)
    struct.pack_into("<I", img, 32, 0xABCD5432)
    img[48:48 + len(version)] = version.encode()
    if signed:
        img[4096], img[4097] = 0xE7, 0x02
    return bytes(img)


def test_image_version_checks():
    assert panelctl.image_version(make_image("v0.2.0")) == "v0.2.0"
    with pytest.raises(ValueError):
        panelctl.image_version(make_image("v0.2.0", chip=0))  # ESP32, not S3
    bad = bytearray(make_image("x"))
    bad[0] = 0
    with pytest.raises(ValueError):
        panelctl.image_version(bytes(bad))
    with pytest.raises(ValueError):
        panelctl.image_version(b"\xe9" * 100)
    with pytest.raises(ValueError, match="not signed"):
        panelctl.image_version(make_image("v0.2.0", signed=False))


class FakePanel:
    """outcome: 'ok' boots the new image valid; 'rollback' reboots into the old one."""

    def __init__(self, outcome="ok"):
        self.s = {"name": "p1", "hw_id": "aa", "fw": "v0.1.0", "ota_state": "valid",
                  "rolled_back": False, "boot_count": 3, "state": "idle"}
        self.outcome, self.posts = outcome, 0
        dev = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, code, obj):
                b = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def do_GET(self):
                self._send(200, dict(dev.s))

            def do_POST(self):
                data = self.rfile.read(int(self.headers["Content-Length"]))
                dev.posts += 1
                ver = panelctl.image_version(data)
                self._send(202, {"version": ver})
                dev.s["boot_count"] += 1
                if dev.outcome == "ok":
                    dev.s["fw"] = ver
                else:
                    dev.s["rolled_back"] = True

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()


@pytest.fixture
def route(monkeypatch):
    """Send every panelctl connection to one fake panel's port."""
    real = panelctl.http.client.HTTPConnection
    holder = {}
    monkeypatch.setattr(panelctl.http.client, "HTTPConnection",
                        lambda host, _port, timeout: real(host, holder["port"], timeout=timeout))
    return holder


@pytest.mark.parametrize("outcome,expect", [("ok", "updated"), ("rollback", "failed: rolled back")])
def test_update_one(route, outcome, expect):
    dev = FakePanel(outcome)
    route["port"] = dev.http.server_address[1]
    img = make_image("v0.2.0")
    assert panelctl.update_one("127.0.0.1", img, "v0.2.0", wait=5, poll=0.05) == expect
    assert dev.posts == 1
    dev.http.shutdown()


def test_update_skips_same_version(route):
    dev = FakePanel()
    dev.s["fw"] = "v0.2.0"
    route["port"] = dev.http.server_address[1]
    assert panelctl.update_one("127.0.0.1", make_image("v0.2.0"), "v0.2.0", wait=5, poll=0.05) == "skipped"
    assert dev.posts == 0
    dev.http.shutdown()


def test_update_waits_for_idle(route):
    dev = FakePanel()
    dev.s["state"] = "refreshing"
    route["port"] = dev.http.server_address[1]
    threading.Timer(1.5, lambda: dev.s.update(state="idle")).start()
    assert panelctl.update_one("127.0.0.1", make_image("v0.2.0"), "v0.2.0", wait=10, poll=0.05) == "updated"
    dev.http.shutdown()


def test_pending_same_version_is_waited_on_not_skipped(route):
    dev = FakePanel()
    dev.s.update(fw="v0.2.0", ota_state="pending")
    route["port"] = dev.http.server_address[1]
    threading.Timer(0.5, lambda: dev.s.update(ota_state="valid")).start()
    assert panelctl.update_one("127.0.0.1", make_image("v0.2.0"), "v0.2.0", wait=5, poll=0.05) == "updated"
    assert dev.posts == 0
    dev.http.shutdown()


def test_force_reinstalls_same_version(route):
    dev = FakePanel()
    dev.s["fw"] = "v0.2.0"
    route["port"] = dev.http.server_address[1]
    assert panelctl.update_one("127.0.0.1", make_image("v0.2.0"), "v0.2.0", wait=5, poll=0.05,
                               force=True) == "updated"
    assert dev.posts == 1
    dev.http.shutdown()


def _run_update(route, tmp_path, dev):
    route["port"] = dev.http.server_address[1]
    img = tmp_path / "img.bin"
    img.write_bytes(make_image("v0.2.0"))
    report = tmp_path / "r.csv"
    rc = panelctl.main(["update", "127.0.0.1/32", "--image", str(img), "--report", str(report),
                        "--wait", "5", "--poll", "0.05"])
    return rc, report.read_text().splitlines()[1:]


def test_cmd_update_canary_installs_twice(route, tmp_path):
    dev = FakePanel()
    rc, rows = _run_update(route, tmp_path, dev)
    assert rc == 0 and dev.posts == 2  # install + self-update check
    assert rows == ["p1,127.0.0.1,updated"]
    dev.http.shutdown()


def test_cmd_update_canary_failure_exits_nonzero(route, tmp_path):
    dev = FakePanel("rollback")
    rc, rows = _run_update(route, tmp_path, dev)
    assert rc == 1 and dev.posts == 1
    assert rows == ["p1,127.0.0.1,failed: rolled back"]
    dev.http.shutdown()
