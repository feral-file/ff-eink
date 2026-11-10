# Hardware

What the firmware runs on, what is verified, and what is still open.

## Board and panel

| Part | Fact | Source |
|---|---|---|
| Panel | Good Display GDEP133C02: E Ink Spectra 6, 13.3", 1200×1600, bare 0.85 mm glass, 60-pin FPC | datasheet |
| Board | ESP32-133C02-IL: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM), one USB-C, SD slot, IP5306 battery charger | spec, bench (`esptool flash_id`) |
| USB-C | Native USB-Serial-JTAG (`303a:1001`); there is no UART bridge. Proper CC Rd, so a C-to-C cable powers the board. | bench |
| Panel controllers | Two ICs. Master takes the left 600 columns (CS IO18), slave the right 600 (CS IO17). | spec §4.5, example |
| Refresh | about 26 s at room temperature on the bench (datasheet: 12 s typical); slower when cold, up to about 47 s reported | bench G1, datasheet |
| Orientation | The frame's top edge is the edge **opposite the FPC**. View the panel portrait with the FPC at the bottom. | bench G1 |

The frame format is described in [api.md](api.md#frame-format) and in `main/frame.h`.

## Pins

**Source:** Good Display's **Chinese** manual 《ESP32-133C02-IL 驱动开发板说明书》 §3.1–3.6 (good-display.cn), checked on the bench. The English specification rev 2.0 §3.1 says "SW2–SW4 on IO12–14, triggered by a high level", and **that is wrong**.

| Function | Pin | Notes |
|---|---|---|
| Panel SCK / MOSI / MISO | IO9 / IO41 / IO40 | SPI3 at 10 MHz; the firmware does not use MISO |
| Panel CS_M / CS_S | IO18 / IO17 | GPIO chip selects, held low for each half |
| Panel RST / BUSY | IO6 / IO7 | BUSY is low while busy |
| Panel DC | IO2 | Listed, but unused: commands go as an 8-bit command phase |
| Panel load switch | IO45 | See the open questions below |
| SD card CS / MOSI / MISO / SCK | IO15 / IO3 / IO5 / IO8 | Unused |
| Reset key (SW8) | IO12, low when pressed | |
| Power key (SW7) | IO13 (manual) | Controls IP5306 boost |
| IP5306 I2C SDA / SCL | IO11 / IO12 | Unused |
| LDO EN | IO48, high enables | The manual does not name the rail; unused |
| Console UART TX / RX | IO43 / IO44 | No known pad |
| USB D− / D+ | IO19 / IO20 | |

## Buttons

| Button | Wiring | With this firmware |
|---|---|---|
| **SW8** 复位按键 (reset key) | GPIO12, low while pressed. **Not** the chip's EN pin. | A press restarts the board on release, once the panel is idle. Holding it for at least 1 s while powering up enters **maintenance mode**. |
| **SW7** 电源按键 (power key) | IP5306 battery boost key | Does nothing on USB power |

- There is no BOOT button, and no button reaches EN. A **hard** restart means unplugging and replugging USB-C at the board, which is its only power source.
- Never replug while the panel is refreshing.
- **Measured 2026-11-10,** with a passive watch for USB re-enumeration plus a GPIO watch:
  - SW8 drove GPIO12 1→0→1 on every press and never reset the chip, even when held for 10 s.
  - SW7 changed no GPIO that was being watched and never reset the chip.
- The board edge has the silkscreen labels SW7 and SW8. Both are side-actuated: push the small black plunger on the board edge toward the board.

## Network path

```
PoE switch/injector ─Cat5e/6─► "PoE to USB-C with Ethernet" adapter ─USB-C─► board
```

- **Data.**
  - The board's USB-C runs as a **USB host** at Full Speed (12 Mbit/s).
  - The adapter is both the power supply and the NIC.
  - A 960,000 B frame takes about 1–2 s to upload (estimate; to be measured at network bring-up).
- **NIC requirement.**
  - The NIC must expose a **CDC-ECM** USB configuration.
  - Realtek RTL8152/8153 (`0bda:8152` / `8153`) are expected to do this as configuration 2 (unverified on this board), and the firmware selects configuration 2 whenever a device has more than one.
  - Check any adapter before use: [bench.md §0](bench.md).
- **No Ethernet MAC on the board.** The earlier W5500-over-SPI plan was replaced by this route because it needed no soldering. The adapter needs 802.3af PoE.
- **Security.** The API is plain HTTP. **A dedicated VLAN is mandatory**; see [api.md](api.md).

## Vendor guidance

Board specification rev 2.0, p.23, and the Chinese manual §7:

| Guidance | What the firmware does |
|---|---|
| Refresh at intervals of at least 180 s | Not enforced: the interval is the controller's choice. The reference soak runs at 60 s to stress the system; choose the interval for an installation with this guidance in mind. |
| Refresh at least every 24 h | Left to the installation's controller |
| Never leave the panel powered while idle | After every frame: POF (power off), then deep sleep (`0x07 A5`) |
| Store the panel refreshed to white | Not automated |

## Open questions for Good Display

None of these blocks the spike.

**IO45.**
- The manual §3.4 calls IO45 the power LED (low = on). The 2026-04-20 example uses it as the TPS22913B panel load switch.
- On the bench, the panel works with IO45 high.
- **Unverified:** that IO45 low actually removes panel power. If it does not, the firmware's post-error "power-cycle" is only a reset plus re-initialisation. That is harmless, but it is not a power cycle.
- One check settles it: measure panel VCI with IO45 low.

**IO48.**
- The manual §3.6 says "LDO EN, high = enable", but does not say which rail it switches.
- The firmware leaves IO48 as an input, and the board and panel work.

**IO12.**
- The manual lists IO12 both as the reset key and as the IP5306 I2C SCL line.
- The firmware never talks to the IP5306.

## References

- Good Display product page (board, panel, example code, specification):
  https://www.good-display.com/product/574.html
- Good Display Chinese board manual 《ESP32-133C02-IL 驱动开发板说明书》 on
  good-display.cn: the pin and button source above.
- GDEP133C02 datasheet (Good Display).
