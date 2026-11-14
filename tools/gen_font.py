"""Render ASCII 32..126 into a 10x20 alpha atlas (font_gen.h) for the player menu. Font: DejaVu Sans Mono (free license)."""
import sys, os
from PIL import Image, ImageDraw, ImageFont
here = os.path.dirname(os.path.abspath(__file__))
font = ImageFont.truetype(os.path.join(here, '../assets/DejaVuSansMono-Bold.ttf'), 16)
CW, CH = 10, 20
data = []
for c in range(32, 127):
    im = Image.new('L', (CW, CH), 0)
    ImageDraw.Draw(im).text((0, 1), chr(c), font=font, fill=255)
    data.extend(im.tobytes())
with open(sys.argv[1], 'w') as f:
    f.write(f'#define FONT_W {CW}\n#define FONT_H {CH}\nstatic const unsigned char FONT[95][{CW*CH}] = {{\n')
    for i in range(95):
        f.write('{' + ','.join(str(x) for x in data[i*CW*CH:(i+1)*CW*CH]) + '},\n')
    f.write('};\n')
