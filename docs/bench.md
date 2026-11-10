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

## 2. First flash and the two ways back

Normal mode turns the only USB-C into a USB host port, so the board stops
being a serial/JTAG device and esptool cannot reach it. Two ways back:

- **Grace window (always works):** for 15 s after every boot in normal mode,
  USB-Serial-JTAG is still up. Have the command ready, press **Reset**, and
  run `idf.py -p /dev/cu.usbmodem1101 flash` within the window.
- **Maintenance mode:** hold **SW2 (IO12)** while pressing **Reset**. No USB
  host, no network; flashing and logs work indefinitely.

The first flash relies on the grace window. Right after it, still on the Mac
cable, check maintenance mode before leaving the board in normal mode:

1. `idf.py -p /dev/cu.usbmodem1101 monitor`, press Reset without SW2: the log
   shows `SW2=0 ... -> normal mode` and `USB host starts in 15 s`.
2. Hold SW2, press Reset: `SW2=1 ... -> maintenance mode`, and the port stays.
3. In maintenance mode, `idf.py -p /dev/cu.usbmodem1101 flash` succeeds.

If step 2 fails, SW2 is not wired as the spec says: keep using the grace
window and record it.

## G1: panel from our driver, USB power only

In maintenance mode (Mac cable), press **SW3** to cycle the generated
patterns: chart → bars → checker. Photograph each.

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

## Network bring-up

Board on the adapter, normal mode (no SW2). Find the DHCP lease on the router,
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
