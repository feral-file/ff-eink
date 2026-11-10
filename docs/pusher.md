# Reference tools (`pusher/`)

ff-eink ships the panel firmware, its [API](api.md), and these reference tools. A controller that drives a whole installation is outside ff-eink's scope: each installation builds its own against the API. The tools below show how to call the API, and cover testing and operating the panels.

**Requirements:**
- Python 3.9 or newer.
- `dither.py` needs Pillow ≥ 9.1, and so does `push.py --chart-ends` (it draws the chart with `dither.py`). Everything else uses the standard library only.

## `dither.py`: any image to a panel frame

```sh
python3 pusher/dither.py photo.jpg other.png -o pusher/out --preview
python3 pusher/dither.py -o pusher/out --chart          # the six-colour test chart
```

For each input image, `dither.py`:
1. applies the EXIF rotation;
2. cover-crops to portrait 3:4 and scales to 1200×1600;
3. applies Floyd–Steinberg dithering onto the six measured Spectra 6 inks;
4. packs the result into the 960,000 B frame format.

It writes one `<name>.bin` per input. With `--preview` it also writes `<name>.preview.png`, which shows the result in the panel's inks.

`--palette bpc80` selects a brighter, lossy ink set. The default is `d65`. The colours are measured values from Frans-Willem/epd-dither; only the numbers are used.

## `push.py`: push frames on a schedule (reference pusher and soak harness)

```sh
python3 pusher/push.py --host <panel-ip> pusher/out/photo.bin --count 1                     # show one picture
python3 pusher/push.py --host <panel-ip> pusher/out/*.bin --count 1440 --chart-ends \
    --log run.csv --pull-file PULL                                                            # 24 h at 60 s
```

**Each tick** (the default interval is 60 s):
1. Wait (up to a quarter of the interval) until the panel is idle.
2. `POST` the next frame.
3. Poll `/api/v1/status` until the panel reports that frame as shown.

**Logging.**
- Every tick gets one CSV row.
- A frame that is not shown is a **miss**, and is attributed to exactly one cause. The causes and their order are in [api.md](api.md#misses).
- The run exits non-zero if any miss is `unattributed` or `no_evidence`.

**`--pull-file`.** While that file exists, misses count as deliberate cable pulls. Run `touch PULL` before pulling a cable, and `rm PULL` once the panel is back.

## `panelctl.py`: provisioning, inventory, firmware updates

```sh
python3 pusher/panelctl.py scan 192.168.1.0/24                  # every panel: name, hw_id, IP, fw, state
python3 pusher/panelctl.py status <ip>
python3 pusher/panelctl.py name <ip> lobby-r03-c07              # wall-position name
python3 pusher/panelctl.py update --image ff-eink-v0.2.0.bin 192.168.1.0/24
```

**`scan`**
- Flags duplicate names and exits non-zero if it finds any.
- Its timeout is 4 s, so a panel that is busy receiving a frame is not missed.

**`name`**
- Sets the name through `PUT /api/v1/config`.
- The name rules are in [api.md](api.md).
- When a spare replaces a dead panel, give the spare the dead panel's name.

**`update`** rolls a signed firmware image out to every panel in the network ([ota.md](ota.md)):
1. Check the image locally.
2. Update one canary panel. If it fails, stop.
3. Update the rest in batches of 5.
4. Touch each panel only while its display is idle.
5. Count a panel as updated only when it reports the new version with `ota_state: valid`.
6. Write a result table to `update-report.csv`.

Running `update` again finishes whatever is left.

Step-by-step instructions for museum operators are in [UPDATING.txt](UPDATING.txt).

## Options worth knowing

| Tool | Option | Default |
|---|---|---|
| `push.py` | `--interval` seconds between frames | 60 |
| `push.py` | `--poll` status poll period | 1 s |
| `push.py` | `--port` | 80 |
| `push.py` | `--pull-file` (or env `FF_EINK_PULL_FILE`) | none |
| `panelctl.py scan` | `--timeout` per address | 4 s |
| `panelctl.py update` | `--batch` panels at once after the canary | 5 |
| `panelctl.py update` | `--wait` per panel, idle to valid | 360 s |
| `panelctl.py update` | `--report` CSV path | `update-report.csv` |
