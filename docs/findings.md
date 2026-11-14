# Findings

## Disc (JP, trurip, MD5 d53b06ce6cb60ae5891806904dced7f6)
- 22 tracks: 1 audio (warning), **2 data** (10,149 sectors), 3-21 audio (CD-DA music/voice), 22 data = exact copy of track 2.
- Track 2 INDEX 01 = absolute LBA 3890 ($F32). Game tables store absolute LBAs and subtract $000F32 before CD_READ.
- IPL (LBA 1): load 1 sector from LBA 2 to $3000, MPR2-6 = $80-$84, jump $3000.
- Loader: CD_BASE x2, EX_GETVER; System Card 3 -> EX_MEMOPEN (Super CD RAM base bank $68 in $DB),
  then a 5-byte-per-entry directory at $312F drives CD_READ into SCD RAM banks. Main program at bank $69 ($4000).

## Runtime structure
- Game installs its own VDC IRQ ($2202 -> $4177) and timer IRQ ($2204 -> $4297, RTI only); irq_m = $06.
  The whole game runs from the VDC interrupt; the main thread is `JMP $4174` forever.
- IRQ1 handler reads VDC status: RR (raster) path first via jump table at $44E9 indexed by $3F31, then VD (vblank).
- Joypad via EX_JOYSNS each frame; game reads joytrg ($222D) and joy ($2228).
- Backup RAM: one entry "X DRACULA X" (12-byte name at $6C9A), 144 bytes at $7D00.
- ADPCM RAM (64 KB) is used as extra storage: AD_TRANS (CD->ADPCM), AD_READ (ADPCM->CPU, 16 KB chunks) for stage data.
- Konami logo, intro, title, file select, name entry, slot menu and Stage 0 (Prologue) all run on the HLE BIOS.

## BIOS calls used (static scan of track 2)
cd_read, cd_pause, cd_fade, cd_reset, cd_base, cd_subq, cd_exec, cd_seek, cd_stat, cd_play, cd_search, cd_contnts,
ad_reset, ad_stop, ad_read, ad_cplay, ad_stat, ad_trans, ad_play, bm_read/write/delete/free/format,
ex_getver, ex_memopen, ex_rcroff/rcron, ex_getfnt (kanji font, needs real ROM), ex_setrcr, ex_sprdma, ex_vsync,
ex_scrsiz, ex_irqon, ex_dspoff/dspon, ex_satclr, ex_setvec, ex_setred/setwrt/setdma, ex_bgon, ex_spron, ex_joysns,
ex_rnd, ex_scrmod, ex_imode, ex_colorcmd, ex_dotmod, ex_bcdbin, ma_mul8u/mul8s/div16s/sqrt/sin, psg_driver, grp_bios.

## Open items
- ex_getfnt: prologue text uses the System Card kanji font -> needs the real BIOS ROM (or a font dump).
- Raster split a few lines off in the carriage cutscene (VDC timing / BYR latch).
- CD-DA and ADPCM audio not rendered yet (calls are logged).

## Save record ("X DRACULA X", 144 bytes) - slot 1 layout found so far
- +$01 slot used; +$15..$1C name (char codes, "A" = $21).
- +$02..$05 and +$07..$0A: stage progress (main / alternate route). Setting one of them selects the stage
  Game Start loads (e.g. $02=1 -> castle bridge, music track 9; $02=2 -> track 10; $03=1 -> 13; $03=2 -> 14).
- +$1F..$21 money (BCD). +$24 = 01 after the Prologue.
- Coverage warps: copy tools/save_template.sav into OUT/bram, patch one byte, route "7600:08,7606:00,7900:01,7904:00,8100:01,8104:00".

## Coverage 2026-11-13
65 bank versions, 41,695 executed instruction addresses (Stage 0-1 random play + 16 stage-save warps).
Difftest on 8 stage saves x 30,000 frames: 423,762 checks, 0 failures, 99.9-100% native.

## Invincibility / boss coverage 2026-11-13
- Player health: $2098 (max $5C), HUD copy $2099. `PCE_POKE=8400-99999:2098=5c` keeps Richter alive.
- 8 saves x 60,000 frames invincible random play: castle interiors, skyline bridge, dungeon, cave, balcony, giant-bat boss.
- Totals: 46,929 executed instruction addresses in 65 bank versions; 46,722 translated, 0 bails.
- Difftest (same 8 saves, native, invincible): 567,589 checks, 0 failures, 99.9-100% native.
- The "raster split" in the carriage cutscene is the game's intended portrait-to-carriage wipe, not a bug.

## Later stages 2026-11-13
- Stage byte values: $02 = 1/2-3/4-6/8 -> music 9/10/11/12; $03 = 1/2-3/4-6/8 -> 13/14/15/6; $04 = 4-6 -> 16.
- Totals: 65,361 executed instruction addresses in 95 bank versions; 65,147 translated, 0 bails.
- Difftest on 8 saves x 40,000 frames (invincible): 380,228 checks, 0 failures, 100% native.
