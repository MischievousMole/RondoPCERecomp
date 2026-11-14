# HuC6280 -> C recompiler

## Overlays
Rondo loads code into Super CD RAM banks ($68-$7F) per game mode/stage, and also copies code blocks between banks
with its own CPU loops. A bank's *version* is identified by an FNV hash of its 8 KB contents (bytes listed in
`tools/volatile.txt` are zeroed for hashing: runtime-patched jump vectors and the whole RAM bank $F8).
A bank is re-hashed lazily after a BIOS bulk load or after the CPU writes over an already-executed instruction.

The trace harness records, per bank version, a snapshot (`bank_BB_HASH.bin`) and an executed-instruction bitmap
(`cov_BB_HASH.bin`). `tools/ovl_merge.py` merges runs.

## Translation (`tools/recomp.py`)
- One C function per bank version: `int N_BB_HASH(cpu6280 *c, int budget)`.
- Only executed instruction starts are translated; each becomes a case label (resume point).
- Straight-line code falls through; any taken branch / jump / call / return sets `c->pc` and goes to `top`,
  which checks: cycle budget, pending interrupts, the code window still maps this bank, and the overlay epoch.
  Anything else returns to the dispatcher (interpreter takes over for one instruction or more).
- Kept interpreted: BIOS HLE traps ($FB), BRK, bytes in volatile.txt, banks $00 (BIOS stubs) and $F8 (RAM).
- SET (T flag) is handled locally: the following ALU op is emitted in both forms.
- Block moves call the shared `cpu_block`; ALU helpers (`cpu_adc/sbc/cmp`) are shared with the interpreter.

## Verification
`PCE_NATIVE=1 PCE_DIFFTEST=N`: every Nth native call is replayed on the interpreter from a full machine snapshot;
CPU registers, RAM, CD RAM, Super CD RAM, VRAM, VDC registers and palette are compared.
2026-11-13: 3 random-play seeds x 40,000 frames, 271,692 checks, 0 failures, 99.9% of instructions native.
