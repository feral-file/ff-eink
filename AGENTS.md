# AGENTS.md: guide for coding agents

ff-eink is ESP-IDF C firmware and Python tools for a networked 13.3" Spectra 6 e-paper panel. The board is an ESP32-S3 (Good Display ESP32-133C02-IL). Each panel accepts frames over a plain-HTTP API and reports its status.

This file is navigation only. The facts live in `docs/`. Read the doc for your task before you change anything.

## Hard rules

1. **Hardware needs the human's permission.**
   - Do not flash, erase, `read_flash`, or open the serial port unless the human asks.
   - On macOS, opening the port or running esptool can reset the chip. A reset during a panel refresh cuts the waveform. Before touching the port, ask whether the panel is idle (not flashing).
   - Use `--after no_reset` with esptool.
   - Details: docs/development.md, "Flashing, and the way back".
2. **Never commit a key.** `*.pem` is gitignored.
   - Every build needs `signing-key.pem` (a dev key; see docs/development.md).
   - Never print, move or upload a release key.
3. **The API is additive only.** Controllers in the field depend on docs/api.md. You may add fields or endpoints. Never change the meaning of an existing field, status code, error string, or connection behaviour.
4. **Never move or shrink `nvs`** in `partitions.csv` (0x9000, 24 KB). It holds each panel's wall-position name, and the layout can only change over USB.
5. **Never cut a refresh short.** Restarts go through `restart_when_idle()` in `main/restart.c`, and the panel state machine guards every writer. Keep both properties.
6. **Code and docs change in the same commit.** When behaviour changes, update the doc that describes it (see the task table). README stays an introduction plus navigation; details belong in `docs/`.
7. **Open-source project.** Docs must make sense to an outside contributor:
   - no organisation-internal names, links, dates or local paths;
   - no code copied from GPL or AGPL sources, or from the unlicensed vendor example (README, "Licence and sources").

## Where to look

| Task | Read first | Code |
|---|---|---|
| HTTP API: endpoints, status fields, errors, miss causes | `docs/api.md` | `main/api.c` |
| Panel driving: init, SPI, BUSY, refresh, recovery | `docs/hardware.md` | `main/panel.c`, `main/frame.h` |
| Frame format, test patterns | `docs/api.md#frame-format` | `main/frame.h`, `main/patterns.c` |
| Pins, buttons, board facts, open hardware questions | `docs/hardware.md` | `main/panel.c`, `main/main.c` |
| Boot order, SW8 reset/maintenance, grace window, watchdog | `docs/development.md` | `main/main.c` |
| Network: USB host, CDC-ECM, DHCP, hostname | `docs/hardware.md#network-path` | `main/net_usb.c`, `main/net.h` |
| Panel name, hw_id, `/api/v1/config` | `docs/api.md` | `main/identity.c`, `main/name_rules.h` |
| OTA, rollback, signing, release keys | `docs/ota.md` | `main/ota.c`, `main/api.c` (`firmware_post`), `sdkconfig.defaults`, `partitions.csv` |
| Image → frame, pusher, provisioning, updater | `docs/pusher.md` | `pusher/dither.py`, `pusher/push.py`, `pusher/panelctl.py` |
| Operator update steps | `docs/UPDATING.txt` | `pusher/panelctl.py` (`update`) |
| CI, releases | `docs/development.md#ci-and-releases`, `docs/ota.md` | `.github/workflows/`, `keys/` |
| Hardware test procedures and results | `docs/bench.md` | — |

## Commands

```sh
. $IDF_PATH/export.sh                         # ESP-IDF v5.5.6
idf.py build                                  # normal image (needs signing-key.pem)
idf.py -B build-bench -D SDKCONFIG=build-bench/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" build   # bench image
make -C test                                  # C host tests (ASan/UBSan)
pytest -q pusher/tests                        # Python tests
```

**Done means:**
- both images build with **zero warnings**;
- both test suites pass;
- the docs are updated.

When `sdkconfig.defaults` or `partitions.csv` changes, delete `sdkconfig` and `build-bench/sdkconfig` so they are regenerated.

## Conventions

- **Comments.** Comment *why*: invariants, hazards, hardware facts. Cite the source of each fact (datasheet page, vendor manual section, bench measurement).
- **Pure helpers.** Logic that can run without hardware goes in pure headers (`frame.h`, `name_rules.h`) with host tests in `test/`.
- **Python tools.** `panelctl.py` and `push.py` use the standard library only. `dither.py` may use Pillow.
- **Prefer less.** Delete or simplify before adding options or abstractions.
- **Review.** Before calling a change done, have it reviewed by someone (or an agent) trying to break it, with concrete failure scenarios.
