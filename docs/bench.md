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
| 2026-11-15 | P2C5V (802.3af, 5 V/2–2.4 A, 10/100, sold under several brands) | `0bda:8152` | `0x2000` | 2 | NIC directly on the root port, no hub. macOS picked configuration 2 itself: if0 class 2/6 (ECM), if1 class 10 (CDC data), bound by the built-in ECM driver. Listings say it needs a USB PD host; on the board (CC Rd only, no PD) it powers and enumerates anyway (see Network bring-up). |

The Mac readout cannot prove the board will work: the Mac speaks USB PD and
can swap data roles, the board cannot. Only `usb_configs` and `link_up` on
the board settle that.

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
| Grace window with the normal build | pass (2026-11-18): `provision.py flash` reached a board running the normal build within the 60 s window, without SW8 ([release candidate on hardware](#release-candidate-on-hardware)) |

Notes:

- `brownouts` read 1 before this flash. It most likely comes from an
  earlier cable replug: a plug-in can bounce the supply. The soak's pass
  criterion is therefore "`brownouts` unchanged during the run", not zero.
- A refresh takes about 26 s on every frame, not only after a reset. That
  is the panel's own waveform (about 1.2 s of each frame is ours: wake,
  init, data). Expect about 28 s per pushed frame including upload.

## Showing an image without a network (bench build)

The bench build shows a frame stored in unused flash at `0x620000` (the
first byte after `ota_1`) when one is there, and the test patterns
otherwise. Erased flash is `0xFF`, which is not a colour, so an empty region
falls back to the patterns. With the panel idle:

```sh
python3 pusher/dither.py photo.jpg -o pusher/out --preview      # check the preview first
cd build-bench && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
    --before default_reset --after no_reset write_flash \
    0xf000 ota_data_initial.bin 0x20000 ff-eink.bin 0x620000 ../pusher/out/photo.bin
```

Then replug (or press SW8). Rewriting `ota_data_initial.bin` makes the
board boot `ota_0` even if it took an OTA update earlier. To go back to the
patterns: `esptool.py … erase_region 0x620000 0xEB000`.

## Palette comparison

Same dark, high-contrast artwork (a 9:16 photograph, centre-cropped to 3:4),
shown with the bench build's flash-frame path, 2026-11-10:

| Variant | On the panel |
|---|---|
| `d65` | darker areas collapse into one flat dark-blue colour |
| `bpc80` | **best**: deep, clean blacks with detail in the shadows |
| `d65` + per-image tone mapping (CIELAB L* stretched onto the inks) | shadow detail kept, but noisy coloured dots in the dark areas; dropped |

A second round on a very light, low-contrast pastel artwork (same day):

| Variant | On the panel |
|---|---|
| `bpc80` | **best** |
| `aitjcize` palette only | lost light detail (its white is darker, so pale tones turn white) |
| `epdoptimize` palette | softer, lower contrast |
| aitjcize-style pipeline (S-curve + saturation ×1.2, approximated) | nearly blank: the curve pushes pale tones to white |

Result: `bpc80` is the default; `aitjcize` and `epdoptimize` ship as
options. `d65` (epd-dither's D65_ADJUSTED) is not shipped.
Refresh was 25.9 s for every variant.

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
| 2026-11-15 | lease on first boot; `scan` finds `ffeink-cb0cb4` | `0bda:8152` / 2 | 1220–1283 | not run: 3 frames (chart, then 2 photos at a 180 s interval), all shown, 0 misses | P2C5V adapter, normal build at `ac8271b`, `ota_state` `valid`, `link_downs` 0, `panel_errors` 0. Each frame shown in 29.2–29.3 s, refresh 25999–26047 ms. Boot `reset_reason` was `BROWNOUT` (`brownouts` 1 → 7 over a day of replugs); unchanged across all three refreshes and 8 min of uptime. |
| 2026-11-17 | lease on every boot of a network-enabled image during the OTA checks | `0bda:8152` / 2 | 1224 (POST 1.39–1.76 s on the laptop) | pass: 437 in a row in the G6 run at 180 s, 0 misses | Same adapter, `v0.1.0-rc1-4-gdf085a0`. See G6 for the run. |

The first attempt found nothing on the LAN. With no log once USB host is up,
a temporary build painted the panel by bring-up stage after `net_start`
(USB device seen, ECM link up, IP). It showed USB and ECM link up but no IP.
The adapter on the Mac then also got no lease: 10baseT, 0 packets received.
The cause was the injector's upstream cabling, not the board; after fixing
it the link came up at 100baseTX and the board got a lease.

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
| 2026-11-17 | 1 | pass | `ac8271b` → test image B (dev key, version `ota-test-b`) with `panelctl.py update`: both canary passes `updated`, 4 min 21 s in total, `ota_state` `valid`, `restart_cause` `ota`. |
| 2026-11-17 | 2 | pass | B re-signed with a throwaway key (`espsecure.py sign_data` on `ff-eink-unsigned.bin`): `422 {"error":"image"}`, no restart, `fw` unchanged. |
| 2026-11-17 | 3 | pass | First 300000 bytes of B, sent with an exact `Content-Length`: `422 {"error":"image"}`, no restart. |
| 2026-11-17 | 4 | pass | A sent slowly (over 55 s) so the cable could be pulled by hand. The injector → adapter cable was pulled at 30–40 %: the board booted B, `ota_state` `valid`, `rolled_back` false, `reset_reason` `BROWNOUT` (`brownouts` +1, from the pull). |
| 2026-11-17 | 5 | pass | Image C2 calls `esp_netif_init()` instead of `net_start()`, so lwIP and the HTTP server run but no IP ever arrives. It booted at 08:30, rolled back on its own at 08:41 (60 s grace + 10 min), back on B with `rolled_back` true, `reset_reason` `SW`. A first attempt, C, skipped `net_start()` entirely: without `esp_netif_init()` the HTTP server start panics, and the bootloader reverted to B 138 s after the upload (`reset_reason` `PANIC`, `rolled_back` true). That is a test-image artefact, since `net_start()` initialises the netif before anything that can fail, but it did exercise the crash-before-valid path. |
| 2026-11-17 | 6 | pass | `update` again with B: `skipped`, exit 0. |
| 2026-11-17 | 7 | pass | `ac8271b` → B (check 1), then B → A (`v0.1.0-rc1-4-gdf085a0`, a clean build) with `update`: both canary passes `updated` in 4 min 22 s; the board now runs A. Each canary's second pass also proves the new image takes the next update. |
| 2026-11-17 | 8 | pass | Image B reported `uxTaskGetStackHighWaterMark` in status (temporary code). httpd (10 KB): 5600 B free at the lowest, after the 422 uploads of checks 2 and 3, which run the full signature check. `ota_valid` (8 KB): 6960 B free when it marked the image valid. The rollback path of check 5 runs in the same task and could not be read over the network; it completed without a crash (`reset_reason` `SW`). |

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
- Pass: no `unattributed` or `no_evidence` misses, `panel_errors == 0`, and
  no `BROWNOUT` except the one the power pull causes (`brownouts` up by
  exactly 1 over the run).

| Run | Start (UTC) | Pushed | Shown | Missed by cause | panel_errors | brownouts | Refresh ms min/median/max |
|---|---|---|---|---|---|---|---|
| 1, 60 s | 2026-11-17 09:09 | 9 | 9 | none | 0 | 10 → 10 | 25860 / 25956 / 26054 |
| 2, 180 s | 2026-11-17 09:18 | 437 | 437 | none | 0 | 10 → 10 | 25742 / 25998 / 26036 |

Reference unit, `v0.1.0-rc1-4-gdf085a0` (dev key), five dithered artwork
photos cycled, chart first and last.

- Run 1 was stopped after 9 frames (panel idle) to switch to 180 s, the
  board manual's recommended minimum refresh interval.
- Run 2 was stopped after 21.8 h instead of 24 h, at 07:07 the next day, with
  the panel idle. A final chart was pushed by hand (`push.py --count 1
  --chart-ends`): shown, refresh 25385 ms.
- Across both runs: no reboot (`boot_count` 44, `uptime_s` 80580 at the
  end), `link_downs` 0, `frames_rejected` 0, `heap_internal_min` 258272
  at start and end. Each frame was shown 29.1–29.7 s after its tick.
- Ghosting: none visible on the last chart against the first, same light.
- The link pull and the power pull were not done in these runs. They were
  done afterwards on `v0.1.0-rc2`, outside a soak, and both recovered
  ([release candidate on hardware](#release-candidate-on-hardware)).
- So this is a pass for stability at 180 s. It is not a full G6 pass: 60 s
  over 24 h remains open.

## Release candidate on hardware

`v0.1.0-rc2`, the first release-key image on a board, run through
[provisioning](provisioning.md) on the reference unit on 2026-11-18.
`SHA256SUMS` matched, the zip's app carried the pinned release key and the
version `v0.1.0-rc2`.

| Step | Result |
|---|---|
| `provision.py flash`, board on the Mac, panel detached, running the normal build | read `hw_id` `44b176cb0cb4` (same as the firmware reports), erased `nvs`, wrote and verified: OK about 8 s after the board was found, within the 60 s grace window, without SW8 |
| Board on its panel and the PoE adapter | on the LAN about 70 s after power-up; `fw` `v0.1.0-rc2`, `ota_state` `valid`, `boot_count` 1 (`nvs` erased), `usb` `0bda:8152` |
| `provision.py check` | `ok`; chart shown with the name `ffeink-cb0cb4` in the white area, checked by eye; refresh 26060 ms |
| Link pull (router → injector, about 53 s) | no reboot (`boot_count` 1), `link_downs` 0 → 1, `ip_acquired` 1 → 2; the next `check` showed the chart, `ok` |
| `provision.py check --white` | `ok`, panel left white; `ready` `no` because the burn-in hour was skipped on this already soaked board |
| Power pull (injector → adapter, about 5 s, panel idle) | back on the LAN about 1 min later on `v0.1.0-rc2`, `ota_state` `valid`, `reset_reason` `BROWNOUT`; the next `check` showed the chart and failed with `rebooted since the last check (boot_count 1 -> 2)`, as designed |

The reference board now runs the release key: dev-key images reach it only
by USB.

### OTA between release images

With the `panelctl.py` and images from the `v0.1.0` release assets, the
way an operator would run it from the USB stick. `SHA256SUMS` matched for
all assets.

| Update | Result |
|---|---|
| `v0.1.0-rc2` → `v0.1.0` | first pass `updated` (`valid`); the self-update pass then reported `failed: unreachable` and the canary stopped. The board had not rebooted and stayed on `v0.1.0` `valid`. `panelctl` gave up on a single status probe that took longer than its 4 s timeout. |
| `v0.1.0` → `v0.1.0-rc2` | both passes `updated`, 4 min 22 s: `v0.1.0` takes an update |
| `v0.1.0-rc2` → `v0.1.0` | both passes `updated`, 4 min 51 s; the board ends on `v0.1.0`, `ota_state` `valid` |

**Slow replies.** For about 15 minutes after the first update, status
replies took 0.8–4.5 s and `/api/v1/config` over 10 s, while ping stayed at
about 6 ms with no loss. The headers arrived within 15 ms; the body came
seconds later. It cleared without a reboot, and outside restarts replies
then took about 20 ms again. The laptop was on Wi-Fi; the cause is not
known. Since then `panelctl` waits up to 10 s for a status reply and tries
three times before it calls a panel unreachable.

## What the kit needs that it doesn't have

Record findings here as they come up.

- A way to see bring-up progress without a log. A lit link LED on the
  adapter only proves the adapter-to-injector hop; check the network with
  the adapter on a Mac first (it must get a lease and receive packets).
  When the board still gets no IP, a temporary build that paints the panel
  by stage (no USB device / no ECM link / no IP / IP) narrows it down.
