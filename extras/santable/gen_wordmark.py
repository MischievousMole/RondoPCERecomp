"""Render the 'santable.ai' wordmark (Quicksand) into an alpha bitmap header for the screensaver build."""
import sys, os
from PIL import Image, ImageDraw, ImageFont
here = os.path.dirname(os.path.abspath(__file__))
font = ImageFont.truetype(os.path.join(here, 'Quicksand.ttf'), 150)
try: font.set_variation_by_axes([500])
except Exception: pass
W, H = 1024, 224
im = Image.new('L', (W, H), 0); d = ImageDraw.Draw(im)
bb = d.textbbox((0, 0), 'santable.ai', font=font)
d.text(((W - (bb[2] - bb[0])) // 2 - bb[0], (H - (bb[3] - bb[1])) // 2 - bb[1]), 'santable.ai', font=font, fill=255)
data = im.tobytes()
with open(sys.argv[1], 'w') as f:
    f.write(f'#define WORD_W {W}\n#define WORD_H {H}\nstatic const unsigned char WORDMARK[{W*H}] = {{')
    f.write(','.join(map(str, data))); f.write('};\n')
