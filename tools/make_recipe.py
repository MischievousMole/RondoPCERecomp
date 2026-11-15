"""Build recipe/overlays.txt: for every translated bank version, which offsets are instructions and where
each run of instruction bytes can be read from the disc's data track. Contains no game bytes.
Usage: make_recipe.py OVL_DIR DATA_ISO OUT"""
import sys, os, glob, zlib
sys.path.insert(0, os.path.dirname(__file__))
from dis6280 import OP, M
ovl_dir, iso, out = sys.argv[1:4]
disc = open(iso, 'rb').read()
lines = []; nseg = 0; missing = 0
for covf in sorted(glob.glob(os.path.join(ovl_dir, 'cov_*.bin'))):
    key = os.path.basename(covf)[4:-4]; bank = int(key[:2], 16)
    if bank in (0x00, 0xF8): continue
    code = open(os.path.join(ovl_dir, f'bank_{key}.bin'), 'rb').read(); cov = open(covf, 'rb').read()
    need = bytearray(0x2000)
    for i in range(0x2000):
        if cov[i >> 3] >> (i & 7) & 1:
            n = 1 + M[OP.get(code[i], ('NOP', 'imp'))[1]][0]
            for k in range(i, min(i + n, 0x2000)): need[k] = 1
    # runs of needed bytes
    runs = []; i = 0
    while i < 0x2000:
        if need[i]:
            j = i
            while j < 0x2000 and need[j]: j += 1
            runs.append((i, j)); i = j
        else: i += 1
    segs = []
    for a, b in runs:
        pos = disc.find(code[a:b])
        if pos < 0:   # split the run until every piece is found
            stack = [(a, b)]
            while stack:
                x, y = stack.pop()
                p = disc.find(code[x:y])
                if p >= 0: segs.append((x, y - x, p))
                elif y - x > 1: m = (x + y) // 2; stack += [(m, y), (x, m)]
                else: missing += 1
        else: segs.append((a, b - a, pos))
    segs.sort()
    crc = zlib.crc32(bytes(code[i] if need[i] else 0 for i in range(0x2000)))
    nseg += len(segs)
    lines.append(f'O {key} {crc:08x} {cov.hex()}')
    lines += [f'S {o:04x} {n} {p:x}' for o, n, p in segs]
open(out, 'w').write('# Rondo recomp overlay recipe: O bank_hash crc32(code bytes) coverage-bitmap / S bank_offset length disc_offset\n' + '\n'.join(lines) + '\n')
print(f'{len([l for l in lines if l[0]=="O"])} overlays, {nseg} segments, {missing} bytes not found on disc')
