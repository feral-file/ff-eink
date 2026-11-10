# Development

Building, signing, flashing, testing and releasing ff-eink.

## Toolchain

- **ESP-IDF v5.5.6**, pinned. Install it as Espressif documents, then activate it in each shell with `. $IDF_PATH/export.sh` (e.g. `. ~/esp/esp-idf-v5.5.6/export.sh`).
- Component versions are locked in `dependencies.lock`:
  - `espressif/iot_usbh_ecm` 0.4.0
  - `espressif/iot_usbh_cdc` 3.1.1
  - `espressif/iot_eth` 1.1.0
- Host tests need a C compiler, Python 3.9 or newer, `pytest`, and `Pillow` ≥ 9.1.

## Signing key

Every image is signed, the bench build included ([ota.md](ota.md#signing-and-keys)). A build needs `signing-key.pem` in the project root. It is gitignored.

```sh
espsecure.py generate_signing_key --version 2 --scheme rsa3072 signing-key.pem   # once: your dev key
```

- A board accepts OTA updates only when they are signed with the key of the image it last received over USB.
- Lab boards use a dev key.
- Anything that ships is USB-provisioned only from a release `-full.zip`, which is signed with the release key in CI.

## Build

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3      # once
idf.py build                   # normal build -> build/ff-eink.bin (signed)
idf.py -B build-bench -D SDKCONFIG=build-bench/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" build   # bench build
```

| Build | What it does |
|---|---|
| **Normal** | Waits through a 60 s flash grace window, then starts USB host and the network, then serves the [API](api.md). |
| **Bench** (`CONFIG_FF_EINK_BENCH_PATTERNS`) | Never starts USB host or the network. Each boot shows the next test pattern: chart → bars → checker; press SW8, or replug, to step. If a valid frame is stored at flash `0x620000`, it shows that instead ([bench.md](bench.md#showing-an-image-without-a-network-bench-build)). |

**Flash layout** (`partitions.csv`):
- `nvs` sits at 0x9000 and is 24 KB. It **never moves**, because it holds the panel's name.
- Two 3 MB OTA slots.

## Flashing, and the way back

Back up a board's factory firmware before its first flash ([bench.md §1](bench.md#1-back-up-the-factory-firmware-once-before-the-first-flash)).

**The first flash** onto a board, or after a layout change, is a full USB flash:
- Run `idf.py -p /dev/cu.usbmodem1101 flash`.
- It writes the bootloader, the partition table, `otadata` and the app.

**Getting back to a flashable board.** The normal build turns the only USB-C into a USB **host** port 60 s after boot. From then on the Mac sees no serial port, and esptool cannot reach the board. There are two ways back:

- **Maintenance mode.**
  - Hold **SW8** while plugging the board into the computer, and keep holding for at least 1 s.
  - The board starts no USB host and no network.
  - Flash and monitor at any time.
- **Grace window.**
  - Build first, replug the board, then within 60 s run esptool directly (command below).
  - Do not use `idf.py flash` here: it rebuilds before it connects and can miss the window.
  - This works even if SW8 were broken.

```sh
idf.py build
cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args
```

**Bench pitfalls** (learned on 2026-11-10):

- On macOS, opening the port with the DTR/RTS lines preset can reset the chip, and so can every esptool run. A reset cuts any refresh that is in progress. Only touch the port while the panel is idle.
- Use `--after no_reset` for reads, backups and verifies, so the factory or bench app does not start a refresh right afterwards. A write that should boot the new image uses `--after hard_reset`: run it only while the panel is idle.
- A plain serial open does not reset the chip. To keep the logs flowing, keep one logger open rather than reopening the port.

## Tests

```sh
make -C test                 # C: frame helpers, patterns, name rules (ASan/UBSan)
pytest -q pusher/tests       # Python: dither/pack, miss classifier, pusher, panelctl, updater
```

Hardware procedures and their results are in [bench.md](bench.md).

## CI and releases

| Workflow | Trigger | Does |
|---|---|---|
| `.github/workflows/ci.yml` | every branch push (not `v*` tags) and every pull request | Builds the normal and bench images, signed with a throwaway key, and runs both test suites |
| `.github/workflows/release.yml` | a `v*` tag, after a maintainer approves the `release` environment | Builds and signs with the release key, checks the key digest against `keys/release-key.sha256`, then publishes a GitHub Release |

Each release carries:
- `ff-eink-<tag>.bin`: the app, used for OTA;
- `ff-eink-<tag>-full.zip`: everything for USB provisioning (unzip, `cd` into it, then `python -m esptool --chip esp32s3 -p <port> -b 460800 --before default_reset --after hard_reset write_flash @flash_args` in maintenance mode or within the grace window);
- `panelctl.py`, `dither.py`, `push.py`;
- [UPDATING.txt](UPDATING.txt);
- `SHA256SUMS`.

[ota.md](ota.md#open) lists the one-time GitHub setup still to do: the environment, the release key, and branch protection.

## Code map

| Path | What |
|---|---|
| `main/main.c` | Boot order, NVS counters, SW8 reset key and maintenance mode, flash grace window, link watchdog |
| `main/panel.c` | Panel driver: official init sequence, GPIO chip selects, DMA bounce buffer, capped BUSY waits, silent-panel check, deep sleep, recovery |
| `main/frame.h`, `main/patterns.c` | Frame format and pure helpers (host-tested); test patterns |
| `main/net_usb.c` | USB host + CDC-ECM NIC with DHCP; hostname |
| `main/api.c` | HTTP API ([api.md](api.md)) |
| `main/identity.c`, `main/name_rules.h` | Panel name and hardware id |
| `main/ota.c`, `main/restart.c` | Post-update validation and rollback; restarts that wait for the panel to be idle |
| `pusher/` | Reference and operator tools ([pusher.md](pusher.md)) |
| `test/` | Host C tests |
