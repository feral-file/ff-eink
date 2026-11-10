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

The board has only two buttons: **Reset** and **Power** (silkscreen SW7/SW8,
top-left in spec p.5). The function buttons SW2–SW4 in spec §3.1 are
reserved footprints, not fitted, and there is no BOOT button.

- **Bench build** (G1): never starts USB host, so flashing works at any time.
- **Normal build**: the USB-C port turns into a USB host port 60 s after each
  boot, and the port disappears from the Mac. To reflash: build first, then
  press **Reset** and run the esptool line within 60 s. This is the only way
  back; no button is needed. Do not use `idf.py flash` here: it rebuilds
  before connecting and can miss the window.

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

Each boot shows the next pattern, so **press Reset** to step:
chart → bars → checker → chart. Before each Reset, wait for the log line
`frame seq=0 shown` (about 20 s warm, up to ~50 s cold): a Reset during a
refresh cuts the panel's power mid-waveform. Photograph each pattern.

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
| | | | | |

Before moving on, flash the normal build once on the Mac cable and check the
way back: `idf.py -p /dev/cu.usbmodem1101 flash monitor`; the log shows
`USB host starts in 60 s`. Wait until the port disappears, then press Reset
and run the esptool line above within 60 s: it must succeed.

## Network bring-up

Normal build, board on the adapter. Find the DHCP lease on the router,
then:

```sh
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
