import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import panelctl  # noqa: E402


def test_name_rule_matches_firmware():
    ok = ["a", "lobby-r03-c07", "ffeink-cb0cb4", "0", "abcdefghijklmnopqrstuvwxyz012345"]
    bad = ["", "-a", "a-", "Lobby", "a_b", "a.b", "a b", "é", "abcdefghijklmnopqrstuvwxyz0123456"]
    assert all(panelctl.NAME_RE.match(s) for s in ok)
    assert not any(panelctl.NAME_RE.match(s) for s in bad)


def test_name_and_probe_against_fake_panel(monkeypatch, capsys):
    state = {"name": "ffeink-cb0cb4", "hw_id": "44b176cb0cb4"}

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
            self._send(200, {**state, "fw": "x", "state": "idle"})

        def do_PUT(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            state["name"] = body["name"]
            self._send(200, state)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    real = panelctl.http.client.HTTPConnection
    monkeypatch.setattr(panelctl.http.client, "HTTPConnection",
                        lambda host, _port, timeout: real(host, port, timeout=timeout))
    assert panelctl.main(["name", "127.0.0.1", "lobby-r03-c07"]) == 0
    assert state["name"] == "lobby-r03-c07"
    assert panelctl.main(["name", "127.0.0.1", "Bad_Name"]) == 2
    p = panelctl.probe("127.0.0.1", 1.0)
    assert p["name"] == "lobby-r03-c07" and p["_ip"] == "127.0.0.1"
    srv.shutdown()
