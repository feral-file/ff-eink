# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Feral File
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from push import Evidence, classify, ready  # noqa: E402

BASE = {"boot_count": 5, "uptime_s": 1000, "link_downs": 0, "panel_errors": 0, "state": "idle", "last_seq": 1}


def st(**kw):
    return {**BASE, **kw}


def test_pull_wins_over_everything():
    ev = Evidence(pulled=True, before=st(), after=[st(boot_count=6)], http_status=None)
    assert classify(ev) == "deliberate_pull"


def test_reboot_by_boot_count_or_uptime():
    assert classify(Evidence(before=st(), after=[st(boot_count=6, link_downs=1)])) == "reboot"
    assert classify(Evidence(before=st(), after=[st(uptime_s=10)])) == "reboot"


def test_link_down_before_panel_error():
    ev = Evidence(before=st(), after=[st(link_downs=1, panel_errors=1)], accepted=True, http_status=202)
    assert classify(ev) == "link_down"


def test_panel_error():
    ev = Evidence(before=st(), after=[st(panel_errors=1)], accepted=True, http_status=202)
    assert classify(ev) == "panel_error"


def test_slow_refresh():
    still = Evidence(seq=7, before=st(), after=[st(state="refreshing")], accepted=True, http_status=202)
    late = Evidence(seq=7, before=st(), after=[st(state="refreshing"), st(last_seq=7)], accepted=True, http_status=202)
    busy = Evidence(seq=7, before=st(state="refreshing"), after=[st()], busy_refreshing=True, http_status=429)
    assert classify(still) == classify(late) == classify(busy) == "slow_refresh"


def test_http_error():
    assert classify(Evidence(before=st(), after=[st()], http_status=None)) == "http_error"
    assert classify(Evidence(before=st(), after=[st()], http_status=422)) == "http_error"
    assert classify(Evidence(before=st(), after=[st()], http_status=429)) == "http_error"


def test_no_device_evidence_is_not_a_reboot():
    # Unreachable throughout: no before/after, POST failed.
    assert classify(Evidence(before=None, after=[], http_status=None)) == "http_error"


def test_accepted_then_idle_without_showing_is_unattributed():
    ev = Evidence(seq=7, before=st(), after=[st(last_seq=1)], accepted=True, http_status=202)
    assert classify(ev) == "unattributed"


def test_not_ready_while_still_refreshing():
    ev = Evidence(seq=7, before=st(), after=[st(state="refreshing")], accepted=True, http_status=202)
    assert not ready(ev)
    ev.after.append(st(state="idle", panel_errors=1))  # drf_timeout counted after the tick
    assert ready(ev) and classify(ev) == "panel_error"


def test_ready_on_reboot_or_pull_without_idle():
    assert ready(Evidence(before=st(), after=[st(boot_count=6, state="refreshing")]))
    assert ready(Evidence(pulled=True))
    assert not ready(Evidence(before=st(), after=[]))
