# HBC agent guidance

For build, review, Dolphin, Wii, or release work, read [the HBC project skill](.agents/skills/hbc-build-and-review/SKILL.md). Use [README.md](README.md) for contributor setup and [REVIEW.md](REVIEW.md) for the subsystem review plan and findings.

- Use the installed devkitPro toolchain and standard host-tool locations. Keep local Python packages in `.venv`.
- Keep Wii keys private. `keys/` and `.venv/` are ignored. Check staged files before any commit.
- A direct DOL launch does not provide the installed channel's NAND identity. Check the DOL and retail WAD as separate outputs.
- When making a commit, bump both version fields in `channel/channelapp/config.h` and `channel/title/Makefile` to the same `major.minor.patch` release in that commit.
- Preserve original line endings when editing files. Some PyWii and Wiiload files use CRLF; check them with `git -c core.whitespace=cr-at-eol diff --check`.
- Prefer a focused fix with a matching check. Record larger ideas in `REVIEW.md` with evidence and a clear trigger.
