---
name: hbc-build-and-review
description: Build, test, debug, or review this Homebrew Channel project across its Wii runtime, WAD packaging, and host tools. Use for project changes and subsystem audits; use README for one-time contributor setup.
---

# HBC build and review

Read `README.md` for dependencies and `REVIEW.md` for the current subsystem plan. Trace the affected path from input to output before changing code. Fix the smallest shared cause that explains a confirmed failure. Keep future work separate from verified defects.

## Build checks

From the repository root, use the installed devkitPro toolchain:

```sh
make -C wiipax
make -C channel/channelapp channel
make -C channel PYTHON="$(pwd)/.venv/bin/python"
make -C channel/title PYTHON="$(pwd)/.venv/bin/python" check
tests/host_bounds.sh
```

The full build needs `pycryptodomex` in `.venv`, SoX, `msgfmt`, host libpng, and a private 16-byte `~/.wii/common-key`. Use `make -C channel clean` when testing a clean build. The retail WAD is `channel/title/channel_retail.wad`. The optional DPKI target needs separate private signing keys.

## Runtime checks

Test a new DOL in an isolated Dolphin profile. The sibling Wii64 project's `.dev/dolphin_test.sh` shows the local Intel Mac profile and log setup; adapt its steps to this DOL rather than reusing a Wii64 ROM workflow. Read only the latest boot segment in Dolphin's append-only log. Match guest fault addresses to the ELF from the same build. A link result alone does not show that the menu or loader works.

For a real Wii check, send the DOL through the existing Homebrew Channel with Wiiload, then launch a known DOL such as Wii64 through the new menu. Keep an installable WAD off the real Wii until installation is an explicit test objective.

## Packaging and release checks

For WAD changes, parse the ticket and TMD and verify both content sizes and decrypted SHA-1 hashes. Do not print, copy into the repo, or stage keys. Before a commit, inspect staged names and bump both version fields named in `AGENTS.md`. State whether validation covers a DOL boot, a WAD build, or an installed channel.
