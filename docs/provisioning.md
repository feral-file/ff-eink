# Provisioning new boards

How to turn a batch of new kits (board, panel, PoE adapter) into boards
that are ready to ship. `pusher/provision.py` does the repetitive steps;
a person plugs cables, looks at panels and sticks labels.

## What "ready to ship" means

A board is ready when all of these hold:

- It was flashed by `provision.py flash` from a **release** `-full.zip`,
  whose app is signed with the release key. A board only accepts OTA images
  signed with the key it was flashed with, so a board that leaves with a
  dev-key image would need a USB reflash on site
  ([ota.md](ota.md#signing-and-keys)).
- Its `nvs` was erased at flashing, so nothing from the factory image is
  left in it.
- In one unbroken run of passing checks (correct firmware, `ota_state`
  `valid`, the expected PoE adapter, no panel error, frame shown, no
  reboot), it showed the chart for a person to look at. At least an hour
  after the run began, it passed a final check that left the panel white.
  The hour catches boards that fail early. White is how the vendor says a
  panel should be stored ([hardware.md](hardware.md#vendor-guidance)).
- It has a label with its default name (`ffeink-` plus the last 6 hex of
  its hw_id), and a row in the inventory CSV with `ready` `yes`.

**Names are not set here.** Wall positions belong to the installation, and
the default name is already unique and printed on the label. On site, the
installer maps label to position with `panelctl.py name`
([pusher.md](pusher.md#panelctlpy-provisioning-inventory-firmware-updates)).

## What you need

- A Mac or Linux machine with this repository and Python 3.9+, plus a
  virtual environment holding the two dependencies:
  ```sh
  python3 -m venv ~/ff-provision && ~/ff-provision/bin/pip install "esptool>=4.8,<5" "pillow>=9.1"
  ```
  esptool 5 changed its command line; the script uses the 4.x one.
- The release's `ff-eink-vX.Y.Z-full.zip`, downloaded from the GitHub
  release. Check it against `SHA256SUMS`. The script refuses a zip whose
  app is not signed with the key in `keys/release-key.sha256`, and one that
  writes anything other than bootloader, partition table, `otadata` and app.
- **Flash station:** a powered USB hub with data, and one USB-C cable per
  hub port.
- **Check station:** a PoE switch (802.3af) or one injector per board, a
  router giving DHCP, and the kits' own adapters. Use a network of its
  own: `check` leaves panels it did not flash alone, but reports them and
  fails.
- Labels, and a pen or a printer.

Run one `provision.py` at a time per inventory file: two running at once
overwrite each other's rows.

## Station 1: flash

**Boards only, no panel attached.** Every esptool run resets the chip. With
no panel attached, a reset cannot cut a refresh short, so the script needs
no wait.

```sh
cd ff-eink
~/ff-provision/bin/python pusher/provision.py flash --zip ff-eink-vX.Y.Z-full.zip
```

Then plug boards into the hub, as many at a time as there are ports. For
each board the script:

1. reads the chip MAC, which is the board's `hw_id`;
2. erases `nvs`, then writes the release (bootloader, partition table,
   `otadata`, app); esptool verifies every region;
3. records the board in `provision.csv` and prints `OK <hw_id>. Unplug it.`
   with a beep.

About 30 s per board, all ports in parallel. Unplug each board when it
says so and plug in the next. Ctrl-C stops the script, but not while a
board is still writing: a cut write leaves that board to be flashed again,
and the script says so. Running it again continues the same inventory.

| Message | Do |
|---|---|
| `FAILED before identifying the board` | Unplug and replug. A board that already runs ff-eink drops its USB port 60 s after boot; if it is slow to be picked up, plug it in with **SW8 held** for at least 1 s (maintenance mode). |
| `FAILED: esptool …` | Unplug and replug to retry. If it fails again, set the board aside and write why in its `notes` column. |

## Station 2: check and burn in

Connect each board to its panel and its PoE adapter, and connect the
adapter to the PoE switch. As many at a time as there are ports. Wait
about 2 minutes: 60 s flash grace window, then USB host, link and DHCP.

```sh
~/ff-provision/bin/python pusher/provision.py check 10.20.0.0/24
```

`check` finds the boards of the inventory on the network and checks them in
parallel. It checks firmware against what was flashed, `ota_state`, adapter
and panel errors. Then it shows the colour chart with the panel's own name
in large letters at the bottom. It prints one line per board and updates
the inventory.

1. **Look at every panel.** The chart must be complete and clean: no
   missing or shifted bars, no stripes, no wrong colours. Write anything
   wrong in that board's `notes` column and set the board aside.
2. **Label it.** Write the name shown on the panel, and the `hw_id` from
   the printout, on the label. Stick it on the board and on the back of the
   panel.
3. **Leave the batch powered for at least an hour.** Do not unplug
   anything: a reboot ends the run, and the board starts again from the
   chart.
4. **Final check:**
   ```sh
   ~/ff-provision/bin/python pusher/provision.py check 10.20.0.0/24 --white
   ```
   This repeats the checks and leaves every panel white. Boards with
   `ready` `yes` can be unplugged and packed. With check `ok` but `ready`
   `no`, either the hour is not up yet (run `--white` again later) or the
   run has no chart check (run without `--white`, look, then wait the hour).
   Any other check text says what is wrong.

`check` also lists boards that were flashed, are not ready and are not on
the network: the batch's stragglers.

| Check text | Likely cause |
|---|---|
| not on the network at all | adapter link or PoE: try the adapter on a laptop first ([bench.md](bench.md#what-the-kit-needs-that-it-doesnt-have)) |
| `NOT CHECKED, not flashed by this inventory` | another panel on this network, or a board flashed with another inventory file; nothing was pushed to it |
| `fw …, flashed …` | the board runs another image than the one recorded; flash it again |
| `ota_state pending` | not expected after a USB flash (that image has no pending state); flash again |
| `adapter …, expected 0bda:8152` | a different USB network chip; see [bench.md](bench.md#0-adapter-check-before-the-board-is-involved) |
| `panel_errors …` | FPC seating or a bad panel; see [hardware.md](hardware.md) |
| `rebooted since the last check` | the board lost power or crashed during burn-in; `reset_reason` says which |
| `never idle`, `frame not shown in time` | the panel did not finish a refresh; look at the panel and the FPC |

## Time for a batch

| Step | Per board | 55 boards |
|---|---|---|
| Flash | about 30 s, parallel over the hub | about 5–10 min on a 7-port hub |
| Check, label | about 5 min per batch, plus labelling | batches as large as the PoE switch |
| Burn-in | 1 h per batch, unattended | about 4–5 h in all with a 16-port switch |

Flash every board first, then run batches through the check station.

## The inventory

`provision.csv`, one row per board, keyed by `hw_id`. Both commands
update it. Between runs, `notes` may be edited in a plain text editor.
Avoid spreadsheets: they reformat times and hex ids, and other columns
you add are dropped on the next write. Keep the file with the shipment
records: it maps every label to a board.

| Column | Meaning |
|---|---|
| `hw_id` | chip MAC, 12 hex; also in `/api/v1/status` |
| `fw`, `flashed_utc`, `flash` | release written, when, `ok` or `failed: …` |
| `name`, `ip` | as seen by the last check |
| `checked_utc`, `check` | last check, `ok` or what was wrong |
| `refresh_ms`, `boot_count`, `brownouts` | from the last check |
| `first_ok_utc` | start of the current unbroken run of passing checks |
| `chart_utc` | last chart check in that run |
| `ready` | `yes` after a white check at least an hour into a run that has a chart check |
| `notes` | yours |

## Before the first batch

Do once, on the first new kits, and update this page if anything differs.
The steps above were tried only on the reference board, which had already
been flashed many times.

- Do the kits arrive with the panel unplugged from the board? Station 1
  relies on it. If they arrive connected, decide between unplugging the
  FPC and adding a wait that outlasts the factory image's power-up refresh.
- Does the factory image keep the USB serial port up, so esptool can
  connect? (On the reference board it did.) If not, there is no BOOT
  button on this board: ask Good Display how to enter download mode.
- Run one kit through both stations end to end with a release
  candidate. That is also the end-to-end release test in
  [ota.md](ota.md#open).
