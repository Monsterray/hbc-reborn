# HBC agent guidance

For build, review, Dolphin, Wii, or release work, read [the HBC project skill](.agents/skills/hbc-build-and-review/SKILL.md). Use [README.md](README.md) for contributor setup and [REVIEW.md](REVIEW.md) for the subsystem review plan and findings.

- Use the installed devkitPro toolchain and standard host-tool locations. Keep local Python packages in `.venv`.
- Never convert bases, add offsets or work out sizes in your head: run it (`python -c "print(hex(553649152))"`) and quote the output. Before calling a tool's number wrong, put both raw values side by side in one base, converted by a tool; a gap of one round step (0x400, a power of two) is as often a conversion slip as a bug. A finding that changes a design or a file format gets one re-check against the raw logs first.
- Keep Wii keys private. `keys/` and `.venv/` are ignored. Check staged files before any commit.
- A direct DOL launch does not provide the installed channel's NAND identity. Check the DOL and retail WAD as separate outputs.
- Commit and push work to `develop`; pushes there do not run CI (a pull request or a manual run does). `master` moves only at a release, by merging `develop`, and its pushes run CI.
- When making a commit, bump both version fields in `channel/channelapp/config.h` and `channel/title/Makefile` to the same `major.minor.patch` release in that commit.
- Versions: the patch number for each commit; the minor number (patch back to 0) for a release with a headline feature, in a commit titled "Release HBC x.y.0", tagged `vx.y.0`. The WAD's title version packs `major << 11 | minor << 5 | patch`, so a patch number stops at 31.
- Text files are LF in the repo and in every checkout: `.gitattributes` sets `* text=auto eol=lf`, which overrides Git for Windows' `core.autocrlf=true`. Write LF. The few upstream files kept byte-for-byte with CRLF are marked `-text` there; keep their endings when editing them. `tests/test_line_endings.py` checks it, and `tools/fix_line_endings.py` converts a checkout made before 1.8.3.
- Prefer a focused fix with a matching check. Record larger ideas in `REVIEW.md` with evidence and a clear trigger.
