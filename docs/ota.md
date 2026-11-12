# Firmware updates: LAN OTA from a laptop, released with the firmware

Status: stages 1–4 are written; the on-hardware checks in [bench.md](bench.md#ota-checks-before-g6-so-the-soak-runs-the-shipping-image) are pending.

## Goal and constraints

- An operator updates all ~55 panels in place from a laptop on the panel
  VLAN, with files from a USB stick. No internet and no cloud are involved,
  and no panel comes off the wall.
- A bad image must never strand panels. Physical recovery means unmounting a
  panel from the wall.
- The partition layout can only be changed over USB, so it has to be final
  before panels are deployed.
- Only firmware built and released by this repository's CI may be installed
  over the network. The VLAN sits in a public lobby that students use.

## Decisions

| # | Decision | Why |
|---|---|---|
| 1 | Two OTA app slots plus `otadata`, from now on | An interrupted write leaves the old slot booting. The layout can only change over USB, so it is set before shipping. |
| 2 | **Signed images.** ESP-IDF "signed apps without hardware secure boot" (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072, Secure Boot V2 format), checked on update | `esp_ota_end` checks the signature, so we write no crypto code. No eFuses are burned, so a key problem is fixed by a USB reflash, never a dead board. |
| 3 | Bootloader rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) | A new image that cannot reach the network is reverted automatically. |
| 4 | Updater in **Python**, standard library only: `pusher/panelctl.py update` | A plain script avoids the per-OS executable trust checks (Gatekeeper, SmartScreen). It needs Python 3.9+ on the laptop. |
| 5 | GitHub Actions builds, signs and releases on a version tag, in the protected `release` environment; a maintainer approves each run | The release key never leaves CI, and every shipped image is reproducible from a tag. |

## Flash layout (16 MB)

Defined in `partitions.csv`:

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB, unchanged, so names and counters survive |
| otadata | data/ota | 0xF000 | 8 KB |
| ota_0 | app | 0x20000 | 3 MB |
| ota_1 | app | 0x320000 | 3 MB |

- The app is about 0.6 MB today. The remaining ~9.9 MB is unallocated.
- **`nvs` never moves or shrinks.** It holds the panel's name, and changing it would reset every wall position.
- Moving from the old single-app layout is a one-time USB flash per board, which also replaces the bootloader. Provisioning includes it anyway.

## Signing and keys

**What it protects, and what it does not**

- *Protects against* a network attacker flashing their own firmware over the VLAN, and against a tampered image.
- *Does not protect against* anyone with physical access: USB flashing still works, because no secure-boot eFuse is burned. That is the price of never bricking a board over a key.
- *Does not protect* the other v1 calls. Frames and renaming stay unauthenticated on the VLAN.

**How the check works**

- A new image is verified against the public key in the **running** image's signature block.
- An unsigned image aborts at startup.
- So every image put on a board is signed, the bench build included. A board accepts updates only from the key it was last USB-flashed with.

| Key | Where it lives | Signs |
|---|---|---|
| **Release key** (RSA-3072) | The `SIGNING_KEY` secret of the `release` environment, plus one offline backup held by the maintainers. Its public-key digest is pinned in `keys/release-key.sha256`; `release.yml` refuses to publish an image signed with any other key. | Everything that ships. A board is USB-provisioned only with the `-full.zip` of a release. |
| **Dev key** | Each developer's own `signing-key.pem`, gitignored. Created with `espsecure.py generate_signing_key --version 2 --scheme rsa3072 signing-key.pem`. | Lab boards only. A lab board moves to the release key with one USB flash of a release `-full.zip`. |

**Protecting the "only from CI" property**

- The `release` environment requires approval from a maintainer listed as its reviewer before its job can read the secret.
- Pull-request workflows never see the secret: `ci.yml` signs its images with a throwaway key.
- The release job runs the tagged commit's own workflow and code with the key, and a `v*` tag can point at any commit. So:
  - limit the environment's deployments to `v*` tags, and restrict who may create `v*` tags with a tag ruleset;
  - **approvers check that the tagged commit is on `main`** before approving;
  - protect `main` (see Open), so workflow changes need a reviewed PR.
- The approval is the real control: whoever can get a run approved can have arbitrary code signed.
- The release job also refuses to publish if the image's version string is not exactly the tag, or if it was signed with any key other than the one in `keys/release-key.sha256`.

**When the key changes**

- An image must be signed with the key of the image it replaces, so a key cannot be rotated over the air.
- If the key is lost, OTA ends and USB flashing still works.
- If the key leaks, every panel is USB-reflashed with a new key.
- Hence the offline backup.

**No anti-downgrade.** Rejecting older images would need eFuses. Anyone on the VLAN can reinstall any *older* release-signed image. "Only CI-signed firmware" holds; "only the latest release" does not.

**Creating the release key**, once, by a maintainer:

1. Run `espsecure.py generate_signing_key …` on a trusted machine.
2. Paste the PEM into the environment secret and store the backup.
3. Commit the digest printed by `espsecure.py signature_info_v2` for an image signed with it.
4. Delete the local file.

## Device side

### `POST /api/v1/firmware`

- **Body:** the app image (`ff-eink.bin`). `Content-Length` must be exact and at most 3 MB.
- **Busy rule:** it shares the panel state machine. A firmware upload gets 429 while a frame uploads or refreshes, and frames get 429 while the firmware uploads.
- **Write:** the body streams through `esp_ota_begin/write/end` into the inactive slot. It uses the same deadline-plus-`recv_wait_timeout` pattern as frames, with a longer deadline of about 60 s.
- **Image check fails** (`esp_ota_end` checks the header, the chip, the SHA-256 and the **signature**): `422 {"error":"image"}`, and nothing changes.
- **Accepted:** `esp_ota_set_boot_partition`, then `202 {"version":"…"}`. The device restarts once the panel is idle, with `restart_cause: ota`.

### First boot after an update

The new app boots in the *pending-verify* state.

- It marks itself valid (`esp_ota_mark_app_valid_cancel_rollback`) after it has held an IP for 60 s with the HTTP server running. If that otadata write fails, it stays `pending` and retries.
- Any restart before that boots the previous slot: a crash, a watchdog reset or a power loss.
- If no IP arrives within 10 minutes, it calls `esp_ota_mark_app_invalid_rollback_and_reboot`.

### Status additions

- `fw` comes from `esp_app_get_description()->version`, which is the git tag.
- `ota_state` is `valid` or `pending`.
- `rolled_back` is true when the last update was reverted.

Maintenance mode and the bench build never start the network, so they never take updates.

## The updater (`pusher/panelctl.py`)

| Command | What it does |
|---|---|
| `panelctl.py scan 10.20.0.0/24` | Lists every panel (name, hw_id, IP, fw, state) and flags duplicate names. |
| `panelctl.py status <ip>`, `panelctl.py name <ip> <name>` | Provisioning, as today. |
| `panelctl.py update --image ff-eink-v1.2.0.bin 10.20.0.0/24` | Runs the rollout below. |

**Rollout:**

1. **Check the image locally.** It must have the ESP app header magic, the chip id for ESP32-S3, a size of at most 3 MB, and a signature sector. The panel verifies the signature itself. Its version string, read from the image's app description, must differ from each panel's `fw`. Panels already on that version with `ota_state: valid` are skipped. A panel already on it but still `pending` is waited on, because it may yet roll back.
2. **Canary.** Update one panel first:
   - upload, wait for the restart;
   - require the new `fw` and `ota_state: valid` within `--wait` (default 360 s; it normally takes about 2.5 min);
   - then **install the same image over itself once more**. That second upload is served by the new image, so it proves the new image can take the *next* update. A release whose update path is broken would otherwise reach every panel, and each would need a USB reflash;
   - stop everything if either pass fails or rolls back.
3. **Batches.** Update the rest five at a time. Each panel is attempted only while `state == idle`, so a refresh is never cut.
4. **Report.** Print one line per attempted panel as it finishes, and write every panel to `update-report.csv`: `updated`, `skipped`, `not attempted` (after a canary failure), or `failed: <reason>` (`rolled back`, `old image booted`, `not valid in time`, `never idle`, `unreachable`, `upload …`). The exit code is 0 only if every panel ended `updated` or `skipped`.
5. **Re-run.** Running it again finishes whatever is left.

**Time:** per panel, about 1–2 s of upload plus roughly 2.5 minutes of restart, 60 s flash grace window, DHCP and 60 s validation. The canary takes two passes (about 5 minutes); then batches of 5 put 55 panels at roughly 30 minutes.

**The USB stick holds** the release's `panelctl.py`, `ff-eink-vX.Y.Z.bin`, `SHA256SUMS` and `UPDATING.txt` (which includes how to get Python 3 on macOS and Windows); copying every release asset is fine too.

## CI/CD (GitHub Actions, `feral-file/ff-eink`)

| Trigger | Jobs |
|---|---|
| Branch push (not `v*` tags) or pull request (`ci.yml`) | Firmware build, normal and bench, in the `espressif/idf:v5.5.6` container, signed with a throwaway key; `make -C test`; pytest |
| Tag `v*` (`release.yml`) | The release steps below, in the `release` environment after approval |

**Release steps:**

1. Build the firmware, which is signed at build time with the release key from the secret. The version comes from `git describe`, which is the tag.
2. Check that the image's public-key digest equals `keys/release-key.sha256`.
3. Assemble the release assets:
   - `ff-eink-vX.Y.Z.bin`, the app image used for OTA;
   - `ff-eink-vX.Y.Z-full.zip`, containing the bootloader, partition table, `ota_data_initial.bin`, the app and `flash_args`, used for USB provisioning of new boards;
   - `panelctl.py`, `dither.py`, `push.py` and `UPDATING.txt`;
   - `SHA256SUMS`.
4. Create a GitHub Release with those assets and generated notes. This runs in a separate job on the hosted runner, which has `gh`.

The bench build is a lab tool. It is built locally and is not released.

## Open

- **Unauthenticated calls.** `PUT /api/v1/config` (renaming) and frames stay unauthenticated on the VLAN, like the rest of v1. Firmware is protected by the signature.
- **One-time GitHub setup, by a maintainer.**
  1. Create the `release` environment with the maintainers as required reviewers, deployments limited to `v*` tags.
  2. ~~Create the release key, its secret and its backup, and commit `keys/release-key.sha256`.~~ Done (2026-11-12).
  3. Protect `main`, and add a tag ruleset restricting who may create `v*` tags.

## Staging

1. **Partition table, rollback and signing.** USB-flash the bench board with a dev-key image. Verify it by booting and by G1 still passing.
2. **Firmware upload endpoint.** Add `POST /api/v1/firmware`, the pending-verify logic and the new status fields. Test by hand with `curl` on the PoE adapter:
   - a good image;
   - a corrupt image (expect 422);
   - a power pull during the upload (the old slot must boot);
   - a deliberately broken image (it must roll back).
3. **Updater.** `panelctl.py update`, written.
4. **CI.** `ci.yml` and `release.yml` are written. Then: the GitHub setup above, the first tagged release, and an end-to-end test of the stick contents.

Stages 1 and 2 come before the 24 h soak (G6), so the soak runs the image that ships. Stages 3 and 4 can overlap with the soak.
