# Rondo of Blood recomp (PC Engine Super CD-ROM²)

Static recompilation of *Akumajou Dracula X: Chi no Rondo* (Konami, 1993, JP) to native PC code.

**No game code or data is in this repository.** The recompiled code is generated from your own disc image at build time.
You need:
- your own disc image of the JP release (`.cue` + `.img`/`.bin`, with audio tracks)
- your own System Card 3.0 ROM dump (`syscard3.pce`)

## Features
- Native recompiled CPU code (one C function per code bank / overlay), checked against a reference interpreter
- PC Engine CD hardware model: VDC, VCE, PSG, ADPCM, CD-DA, System Card 3.0 (hybrid HLE + your ROM)
- Original game speed: VRR clock at the PCE's 59.826 Hz, or vsync-locked
- 120 Hz presentation (frame repeat or black-frame insertion)
- Authentic upscaling: 8:7 pixel aspect, pillarboxed, fill-height or integer scaling (e.g. 1440p)
- GPU filters (GLSL 3.30): sharp pixels, sharp bilinear, scanlines, CRT aperture grille
- Per-layer brightness/contrast
- Launcher and pause menu: video, audio (per-channel volume), remappable keyboard/controller bindings
- Saves to host files (backup RAM)

## Building
Requirements: Python 3, GCC (Linux) or MinGW-w64 (Windows cross-build), SDL2 2.30+.

    RONDO_DISC=/path/to/your/game.cue harness/build_player.sh windows     # or: linux

The first build runs `tools/gen_from_disc.py`, which reads your disc, verifies every code bank against the CRC in
`recipe/overlays.txt`, and translates it into `harness/native/` (ignored by git). Output goes to `build/player/<target>/`.

Optional variant: `harness/build_player.sh windows santable` adds an idle screensaver (`extras/santable/`).

## Running
Put the executable in the folder with your `.cue` and `syscard3.pce`. It writes `settings.ini`, `rondo.log` and
save files beside the executable.

Default keys: arrows = d-pad, Z = I (jump), X = II (attack), Enter = Run, Right Shift = Select, Esc = menu,
F11 = fullscreen. All bindings (keyboard and controller) can be changed in the menu.

## Layout
- `harness/` - interpreter (`cpu6280.c`), machine (`pce.c`), System Card HLE (`bios_hle.c`), renderer, audio, video,
  menu, SDL2/OpenGL player (`player.c`), trace runner (`pcerun.c`)
- `tools/` - disassembler, recompiler (`recomp.py`), disc-based generator, recipe tools
- `recipe/` - per-overlay code maps and disc offsets (no game bytes)
- `shaders/` - video shaders
- `assets/` - Unscii 8 font (public domain)
- `extras/santable/` - optional screensaver (Quicksand font, SIL OFL)
- `docs/` - findings, recompiler and video notes

## Legal
This project contains no Konami code, graphics, audio or text. Akumajou Dracula X and Castlevania are trademarks of
Konami. Not affiliated with or endorsed by Konami or NEC. Use only a disc and System Card you own.
