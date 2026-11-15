# Rondo of Blood recomp (PC Engine Super CD-ROM2)

Static recompilation of *Akumajou Dracula X: Chi no Rondo* (Konami, 1993). Game data is never
committed: point the tools at your own disc image.

## Layout
- `harness/` - trace harness: HuC6280 interpreter (`cpu6280.c`), PCE CD machine (`pce.c`),
  System Card 3.0 HLE (`bios_hle.c`), scanline renderer (`render.c`), runner (`pcerun.c`).
- `tools/` - `dis6280.py` disassembler, disc helpers.
- `docs/` - notes and findings.

## Build / run
    gcc -O2 -o harness/pcerun harness/*.c
    harness/pcerun data.iso build/out 3600 -v -d 0:3600:120
`data.iso` = track 2 user data (2048-byte sectors) starting at INDEX 01.

## Recompiled code is generated from your disc
No game code or data is stored in this repository. `recipe/overlays.txt` lists, for each code bank the game uses,
which offsets are instructions and where those bytes are on the disc. `tools/gen_from_disc.py` reads your own
disc image (.cue + .img/.bin), checks every bank against its CRC and runs the translator (`tools/recomp.py`):

    python3 tools/gen_from_disc.py /path/to/your/game.cue        # -> harness/native/
    RONDO_DISC=/path/to/your/game.cue harness/build_player.sh windows   # does it automatically when needed

Source disc: Akumajou Dracula X: Chi no Rondo (JP), data track MD5 of the trurip set noted in docs/findings.md.
