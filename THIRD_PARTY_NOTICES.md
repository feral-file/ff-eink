# Third-party notices

ff-eink's own code is MIT (see `LICENSE`). This file lists what ff-eink uses
from other projects, under which licence, and what a redistribution must
carry.

## Data in this repository

`pusher/dither.py` contains ink-colour palettes: numbers only, no code.

| Palette | Source | Licence of the source |
|---|---|---|
| `bpc80` | [Frans-Willem/epd-dither](https://github.com/Frans-Willem/epd-dither) `src/palette.rs` @ `9b461577` (`SPECTRA6_D65_BPC80_ADJUSTED`): a spectrophotometer measurement of a Spectra 6 panel | GPL-3.0 at `9b461577`; the repository has been AGPL-3.0-or-later since `eccead50`, with identical values. Only this one set of six measured colours is used, as data with attribution; no code. The project's other palettes are deliberately not copied. |
| `aitjcize` | [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe) `main/color_palette.c` @ `54d87a36` | MIT, notice below |
| `epdoptimize` | [paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize) `src/dither/data/default-palettes.json`, palette `spectra6`, @ `25f42661`; Copyright 2025 Robert Gühne | Apache-2.0 (the project ships no NOTICE file); values converted from hex to decimal |

### aitjcize/esp32-photoframe (MIT)

```
MIT License

Copyright (c) 2025 Wei-Ning Huang (AZ)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Code adapted from other projects

| File | Adapted from | Licence |
|---|---|---|
| `main/net_usb.c` | the USB host / CDC-ECM bring-up sequence and its parameters, from Espressif's [esp-iot-solution](https://github.com/espressif/esp-iot-solution) `examples/usb/host/usb_ecm_4g_module/main/usb_ecm_4g_module.c`, Copyright 2024-2026 Espressif Systems (Shanghai) CO LTD. Modified as stated in the file header. | Apache-2.0 ([LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); the project ships no NOTICE file |

## Facts used without code

- **Panel register values and command order** come from Good Display's example `GDEP133C02.c` (2026-04-20). That example has no licence, so only the facts are used; no code is taken from it.
- **mvs90/esphome-usb-ethernet** (GPL-3.0) was a reference for the method only. No code is taken from it.

## Firmware binaries

The firmware is built with **ESP-IDF v5.5.6** (Apache-2.0) and the Espressif components `iot_usbh_ecm`, `iot_usbh_cdc` and `iot_eth` (Apache-2.0, fetched by the component manager and not stored in this repository). ESP-IDF links third-party code under its own licences.

These are the licences that apply:

- `esp-idf-sbom license` reports these from the SPDX tags in the sources: Apache-2.0, BSD-3-Clause, BSD-2-Clause-FreeBSD, BSD-2-Clause-Views, MIT, CC0-1.0 and Unlicense.
- Some linked libraries have no SPDX tags and keep their notices in their own files. The tool does not report them, so they are listed here:
  - cJSON (MIT)
  - http_parser (MIT)
  - lwIP (BSD-3-Clause)
  - Mbed TLS (Apache-2.0 OR GPL-2.0-or-later; used under Apache-2.0)
  - newlib (a collection of BSD-style and similar notices)
- The GCC runtime libraries (libgcc, libstdc++) fall under the GCC Runtime Library Exception and need no notice.

Every release carries, generated from that release's build:

- `FIRMWARE_LICENSES.txt`: the tool's report, each licence with its copyright holders (`esp-idf-sbom license -u -p`).
- `licenses.zip`, which holds:
  - `LICENSES/`: the standard licence texts;
  - `LICENSES/upstream/`: the original licence files of cJSON, http_parser, lwIP, Mbed TLS and newlib, copied from ESP-IDF.
- `ff-eink-<tag>.spdx`: an SPDX bill of materials. It lists every component the build system knows, including some that are not linked into the image, so it over-reports. Use `FIRMWARE_LICENSES.txt` and `licenses.zip` for the notices.

Re-check this list when ESP-IDF or a component is upgraded: compare the linked archives in `build/ff-eink.map` with the files above.

The repository does not distribute Pillow or Python. Users install them.
