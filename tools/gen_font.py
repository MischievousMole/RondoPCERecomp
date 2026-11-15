"""assets/unscii-8.txt -> font_gen.h (8x8 glyphs, 0/255 alpha) for the player menu."""
import sys, os
here = os.path.dirname(os.path.abspath(__file__))
rows = {}
for line in open(os.path.join(here, '../assets/unscii-8.txt')):
    if line.startswith('#') or not line.strip(): continue
    p = line.split(); rows[int(p[0], 16)] = [int(x, 16) for x in p[1:9]]
with open(sys.argv[1], 'w') as f:
    f.write('#define FONT_W 8\n#define FONT_H 8\nstatic const unsigned char FONT[95][64] = {\n')
    for c in range(32, 127):
        px = [255 if rows[c][y] >> (7 - x) & 1 else 0 for y in range(8) for x in range(8)]
        f.write('{' + ','.join(map(str, px)) + '},\n')
    f.write('};\n')
