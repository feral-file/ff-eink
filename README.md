# ff-eink

Firmware and reference pusher for a networked 13.3" colour e-paper panel:
Good Display **GDEP133C02** (Spectra 6, 1200×1600) on the **ESP32-133C02-IL**
board (ESP32-S3-WROOM-1-N16R8). A laptop pushes pre-dithered frames over
wired Ethernet; the panel shows them.

Status: **spike** for feral-file/feral-file#3594. Compiles; not yet run on
hardware. Bench procedure and results: [docs/bench.md](docs/bench.md).

## Layout

| Path | What |
|---|---|
| `main/panel.c` | panel driver: official init sequence, GPIO chip-selects, DMA bounce buffer, capped BUSY waits, silent-panel check, deep sleep, power-cycle recovery |
| `main/frame.h` | frame format and pure helpers (host-tested) |
| `main/patterns.c` | generated test patterns for bring-up |
| `main/net_usb.c` | USB host + CDC-ECM NIC (`espressif/iot_usbh_ecm`) with DHCP |
| `main/api.c` | HTTP API, see [docs/api.md](docs/api.md) |
| `main/main.c` | boot counters, maintenance mode, wiring, link watchdog |
| `pusher/dither.py` | image → 960,000 B frame (crop, Floyd–Steinberg, pack) |
| `pusher/push.py` | scheduled pusher with per-miss cause attribution, CSV log |

## Network path

```
PoE switch/injector ─► "PoE to USB-C with Ethernet" adapter ─USB-C─► board
```

The board's only USB-C port runs as a **USB host** (Full Speed). The adapter
powers the board and is its NIC. The NIC must expose a CDC-ECM USB
configuration; Realtek RTL8152/8153 adapters do so as configuration 2, which
the firmware selects. Check an adapter before use: docs/bench.md §0.

**The panel API has no authentication. Put the panel on a dedicated VLAN; it
is mandatory, not optional.**

## Build

ESP-IDF **v5.5.6**, pinned.

```sh
. ~/esp/esp-idf-v5.5.6/export.sh
idf.py set-target esp32s3   # once
idf.py build
```

Component versions are locked in `dependencies.lock`.

## Flash, and the way back

In normal mode the USB-C port becomes a host port 15 s after boot, and the
board is then not a serial/JTAG device. Two ways to flash:

- **Grace window:** with the board on the computer's USB-C, press **Reset**
  and run `idf.py -p /dev/cu.usbmodem1101 flash` within 15 s.
- **Maintenance mode:** hold **SW2** and press **Reset**. USB-Serial-JTAG
  stays, no network starts, and logs appear on the same port.

Back up the factory firmware before the first flash (docs/bench.md §1).

In maintenance mode, **SW3** cycles the test patterns (chart → bars →
checker), so the panel can be checked without a network.

## Push frames

Python 3.9+ and Pillow ≥ 9.1.

```sh
python3 pusher/dither.py photos/*.jpg -o pusher/out --preview
python3 pusher/push.py --host <panel-ip> pusher/out/*.bin --count 1440 --chart-ends --log run.csv
```

## Tests

```sh
make -C test                   # C frame helpers and patterns (ASan/UBSan)
python3 -m pytest pusher/tests # dither/pack, miss classifier, pusher against a fake device
```

## Vendor guidance this spike deliberately tests against

Good Display's board specification (rev 2.0, p.23) recommends a refresh
interval of **at least 180 s**, a refresh at least every 24 h, and sleep or
power-off when idle. The spike runs at 60 s because the issue asks for it.
The firmware sleeps the panel (deep sleep) between frames.

## Licence and sources

MIT (see `LICENSE`).

- Panel register values and command order are facts from Good Display's
  example `GDEP133C02.c` (2026-04-20), which has no licence; no code is taken
  from it.
- `espressif/iot_usbh_ecm`, `iot_usbh_cdc`, `iot_eth`: Apache-2.0, fetched by
  the component manager.
- Ink colours in `pusher/dither.py` are measured numbers from
  Frans-Willem/epd-dither (AGPL-3.0); only the numbers are used, with
  attribution, no code.
- Method reference only, no code: mvs90/esphome-usb-ethernet (GPL-3.0).
