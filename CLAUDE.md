# CLAUDE.md

SHARD XL (GPL-3.0-only), by LCFInd: a sample mangler planned for a Raspberry Pi 5 box, with a Mac test build
first. The sibling project is SHARD (LCFInd/SHARD), the FM-1 firmware; the spec is the owner's "SHARD XL" doc.

## Working with the owner

- Answer in English; code, comments, commit messages and documents are English.
- Nothing has been played on a real Mac (or Pi) unless the owner says so. Say it in the closing message of any change.
- One branch, `main`: commit on it and push (`git fetch origin` first: the owner may edit files on GitHub).
- Commit trailers: the ones given in the session's system reminder.
- Do not change the project when the owner only asks a question.

## Layout and commands

- `src/xl.h` the shared core API; `src/xl_core.c` includes engine.c, sample.c, draw.c, ui.c, project.c, app.c
  (one unit: **the order matters**). `src/plat_mac.m` the Mac (Cocoa, Core Audio, Core MIDI, ExtAudioFile);
  `src/plat_posix.c` + `src/plat_headless.c` the Linux test builds.
- `make test` (core checks + a headless render), `make shots` (every page as build/shots/page*.ppm and audio.wav),
  `make app` (macOS only). The Mac app is built by `.github/workflows/mac.yml` on every push to main and published
  to the release `mac-latest`; the sandbox has no macOS SDK, so the Mac layer is only compiled there.
- Generated, checked in, not edited by hand: `src/font_data.h` (`python3 tools/gen_font.py`),
  `assets/AppIcon.iconset/` (`python3 tools/gen_icon.py`).

## Rules that are easy to break

- The project file (project.c) stores `P_*` and `G_*` by index: append to those enums, never reorder.
- The audio thread never allocates or frees: samples are swapped with CMD_SAMPLE and freed by the UI on EV_FREE.
- New source files carry `SPDX-License-Identifier: GPL-3.0-only`.
