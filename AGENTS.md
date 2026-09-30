# HBC agent guidance

For build, review, Dolphin, Wii, or release work, read [the HBC project skill](.agents/skills/hbc-build-and-review/SKILL.md). Use [README.md](README.md) for contributor setup and [REVIEW.md](REVIEW.md) for the subsystem review plan and findings.

- Use the installed devkitPro toolchain and standard host-tool locations. Keep local Python packages in `.venv`.
- Keep Wii keys private. `keys/` and `.venv/` are ignored. Check staged files before any commit.
- A direct DOL launch does not provide the installed channel's NAND identity. Check the DOL and retail WAD as separate outputs.
- When making a commit, bump both version fields in `channel/channelapp/config.h` and `channel/title/Makefile` to the same `major.minor.patch` release in that commit.
- Text files are LF in the repo and in every checkout: `.gitattributes` sets `* text=auto eol=lf`, which overrides Git for Windows' `core.autocrlf=true`. Write LF. The few upstream files kept byte-for-byte with CRLF are marked `-text` there; keep their endings when editing them. `tests/test_line_endings.py` checks it, and `tools/fix_line_endings.py` converts a checkout made before 1.8.3.
- Prefer a focused fix with a matching check. Record larger ideas in `REVIEW.md` with evidence and a clear trigger.
