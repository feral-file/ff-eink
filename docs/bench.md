# Bench procedure and results

Spike goal (feral-file/feral-file#3594): a laptop pushes a pre-dithered
1200×1600 frame over the network to the Good Display kit, and it shows,
sustained once a minute for 24 h.

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
(ECM) in configuration 2 is confirmed on the board (`usb_configs` and
`link_up` in the status).

| Date | Adapter | VID:PID | bcdDevice | Configurations | Notes |
|---|---|---|---|---|---|
| | | | | | |

## 1. Back up the factory firmware (once, with approval)

```sh
esptool.py -p /dev/cu.usbmodem1101 read_flash 0 0x1000000 ~/esp/work/backup/factory-44b176cb0cb4.bin
```

Restore: `esptool.py -p /dev/cu.usbmodem1101 write_flash 0 <that file>`.

## 2. Buttons and the way back to flashing

Source for buttons and pins: Good Display's **Chinese** manual
《ESP32-133C02-IL 驱动开发板说明书》 §3.1–3.6 (good-display.cn), confirmed on the
bench board. The English specification rev 2.0 §3.1 ("SW2–SW4 on IO12–14,
triggered by a high level") is wrong.

| Button | Wiring | What it does with this firmware |
|---|---|---|
| **SW8** "复位按键" (reset key) | GPIO12, low while pressed. **Not** the chip's EN pin | press: restart on release, once the panel is idle; held ≥ 1 s while powering up: **maintenance mode** |
| **SW7** "电源按键" (power key) | IP5306 battery boost key (manual: IO13) | nothing on USB power |

There is no BOOT button, and no button reaches EN, so a **hard** restart is
unplugging and replugging the USB-C cable at the board (USB is its only
power source). Never replug while the panel is refreshing.

Measured 2026-11-10 (passive USB re-enumeration watch plus a GPIO watch in
the bench build): SW8 drove GPIO12 1→0→1 on each press and never reset the
chip, even held 10 s; SW7 changed no watched GPIO and did not reset it.

Open pin questions for Good Display (the Chinese manual conflicts with
other sources; nothing here blocks the spike):

- **IO45**: manual §3.4 says power LED, low = on; the 2026-04-20 example
  drives it as the panel load switch (TPS22913B). Bench: IO45 high and the
  panel works. **Unverified: that IO45 low removes panel power.** If it does
  not, the firmware's post-error "power-cycle" is only a reset plus re-init
  (harmless, but not a power cycle). Check once: panel VCI with IO45 low.
- **IO48**: manual §3.6 says "LDO EN, high = enable", without naming the
  rail. This firmware leaves IO48 an input; board and panel work.
- **IO12**: manual lists it both as the reset key and as IP5306 I2C SCL
  (SDA IO11). This firmware never talks to the IP5306.

- **Bench build** (G1): never starts USB host, so flashing works at any time.
- **Normal build**: the USB-C port turns into a USB host port 60 s after each
  boot, and the port disappears from the Mac. Two ways back:
  - **Maintenance mode:** hold SW8 while plugging in USB-C, for at least
    1 s. The log shows `maintenance mode (SW8 held at boot)`; flash at leisure.
  - **Grace window:** build first, replug, and run the esptool line within
    60 s. Do not use `idf.py flash` here: it rebuilds before connecting and
    can miss the window.

```sh
idf.py build                       # build FIRST: idf.py flash would rebuild inside the window
cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args
```

## G1: panel from our driver, USB power only

Board on the Mac's USB-C (power and serial). Flash the bench build:

```sh
idf.py -B build-bench -D SDKCONFIG=build-bench/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" -p /dev/cu.usbmodem1101 flash monitor
```

Each boot shows the next pattern, so **press SW8** (or replug) to step:
chart → bars → checker → chart. SW8 waits for the refresh to finish before
restarting. A replug does not: before replugging, wait for the log line
`frame seq=0 shown` (about 26 s at room temperature, up to ~50 s cold).
Photograph each pattern.

| Check | Expected |
|---|---|
| Chart corners | black top-left, red top-right, blue bottom-left, empty bottom-right |
| Chart bars, left to right | black, white, yellow, red, blue, green; red begins exactly at the centre seam |
| Border | 2 px black on all four sides |
| Log | `refresh` ≈ 12–20 s at room temperature, no `failed` |

If the chart is mirrored, rotated or its halves swapped, record which, then
fix it in the firmware's send path (`main/panel.c`), so the wire format in
docs/api.md stays as specified.

| Date | Pattern | Result | refresh_ms | Photo |
|---|---|---|---|---|
| 2026-11-10 | chart | pass: corners, bar order, centre seam all correct; no mirror, halves not swapped | 26023 | `~/esp/work/bench-logs/2026-11-10-g1-chart.jpg` |
| 2026-11-10 | bars | pass | 26020 | `…-g1-bars.jpg` |
| 2026-11-10 | checker | pass: diagonals continuous across the seam | 25986 | `…-g1-checker.jpg` |

Orientation: the frame's top edge is the edge **opposite the FPC**; view the
panel portrait with the FPC at the bottom. The wire format needs no change.
Bench build at ff-eink `f031a53` + pin watch; factory image backed up and a
restore drill passed first (`2026-11-10-g1-bench.log`, `…-efuse-summary.txt`).

Before moving on, flash the normal build once on the Mac cable and check the
way back: `idf.py -p /dev/cu.usbmodem1101 flash monitor`; the log shows
`USB host starts in 60 s`. Wait until the port disappears, then check both
ways back: (1) replug and run the esptool line within 60 s; (2) hold SW8
while replugging, confirm `maintenance mode` in the log and that
`idf.py -p /dev/cu.usbmodem1101 flash` succeeds. Also replug three times
without touching SW8, on the Mac and later on the PoE adapter: the boot line
`SW8 (GPIO12) at boot: 1 ... -> normal` must appear every time (a false
maintenance entry would leave the device offline).

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

Record here as found (for the result comment on #3594).

-
