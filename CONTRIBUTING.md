# Contributing to ff-eink

Thanks for helping. This file covers the workflow. The technical details
live in the docs it links to.

## Before you start

- Read [docs/development.md](docs/development.md): toolchain (ESP-IDF
  v5.5.6), the signing key every build needs, flashing and recovering a
  board, and the code map.
- If you have a board, read [docs/hardware.md](docs/hardware.md) first. It
  lists what is verified, what is not, and how to avoid cutting a panel
  refresh short.
- If your change touches the API, read [docs/api.md](docs/api.md).
  Controllers in the field depend on it. Changes are **additive only**:
  never change the meaning of an existing field, status code or error.

## Your signing key

Every image must be signed to build ([docs/ota.md](docs/ota.md#signing-and-keys)).

- Generate your own dev key once:
  `espsecure.py generate_signing_key --version 2 --scheme rsa3072 signing-key.pem`
- It is gitignored. Never commit a key.
- A board flashed with your dev key accepts OTA updates signed with your
  key only.
- Release images are signed in CI with a key that contributors never need.

## Making a change

1. Branch from `main` and keep the change focused.
2. Add or update tests next to the code:
   - C helpers: `test/` (`make -C test`, run with ASan and UBSan).
   - Python tools: `pusher/tests/` (`pytest -q pusher/tests`).
3. Build both firmware variants: `idf.py build`, plus the bench build
   command in docs/development.md. Both must build with zero warnings.
4. Update the docs your change affects, in the same pull request. The docs
   are expected to stay in sync with the code.
5. Open a pull request against `main`. CI builds both variants and runs
   both test suites.

If you tested on hardware, say so in the pull request: which board, which
checks from [docs/bench.md](docs/bench.md), and the results.

## Style

- **C:** ESP-IDF conventions; keep the surrounding file's style. Comment
  *why* (invariants, hardware facts, hazards), not what.
  Cite the source of any hardware fact.
- **Python:** standard library only for the operator tools (`panelctl.py`,
  `push.py`); Pillow only in `dither.py`.
- **Simple first.** Prefer deleting or simplifying over adding options.

## Licence

By contributing you agree your contribution is released under the MIT
licence in [LICENSE](LICENSE). Do not paste code from incompatibly licensed
projects. GPL and AGPL sources may inform a method, but no code may be
copied from them. Record any third-party data you add in THIRD_PARTY_NOTICES.md.
