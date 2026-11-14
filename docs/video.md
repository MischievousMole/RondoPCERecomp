# Video output (harness/video.c)

Target: 2560x1440 at 120 Hz, authentic look.

## Geometry
- Source: 256x224/225 (VDW), dot clock from VCE ctrl -> pixel aspect 8:7 (256 px), 6:7 (336 px), 4:7 (512 px).
- Fill height (default): 6.43x -> 1872x1440, pillarboxed. Integer: 6x -> 1755x1350 (borders top/bottom).
- Aspect: Authentic (PAR from dot clock), Square pixels, Stretch 4:3.

## Filters
- PIXEL: nearest neighbour (use with Integer).
- SHARP: sharp bilinear (one output pixel of blend at each source edge): crisp, no shimmer at non-integer scale.
- SCANLINES: 3-tap gaussian beams in linear light, spot width grows with brightness (0.20-0.36 line), energy-normalised.
- CRT: scanlines + softer horizontal + RGB aperture grille (3 px) with gain compensation.
- Per-layer brightness/contrast: [0] backdrop + BG tiles, [1] sprites (gamma space, before linearising).

## 120 Hz
Game runs at 59.83 Hz. Presentation modes (player): 60 Hz; 120 Hz repeat (each frame shown twice, exact pacing);
120 Hz black frame insertion (frame, black) for CRT-like motion clarity. VRR supported when available.
No frame interpolation (would invent frames the game never drew).

## Cost (CPU, multithreaded rows)
2-core container: sharp 13.6 ms, scanlines 17.4 ms, CRT 24.3 ms per frame at 1440p; single thread CRT 33 ms.
Expected ~5-8 ms on an 8+ thread desktop. A GPU shader path can replace this later.

## GPU path (shaders/rondo_video.vert/.frag) - 2026-11-14
- GLSL 3.30 core, one pass, full-screen triangle; source frame as RGBA8 texture (alpha = sprite layer), texelFetch only.
- Same math as the CPU reference; tools/shader_test.py renders it headless (Mesa EGL via moderngl) and compares:
  pixel bit-exact; sharp/scanlines/CRT/layers max 6/255, PSNR 52-69 dB (CPU uses 32-level beam and 4096-entry LUTs).
- The player will use this path; the CPU scaler stays as reference/fallback.
- "1440p" = picture scaled to the full 1440 height at the original aspect (1872x1440), black bars on the sides.
