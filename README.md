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
