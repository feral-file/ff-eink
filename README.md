# ff-eink

Firmware and reference tools for a networked 13.3" colour e-paper panel: the Good Display **GDEP133C02** (E Ink Spectra 6, 1200×1600) on the **ESP32-133C02-IL** board (ESP32-S3).

Each panel is an independent network endpoint with a name for its position on the wall. It accepts a pre-dithered image over wired Ethernet (PoE through a USB-C adapter) and reports its status. A controller decides what each panel shows; each installation builds its own controller against the [panel API](docs/api.md).

It is built for multi-panel installations, such as a wall of fifty panels where each one is addressed by its position.

## Status

**Early: first hardware bring-up.**

| Area | State |
|---|---|
| Panel driver | Verified on hardware: gate G1 passed on 2026-11-10. |
| Network, OTA updates and the 24 h soak | Written. Testing waits for the PoE USB-C adapter. |
| Procedures and results | See [docs/bench.md](docs/bench.md). |

## Documentation

| If you want to… | Read |
|---|---|
| Drive panels from a controller: frame format, endpoints, status fields, errors | [docs/api.md](docs/api.md) |
| Turn images into frames, push them, provision or inventory panels | [docs/pusher.md](docs/pusher.md) |
| Update the panels' firmware on site (museum operators) | [docs/UPDATING.txt](docs/UPDATING.txt) |
| Know the board: pins, buttons, network path, vendor guidance, open questions | [docs/hardware.md](docs/hardware.md) |
| Build, sign, flash, recover a board, run tests, cut a release | [docs/development.md](docs/development.md) |
| Understand how firmware updates and signing work, and why | [docs/ota.md](docs/ota.md) |
| See hardware test procedures and results | [docs/bench.md](docs/bench.md) |

## Quick start

```sh
# Turn a photo into a frame and show it on a panel (Python 3.9+, Pillow ≥ 9.1)
python3 pusher/dither.py photo.jpg -o pusher/out --preview
python3 pusher/panelctl.py scan 192.168.1.0/24          # find the panel
python3 pusher/push.py --host <panel-ip> pusher/out/photo.bin --count 1

# Build the firmware (ESP-IDF v5.5.6; needs a signing key, see docs/development.md)
idf.py build
```

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) covers setting up, testing and opening a pull request. Coding agents start at [AGENTS.md](AGENTS.md).

| If you want to… | Start with |
|---|---|
| Set up ESP-IDF, build, flash a board, recover a board, run the tests | [docs/development.md](docs/development.md) |
| Understand the code layout | [docs/development.md#code-map](docs/development.md#code-map) |
| Change the API without breaking controllers | [docs/api.md](docs/api.md); changes are additive only |
| Touch the update or signing path | [docs/ota.md](docs/ota.md) |
| Test on real hardware | [docs/hardware.md](docs/hardware.md), then [docs/bench.md](docs/bench.md) |

## Security note

The panel API is plain HTTP with no authentication, so **panels must sit on a dedicated VLAN**.

- **Firmware updates** are the exception: the panels accept only images signed by this repository's release workflow.
- **Physical access** to a board still allows USB flashing.

## Licence and sources

MIT (see `LICENSE`).

- **Panel register values and command order** are facts taken from Good Display's example `GDEP133C02.c` (2026-04-20). That example has no licence, and none of its code is used.
- **`espressif/iot_usbh_ecm`, `iot_usbh_cdc` and `iot_eth`** are Apache-2.0 and are fetched by the component manager.
- **The ink colours in `pusher/dither.py`** are measured numbers from Frans-Willem/epd-dither (AGPL-3.0). Only the numbers are used, with attribution; none of its code is.
- **mvs90/esphome-usb-ethernet** (GPL-3.0) served as a reference for the method only; none of its code is used.
