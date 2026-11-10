# ff-eink HTTP API, v1

Plain HTTP on port 80. There is no authentication: the panel must sit on a
dedicated VLAN, and that VLAN is the security boundary. A signed `v2` can be
added beside `v1` later.

## Frame format

| Property | Value |
|---|---|
| Size | exactly **960,000 bytes**: 1600 rows × 600 bytes, row-major, top row first |
| Pixels | 1200 × 1600 portrait, 4 bits each, two per byte; the **even-x pixel is the high nibble** |
| Colours | `0x0` black, `0x1` white, `0x2` yellow, `0x3` red, `0x5` blue, `0x6` green |
| Invalid | `0x4`, `0x7`, `0x8`–`0xF` in either nibble: the frame is refused (422) |

The device does not dither or scale. `pusher/dither.py` produces this format
from any image.

Inside the device, bytes 0–299 of each row go to the panel's master
controller (left 600 px) and bytes 300–599 to the slave (right 600 px). The
API does not expose that split.

## `POST /api/v1/frame`

Request:

- `Content-Length: 960000`, exact. Chunked bodies are refused.
- `X-Frame-Seq: <uint32>`, decimal. The pusher's sequence number for this
  frame. It is echoed back and reported as `last_seq` once shown. Use values
  that are unique per run (the reference pusher starts at Unix time).
- Body: the frame.

The device validates the frame, replies at once, and then refreshes the panel
in the background, which takes about 26 s at room temperature and up to about 47 s cold.

| Status | Body | Meaning | Connection |
|---|---|---|---|
| `202` | `{"seq":N}` | accepted; refresh started | kept |
| `400` | `{"error":"length"}` | `Content-Length` is not 960,000 | **closed** (the client may see a reset instead of the 400) |
| `400` | `{"error":"seq"}` | `X-Frame-Seq` missing or not a uint32 | kept (body discarded) |
| `400` | `{"error":"body"}` | body incomplete within 10 s | **closed** |
| `422` | `{"error":"pixels"}` | a nibble is not a colour | kept |
| `429` | `{"error":"busy"}` | another frame is uploading or refreshing | kept (body discarded) |

`202` means accepted, not shown. A frame is **shown** when `GET
/api/v1/status` reports `state == "idle"` and `last_seq == N`.

Single-flight: the device holds one frame. While `state` is not `idle`, every
POST gets 429. Nothing is queued.

The HTTP server is a single task. While a frame (about 1–2 s at USB Full
Speed, estimate) or a firmware image uploads, other requests, status
included, wait.

**Connections.** At most 4 sockets are kept; the least recently used one is
closed when a fifth arrives. Unknown paths return 404, wrong methods 405.
Every reply is `application/json` with `Cache-Control: no-store`.

## `GET /api/v1/status`

The response is built from cached counters and never waits on the panel.

| Field | Meaning |
|---|---|
| `name` | the panel's wall-position name (see `/api/v1/config`); also its DHCP hostname |
| `hw_id` | the chip's factory MAC, 12 hex; never changes for a board |
| `fw` | firmware version (`git describe` of the build; the release tag) |
| `ota_state` | `valid`, or `pending` while a freshly updated image proves itself (docs/ota.md); `unknown` only for a moment at start-up |
| `rolled_back` | `true` if an updated image failed to prove itself (crash, restart, power loss or no IP before it was marked valid) and the panel went back to the previous image; read once at boot, stays `true` until the next successful update. Also `true` (harmlessly) after an upload that was interrupted |
| `uptime_s` | seconds since boot |
| `boot_count` | boots since NVS was last erased (survives reflashing and updates) |
| `reset_reason` | this boot's reset reason: `POWERON`, `SW`, `PANIC`, `TASK_WDT`, `BROWNOUT`, … |
| `restart_cause` | why the firmware restarted itself before this boot: `link_watchdog`, `panel_init`, `reset_key` (SW8), `ota`, or `""`. An OTA rollback and a failed panel SPI re-init restart without a cause (`""`, `reset_reason: SW`) |
| `brownouts` | brownout resets since NVS was last erased |
| `state` | `idle`, `receiving` or `refreshing` |
| `last_seq` | seq of the last frame shown successfully; `0` until a frame has been shown this boot |
| `accepted_seq` | seq of the last frame accepted |
| `frames_accepted`, `frames_rejected`, `frames_shown` | counters since boot |
| `panel_errors` | failed refreshes since boot |
| `last_error`, `last_error_seq` | last panel failure (see below) and the seq it hit |
| `last_upload_ms`, `last_refresh_ms`, `last_frame_ms` | last frame upload; last DRF-to-done time; last reset-to-sleep time |
| `last_shown_age_s` | seconds since a frame was last shown; `-1` if none this boot |
| `link_up`, `ip`, `mac` | NIC link, DHCP address, the NIC's own MAC (zeros while the link is down) |
| `usb`, `usb_configs` | last USB device seen (`vid:pid`) and its configuration count |
| `link_downs`, `ip_acquired` | link-down transitions and DHCP leases since boot |
| `heap_internal_free`, `heap_internal_min`, `psram_free` | memory, bytes |

`last_error` values:

| Value | Meaning |
|---|---|
| `reset_busy_timeout` | BUSY did not release within 2 s of a reset |
| `pon_timeout`, `pof_timeout` | power on/off did not finish within 30 s |
| `drf_busy_never_asserted` | silent panel: BUSY did not go low within 1 s of DRF |
| `drf_too_fast` | silent panel: the refresh "finished" in under 5 s |
| `drf_timeout` | the refresh did not finish within 120 s |
| `spi` | an SPI transfer failed |

After any panel error the device power-cycles the panel through IO45 (that
IO45 low actually removes panel power is unverified on this board; see
docs/bench.md §2). The next frame starts
from a hardware reset either way.

## `GET /api/v1/config`, `PUT /api/v1/config`

Provisioning. Today the only setting is the panel's **name**: its
wall-position label, e.g. `lobby-r03-c07`.

- Rules: 1–32 characters of `a–z`, `0–9`, `-`, not starting or ending with
  `-` (a lower-case DNS label, because it is also the DHCP hostname).
- Default until set: `ffeink-` + the last 6 hex of `hw_id`.
- Stored in NVS (0x9000, 24 KB, kept fixed by docs/ota.md): survives
  restarts and app updates. A full flash erase, or a partition-table change
  that moves or shrinks `nvs`, resets it.
- The DHCP hostname follows on the next DHCP request (lease renewal or
  reconnect); the status shows the new name at once.

`PUT` body: exactly `{"name":"lobby-r03-c07"}`. Any other key is refused, so
a typo cannot look like success.

| Status | Body | Meaning |
|---|---|---|
| `200` | `{"name":"…","hw_id":"…"}` | saved (also the `GET` response) |
| `400` | `{"error":"length"}` | body empty or over 128 bytes (connection closed) |
| `400` | `{"error":"body"}` | not a JSON object with exactly one string `name`; or the body broke off (then the connection is closed) |
| `422` | `{"error":"name"}` | the name breaks the rules above |
| `500` | `{"error":"storage"}` | NVS write failed; nothing changed |

Swapping a spare in: give the spare the dead panel's name. `hw_id` is how
inventory and RMA tell the two physical units apart.

## `POST /api/v1/firmware`

Installs a signed app image (`ff-eink-<version>.bin` from a release) in the
inactive OTA slot. Use `pusher/panelctl.py update`, which also sequences the
rollout. Design: docs/ota.md.

- `Content-Length` exact, at most one slot (3 MB). Body: the image.
- Single-flight with frames: refused while a frame uploads or refreshes;
  frames are refused while it uploads (`state` shows `receiving`).
- The image must carry a valid signature from the key of the running image,
  and pass ESP-IDF's header, chip and SHA-256 checks.

| Status | Body | Meaning | Connection |
|---|---|---|---|
| `202` | `{"version":"…"}` | written; the panel restarts once idle (`restart_cause: ota`) | kept |
| `400` | `{"error":"length"}` | missing or too large | **closed** |
| `400` | `{"error":"body"}` | upload broke off, exceeded 60 s, or writing the slot failed | **closed** |
| `422` | `{"error":"image"}` | not an app image, or an image that fails the header, chip, SHA-256 or **signature** check; nothing changed | kept, or **closed** when refused at the first bytes |
| `429` | `{"error":"busy"}` | a frame is uploading or refreshing | **closed** |
| `500` | `{"error":"storage"}` | could not start writing the slot | **closed** |

On the "closed" rows the panel closes the connection without reading the
rest of the body, so a client still sending may see a connection reset
instead of the JSON body. Check `state == "idle"` before uploading (the
updater does).

After the restart the new image is `ota_state: pending` and becomes `valid`
after holding an IP continuously for 60 s with the API up. If it has no IP
10 min after the network starts (~11 min after boot), or if it crashes or
loses power first, the panel returns to the previous
image (`rolled_back: true`).

## Device-side recovery

| Condition | Action |
|---|---|
| No IP for 10 min | restart once the panel is idle (`restart_cause: link_watchdog`) |
| Panel init fails at boot | restart after 10 s (`restart_cause: panel_init`) |
| SW8 pressed | restart once the panel is idle (`restart_cause: reset_key`) |
| Firmware accepted | restart once the panel is idle (`restart_cause: ota`) |
| Updated image has no IP 10 min after the network starts (~11 min after boot), or crashes/restarts before it is valid | back to the previous image (`rolled_back: true`) |
| Panel error | power-cycle the panel through IO45 (power removal unverified) |
| Panel SPI bus cannot be re-initialised after an error | immediate restart (`restart_cause: ""`) |

## Misses

The pusher sends one frame per tick, 60 s by default. A frame not shown
before the tick ends (interval − 2 s at the default 60 s) is a **miss**. Every miss gets exactly
one cause, and the first rule that matches wins:

| Order | Cause | Evidence |
|---|---|---|
| 1 | `deliberate_pull` | the operator's pull file existed during the tick |
| 2 | `reboot` | `boot_count` changed or `uptime_s` went backwards |
| 3 | `link_down` | `link_downs` increased |
| 4 | `panel_error` | `panel_errors` increased |
| 5 | `slow_refresh` | accepted, and later shown late or still refreshing at the deadline; or refused 429 because the previous frame was still refreshing |
| 6 | `http_error` | the POST got no response, or a status other than 202 |
| 7 | `unattributed` | accepted, then idle without showing it and with no counter change |
| — | `no_evidence` | the run ended before the device answered again (not a guess) |

A miss is classified only once the device has answered with its refresh
concluded (`state == "idle"`) or with a new `boot_count`; until then it is
held, so a panel error counted after the tick (e.g. `drf_timeout` at 120 s)
is still attributed. Reboot rows carry the new boot's `reset_reason` and
`restart_cause`. The 24 h pass criteria allow no
`unattributed` or `no_evidence` miss, no `panel_errors` and no `BROWNOUT`.
