# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Feral File
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""push.py main loop against a fake device that follows docs/api.md."""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dither  # noqa: E402
import push  # noqa: E402


class FakeDevice:
    """Refreshes in `refresh_s`; frame number `fail_nth` (1-based) hits a panel error."""

    def __init__(self, refresh_s=0.1, fail_nth=None):
        self.s = {"boot_count": 1, "uptime_s": 0, "state": "idle", "last_seq": 0,
                  "panel_errors": 0, "link_downs": 0, "last_refresh_ms": 100}
        self.refresh_s, self.fail_nth, self.posts = refresh_s, fail_nth, 0
        self.lock = threading.Lock()
        dev = self

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
                with dev.lock:
                    self._send(200, dict(dev.s))

            def do_POST(self):
                n = int(self.headers["Content-Length"])
                body = self.rfile.read(n)
                with dev.lock:
                    if n != dither.FRAME_BYTES:
                        return self._send(400, {"error": "length"})
                    if dev.s["state"] != "idle":
                        return self._send(429, {"error": "busy"})
                    dev.posts += 1
                    seq = int(self.headers["X-Frame-Seq"])
                    fail = dev.posts == dev.fail_nth
                    dev.s["state"] = "refreshing"
                threading.Timer(dev.refresh_s, dev.finish, (seq, fail)).start()
                assert len(body) == n
                self._send(202, {"seq": seq})

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()

    def finish(self, seq, fail):
        with self.lock:
            if fail:
                self.s["panel_errors"] += 1
            else:
                self.s["last_seq"] = seq
            self.s["state"] = "idle"

    @property
    def port(self):
        return self.http.server_address[1]


def run_push(tmp_path, dev, count, extra=()):
    log = tmp_path / "run.csv"
    rc = push.main(["--host", "127.0.0.1", "--port", str(dev.port), "--interval", "0.6",
                    "--poll", "0.05", "--count", str(count), "--chart-ends", "--log", str(log), *extra])
    rows = log.read_text().splitlines()[1:]
    return rc, [r.split(",") for r in rows]


def test_all_shown(tmp_path):
    dev = FakeDevice()
    rc, rows = run_push(tmp_path, dev, 3)
    dev.http.shutdown()
    assert rc == 0
    assert [r[5] for r in rows] == ["1", "1", "1"]


def test_panel_error_is_attributed(tmp_path):
    dev = FakeDevice(fail_nth=2)
    rc, rows = run_push(tmp_path, dev, 3)
    dev.http.shutdown()
    assert rc == 0
    causes = {r[0]: r[8] for r in rows}
    assert causes["1"] == "panel_error"
    assert causes["0"] == causes["2"] == ""


def test_overrun_is_slow_refresh(tmp_path):
    dev = FakeDevice(refresh_s=0.9)  # longer than the 0.6 s interval
    rc, rows = run_push(tmp_path, dev, 2)
    time.sleep(1)
    dev.http.shutdown()
    assert rc == 0
    assert {r[8] for r in rows} == {"slow_refresh"}


def test_never_reachable_device_is_no_evidence(tmp_path):
    rc = push.main(["--host", "127.0.0.1", "--port", "9", "--interval", "0.3", "--poll", "0.05",
                    "--count", "1", "--chart-ends", "--log", str(tmp_path / "x.csv")])
    rows = (tmp_path / "x.csv").read_text().splitlines()[1:]
    assert rc == 1 and rows[0].split(",")[8] == "no_evidence"
