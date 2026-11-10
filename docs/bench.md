# Bench procedures and results

Hardware test procedures for one board, and the results recorded on the
reference bench unit. The end goal they build up to: a laptop pushes a
pre-dithered 1200×1600 frame over the network, and it shows, sustained once
a minute for 24 h.

## Topology

```
router ─Cat5e─► PoE injector (802.3af/at) ─Cat5e─► "PoE to USB-C" adapter ─USB-C─► board
Mac (pusher) ─Wi-Fi─► same router
```

The adapter supplies the board's 5 V and a USB NIC on one cable. The board's
ESP32-S3 runs its native USB as host (Full Speed, 12 Mbit/s).

## 0. Adapter check, before the board is involved

Connect injector → adapter → **Mac**, then:

```sh
system_profiler SPUSBDataType          # vendor/product ID
ioreg -p IOUSB -l -w0 | grep -E 'idVendor|idProduct|bNumConfigurations|bInterfaceClass|bInterfaceSubClass'
```

| Reading | Verdict |
|---|---|
| `0bda:8152` | ideal (the prior-art chip) |
| `0bda:8153` | likely fine |
| ASIX `0b95`, SR9900, DM9601 | use the fallback adapter |
| `bNumConfigurations` ≥ 2 | needed: the firmware selects configuration 2 (CDC-ECM) |

macOS shows only the active configuration's interfaces; class 2 / subclass 6
(ECM) in configuration 2 is to be confirmed on the board (`usb_configs` and
`link_up` in the status).

| Date | Adapter | VID:PID | bcdDevice | Configurations | Notes |
|---|---|---|---|---|---|
| | | | | | |

## 1. Back up the factory firmware (once, before the first flash)

```sh
esptool.py -p /dev/cu.usbmodem1101 --after no_reset read_flash 0 0x1000000 factory-<board-mac>.bin
esptool.py -p /dev/cu.usbmodem1101 --before no_reset --after no_reset verify_flash 0 factory-<board-mac>.bin
```

Restore: `esptool.py -p /dev/cu.usbmodem1101 write_flash 0 <that file>`.

## 2. Buttons and the way back to flashing

Buttons, pins and their measurement: [hardware.md](hardware.md#buttons).
Flashing, maintenance mode (hold SW8 ≥ 1 s while plugging in) and the 60 s
grace window: [development.md](development.md#flashing-and-the-way-back).

## G1: panel from our driver, USB power only

Board on the Mac's USB-C (power and serial). Flash the bench build:

```sh
idf.py -B build-bench -D SDKCONFIG=build-bench/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" -p /dev/cu.usbmodem1101 flash monitor
```

Each boot shows the next pattern, so **press SW8** (or replug) to step:
chart → bars → checker → chart. SW8 waits for the refresh to finish before
restarting. A replug does not: before replugging, wait for the log line
`frame seq=0 shown` (about 26 s at room temperature, up to about 47 s cold).
Photograph each pattern.

| Check | Expected |
|---|---|
| Chart corners | black top-left, red top-right, blue bottom-left, empty bottom-right |
| Chart bars, left to right | black, white, yellow, red, blue, green; red begins exactly at the centre seam |
| Border | 2 px black on all four sides |
| Log | `refresh` ≈ 26 s at room temperature (datasheet: 12 s typical), no `failed` |

If the chart is mirrored, rotated or its halves swapped, record which, then
fix it in the firmware's send path (`main/panel.c`), so the wire format in
docs/api.md stays as specified.

| Date | Pattern | Result | refresh_ms |
|---|---|---|---|
| 2026-11-10 | chart | pass: corners, bar order, centre seam all correct; no mirror, halves not swapped | 26023 |
| 2026-11-10 | bars | pass | 26020 |
| 2026-11-10 | checker | pass: diagonals continuous across the seam | 25986 |

Orientation: the frame's top edge is the edge **opposite the FPC**; view the
panel portrait with the FPC at the bottom. The wire format needs no change.
Reference unit: bench build at `f031a53` (plus a temporary GPIO watch, since removed; see [hardware.md](hardware.md#buttons)); the
factory image was backed up and a restore drill passed first.

Before moving on, flash the normal build once on the Mac cable and check the
way back: `idf.py -p /dev/cu.usbmodem1101 flash monitor`; the log shows
`USB host starts in 60 s`. Wait until the port disappears, then check both
ways back: (1) replug and run the esptool line within 60 s; (2) hold SW8
while replugging, confirm `maintenance mode` in the log and that
`idf.py -p /dev/cu.usbmodem1101 flash` succeeds. Also replug three times
without touching SW8, on the Mac and later on the PoE adapter: the boot line
`SW8 (GPIO12) at boot: 1 ... -> normal` must appear every time (a false
maintenance entry would leave the device offline).

## Layout change (once per board)

The two-slot layout (docs/ota.md) replaces the bootloader, partition table
and `otadata`, so the first flash after it is a full flash over USB
(`idf.py flash` or `write_flash @flash_args`, which include all four
regions). `nvs` at 0x9000 is untouched. The image must be signed: a dev-key
image on lab boards, a release `-full.zip` on anything that ships.

### Results: reference unit, 2026-11-10

Signed bench build (dev key) at `1f331b8`, full USB flash over the earlier
single-app layout, with `--after no_reset` so nothing ran until the logger
booted it.

| Check | Result |
|---|---|
| Boot from the new layout | pass: partition table `nvs`/`otadata`/`ota_0`/`ota_1`; booted `ota_0` |
| Signed image accepted at startup | pass: the app ran (an unsigned app aborts) |
| `nvs` preserved across the reflash | pass: `boot_count` continued, name default kept |
| Chart, bars, checker | pass, refresh 26015 / 26111 / 26111 ms, no errors; again 25982 ms after a later restart |
| SW8 press steps the pattern | pass, twice: restart ~0.13 s after release, `reset_reason: SW`, `restart_cause: reset_key`; next boot `SW8 (GPIO12) at boot: 1 -> normal` |
| Maintenance mode | pass: SW8 held while plugging in → `SW8 (GPIO12) at boot: 0 … -> maintenance`, no refresh; esptool then connected (`flash_id`) |
| Grace window with the normal build | not yet run: needs the normal build, scheduled with network bring-up |

Notes:

- `brownouts` read 1 before this flash. It most likely comes from an
  earlier cable replug: a plug-in can bounce the supply. The soak's pass
  criterion is therefore "`brownouts` unchanged during the run", not zero.
- A refresh takes about 26 s on every frame, not only after a reset. That
  is the panel's own waveform (about 1.2 s of each frame is ours: wake,
  init, data). Expect about 28 s per pushed frame including upload.

## Network bring-up

Normal build, board on the adapter. Find the DHCP lease on the router,
then:

```sh
python3 pusher/panelctl.py scan <subnet>/24     # finds the panel: name ffeink-cb0cb4
python3 pusher/panelctl.py name <ip> bench-01   # status and DHCP hostname follow
curl -s http://<ip>/api/v1/status | python3 -m json.tool
python3 pusher/dither.py --chart -o pusher/out
python3 pusher/push.py --host <ip> --count 1 --chart-ends --log bringup.csv
python3 pusher/push.py --host <ip> pusher/out/*.bin --count 10 --log bringup.csv
```

| Date | DHCP | usb / configs | upload_ms | 10 in a row | Notes |
|---|---|---|---|---|---|
| | | | | | |

## OTA checks (before G6, so the soak runs the shipping image)

Board on the adapter, normal build, dev key. Build a second image with a
different version (e.g. an empty commit, or `git tag v0.0.0-test`), then:

| # | Do | Expect |
|---|---|---|
| 1 | `panelctl.py update --image build/ff-eink.bin <subnet>` | 202, restart, `ota_state` pending then `valid`, `fw` = new version |
| 2 | POST an image signed with a **different** key (`curl --data-binary @other.bin http://<ip>/api/v1/firmware`; panelctl only checks that a signature exists) | `422 image`, no restart, `fw` unchanged |
| 3 | POST a truncated image with `curl` (`head -c 300000 build/ff-eink.bin > cut.bin`; panelctl would refuse it locally) | `400 body` or `422 image`, `fw` unchanged |
| 4 | Pull power during an upload | boots the old image, `fw` unchanged |
| 5 | Update to an image that never brings the network up (e.g. `net_start` disabled) | ~11 min after the restart: back on the old image, `rolled_back: true` |
| 6 | Run `update` again with the same image | all panels `skipped` |
| 7 | A→B→A: update to B, then back to A, both through `update` | both succeed: the new image can take the next update (the canary's self-update automates this per release) |
| 8 | During checks 1, 2 and 5, log `uxTaskGetStackHighWaterMark` for the httpd and `ota_valid` tasks (temporary code) | margin left of the 10 KB and 8 KB stacks |

| Date | # | Result | Notes |
|---|---|---|---|
| | | | |

## G6: 24 h at 60 s

```sh
python3 pusher/dither.py photos/* -o pusher/out
python3 pusher/push.py --host <ip> pusher/out/*.bin --count 1440 --chart-ends \
    --log g6.csv --pull-file PULL
```

- Photograph the first and last frame (both the chart) under the same light,
  for ghosting.
- During staffed hours, do one **link pull** and one **power pull**: `touch
  PULL` first, pull, restore, wait until frames show again, then `rm PULL`.
  The link pull is the **router → injector** cable (the board stays powered;
  restore within 10 min or the link watchdog restarts it). The power pull is
  the **injector → adapter** cable, which carries PoE.
- Pass: no `unattributed` or `no_evidence` misses, `panel_errors == 0`, no `BROWNOUT`
  (`brownouts` unchanged).

| Run | Start (UTC) | Pushed | Shown | Missed by cause | panel_errors | brownouts | Refresh ms min/median/max |
|---|---|---|---|---|---|---|---|
| | | | | | | | |

## What the kit needs that it doesn't have

Record findings here as they come up.

-
