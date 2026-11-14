"""Render a captured frame through shaders/rondo_video.frag (headless GL) and compare with the CPU reference."""
import sys, struct, os, time
import numpy as np, moderngl
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
shot, refprefix, outprefix = sys.argv[1], sys.argv[2], sys.argv[3]
raw = open(shot, 'rb').read()
sw, sh, dot = struct.unpack('<3i', raw[:12])
rgb = np.frombuffer(raw[12:12 + sw * sh * 4], dtype=np.uint32).reshape(sh, sw)
lay = np.frombuffer(raw[12 + sw * sh * 4:], dtype=np.uint8).reshape(sh, sw)
rgba = np.zeros((sh, sw, 4), np.uint8)
rgba[..., 0] = (rgb >> 16) & 255; rgba[..., 1] = (rgb >> 8) & 255; rgba[..., 2] = rgb & 255
rgba[..., 3] = np.where(lay == 2, 255, 0)
DW, DH = 2560, 1440
# geometry identical to video_geometry(): authentic 8:7 PAR, fill height
par = [8/7, 6/7, 4/7, 4/7][dot & 3]
sy = DH / sh; w = sw * par * sy
ow, oh = int(w + 0.5), int(sh * sy + 0.5); ox, oy = (DW - ow) // 2, (DH - oh) // 2
ctx = moderngl.create_standalone_context(backend='egl')
prog = ctx.program(vertex_shader=open(os.path.join(here, '../shaders/rondo_video.vert')).read(),
                   fragment_shader=open(os.path.join(here, '../shaders/rondo_video.frag')).read())
tex = ctx.texture((sw, sh), 4, rgba.tobytes()); tex.filter = (moderngl.NEAREST, moderngl.NEAREST); tex.use(0)
fbo = ctx.simple_framebuffer((DW, DH)); fbo.use()
vao = ctx.vertex_array(prog, [])
prog['uSrc'] = 0; prog['uSrcSize'] = (sw, sh); prog['uPicOrigin'] = (ox, oy); prog['uPicSize'] = (ow, oh)
prog['uFbHeight'] = DH; prog['uScan'] = 0.75; prog['uMask'] = 0.5
names = ['pixel', 'sharp', 'scanlines', 'crt', 'layers']
for i, n in enumerate(names):
    flt = i if i < 4 else 1
    prog['uFilter'] = flt
    prog['uBri'] = (0.85, 1.25) if n == 'layers' else (1.0, 1.0)
    prog['uCon'] = (1.0, 1.15) if n == 'layers' else (1.0, 1.0)
    if n == 'pixel':      # integer 6x geometry
        s6 = int(DH / sh); w6 = sw * par * s6
        prog['uPicSize'] = (int(w6 + 0.5), sh * s6); prog['uPicOrigin'] = ((DW - int(w6 + 0.5)) // 2, (DH - sh * s6) // 2)
    else:
        prog['uPicSize'] = (ow, oh); prog['uPicOrigin'] = (ox, oy)
    t0 = time.time(); vao.render(mode=moderngl.TRIANGLES, vertices=3); ctx.finish(); ms = (time.time() - t0) * 1000
    img = np.frombuffer(fbo.read(components=3), np.uint8).reshape(DH, DW, 3)[::-1]
    Image.fromarray(img).save(f'{outprefix}_{n}.png')
    ref = np.asarray(Image.open(f'{refprefix}_{n}.ppm').convert('RGB')).astype(int)
    d = np.abs(img.astype(int) - ref)
    mse = (d ** 2).mean(); psnr = 99 if mse == 0 else 10 * np.log10(255 ** 2 / mse)
    print(f'{n:10s} max diff {d.max():3d}  mean {d.mean():.3f}  PSNR {psnr:5.1f} dB  ({ms:.0f} ms on llvmpipe)')
