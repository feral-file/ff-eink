# Firmware updates: signed LAN OTA, an offline updater, CI releases

Status: design, decided 2026-11-10 (Brandon). Not implemented yet.

## Goal and constraints

- An operator updates all ~55 panels in place, from a laptop on the panel
  VLAN, with files from a USB stick. No internet and no cloud, and no panel
  comes off the wall.
- A bad image must never strand panels. Physical recovery means unmounting a
  panel from the wall.
- Anyone on the VLAN can reach the plain-HTTP API, so the firmware must
  accept only images we signed.
- The partition layout changes only over USB. It must be final before the
  development units ship (2026-12-10).

## Decisions

| # | Decision | Why |
|---|---|---|
| 1 | Two OTA app slots plus `otadata`, from now on | An interrupted write leaves the old slot booting. Layout is USB-only, so set it before shipping. |
| 2 | ESP-IDF "signed apps without hardware secure boot" (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072, Secure Boot V2 scheme), verified on update only | The signature check is built into `esp_ota_end`, so we write no crypto code. No eFuses are burned, and USB reflashing stays possible. |
| 3 | Bootloader rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) | A new image that cannot reach the network is reverted automatically. |
| 4 | Updater: one Go binary per OS, from GitHub Releases, copied to the USB stick | Cross-compiles from one CI runner, with no runtime on the laptop. The repo's other services are Go. |
| 5 | GitHub Actions builds, signs and releases on a version tag | The signing key never leaves CI, and every release is reproducible from a tag. |

## Flash layout (16 MB)

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB (unchanged: names and counters survive) |
| otadata | data/ota | 0xF000 | 8 KB |
| ota_0 | app | 0x20000 | 3 MB |
| ota_1 | app | 0x320000 | 3 MB |

- The app is about 0.6 MB today, so 3 MB per slot leaves room.
- The remaining ~9.8 MB stays unallocated.
- Moving from the current single-app layout is a one-time USB flash per board, which also replaces the bootloader. This is part of provisioning anyway.

## Signing

- **Keys.**
  - The release key is RSA-3072, generated once with `espsecure.py generate_signing_key --version 2`. It is stored only as a GitHub Actions secret in a protected `release` environment that needs a reviewer.
  - A separate **dev key** (gitignored, per developer) signs local builds.
- **Chain of trust.**
  - A new image is verified against the public key in the **running** app's signature block.
  - The running app must itself be signed: an unsigned app aborts at startup.
  - So every image we flash, bench builds included, is signed.
  - A board flashed with a dev-signed image accepts only dev-signed updates. Moving it to release keys takes one USB flash.
- **Signing step.**
  - Build with `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n` (unsigned output).
  - Then run `espsecure.py sign_data --version 2 --keyfile … --output app-signed.bin app.bin` in CI, or locally with the dev key.
- **Key loss or compromise.** The fix is a USB reflash with a new key. No eFuse ties a board to a key.

## Device side

`POST /api/v1/firmware`:

1. **Request.** The body is the signed app image, with an exact `Content-Length` of at most 3 MB.
2. **Single-flight.** It shares the panel state machine: 429 while a frame uploads or refreshes. While the firmware uploads, frames get 429.
3. **Write.** The body streams through `esp_ota_begin/write/end` into the inactive slot, with the same deadline-plus-`recv_wait_timeout` pattern as frames (a longer deadline, about 60 s).
4. **Verify.** `esp_ota_end` checks the image and its signature. On failure the response is `422 {"error":"image"}` and nothing changes.
5. **Accept.** `esp_ota_set_boot_partition`, then `202 {"version":"…"}`. The device restarts once the panel is idle, with `restart_cause: ota`.

**After an update** the new app boots in the *pending-verify* state:

- It marks itself valid (`esp_ota_mark_app_valid_cancel_rollback`) after it has held an IP and served HTTP for 60 s.
- Any restart before that, from a crash, a watchdog or a power loss, boots the previous slot.
- If no IP arrives within 10 minutes, it calls `esp_ota_mark_app_invalid_rollback_and_reboot`.

**Status additions:**

- `fw` comes from `esp_app_get_description()->version`, which is the git tag.
- `ota_state` is `valid` or `pending`.
- `rolled_back` is true when the last update was reverted.

**Exempt from updates:** maintenance mode and the bench build. Neither starts the network.

## The updater (`ffeink`)

A single binary with these subcommands. It replaces `pusher/panelctl.py` once it exists.

| Command | Does |
|---|---|
| `ffeink scan 10.20.0.0/24` | Lists panels: name, hw_id, IP, fw, state. Flags duplicate names. |
| `ffeink status <ip>` / `ffeink name <ip> <name>` | Provisioning, as `panelctl.py` does today. |
| `ffeink update --image ff-eink-v1.2.0.bin 10.20.0.0/24` | The rollout below. |

**Rollout:**

1. **Check the image.** It must be signed and well-formed (header magic, size, a signature block present), and its version must be newer than what each panel reports. Panels already on that version are skipped.
2. **Canary.** One panel first.
   - Upload the image and wait for the restart.
   - Require the new `fw` plus `ota_state: valid` within about 3 minutes.
   - Stop everything if the canary fails or rolls back.
3. **Batches.** The rest go in batches of 5. Each panel is attempted only while `state == idle`, so a refresh is never cut.
4. **Report.** A table per panel: updated, skipped, failed, rolled back. A CSV is written next to the binary.
5. **Re-runs.** The rollout is idempotent: running it again finishes whatever is left.

**Time.** About 1–2 s of upload per panel at USB Full Speed, plus the reboot and the 60 s validation. All 55 panels take about 10–15 minutes.

**The USB stick holds:**

- `ffeink-darwin-arm64`, `ffeink-darwin-amd64`, `ffeink-windows-amd64.exe`, `ffeink-linux-amd64`;
- `ff-eink-vX.Y.Z.bin`;
- `SHA256SUMS`;
- a one-page `UPDATING.txt`.

**Platform friction to settle before handing a stick to the museum:**

- *macOS* quarantines unsigned downloaded binaries. Feral File already has an Apple Developer account (ff-app), so CI can sign and notarize the darwin binaries.
- *Windows* SmartScreen warns on unsigned executables. Either accept the warning in `UPDATING.txt`, or buy a code-signing certificate.

## CI/CD (GitHub Actions, `feral-file/ff-eink`)

| Trigger | Jobs |
|---|---|
| pull request | Firmware build, normal and bench (container `espressif/idf:v5.5.6`); `make -C test`; pytest; `go test` and `go vet` for the updater |
| tag `v*` | The same checks, then **release** (below) |

The release job runs in a protected `release` environment that requires a reviewer:

1. **Build firmware** with `PROJECT_VER` taken from the tag.
2. **Sign the app** with the release key from the environment secret (`espsecure.py sign_data`).
3. **Assemble the release artifacts:**
   - `ff-eink-vX.Y.Z.bin`: the signed app, used for OTA;
   - `ff-eink-vX.Y.Z-full.zip`: bootloader, partition table, signed app and `flash_args`, used for USB provisioning of new boards;
   - the updater binaries: `GOOS/GOARCH` cross-compiled, with darwin signed and notarized;
   - `SHA256SUMS`.
4. **Create a GitHub Release** with these assets and generated notes.

The bench build is not released: it is a lab tool and is built locally with the dev key.

## Open

- `PUT /api/v1/config` (renaming) stays unauthenticated on the VLAN, like
  the rest of v1. Once firmware is signed, renaming is the cheapest way to
  misdirect images to a wall position. Decide whether config gets a gate.

## Staging

1. **Partition table, rollback and dev-key signing.** USB-flash the bench board with them; it is verified by booting and by G1 still passing.
2. **`POST /api/v1/firmware`, pending-verify and the status fields.** Exercised by hand with `curl` against the bench board on the PoE adapter, covering: a good image, a bad signature (422), a power pull mid-upload (old slot boots) and a deliberately broken image (rollback).
3. **`ffeink` updater** with scan, status, name and update. `panelctl.py` is removed.
4. **CI workflow and the release key.** The first tagged release is cut and the stick contents are tested end to end.

Stages 1–2 come before the 24 h soak (G6), so the soak runs the image that ships. Stages 3–4 can overlap with the soak.
