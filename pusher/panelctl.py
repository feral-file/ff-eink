#!/usr/bin/env python3
"""Provision and inspect ff-eink panels on the panel VLAN. Standard library only.

    python3 panelctl.py status 192.168.1.50
    python3 panelctl.py name 192.168.1.50 lobby-r03-c07
    python3 panelctl.py scan 192.168.1.0/24

`scan` asks every address in the network for /api/v1/status and prints one
line per panel (name, hw_id, IP, firmware, state), sorted by name: the
inventory of the wall. A spare swapped in for a dead panel gets the dead
panel's name with `name`.
"""
from __future__ import annotations

import argparse
import http.client
import ipaddress
import json
import re
import sys
from concurrent.futures import ThreadPoolExecutor

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
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (OSError, http.client.HTTPException, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
