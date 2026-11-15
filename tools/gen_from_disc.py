"""Generate the recompiled C (harness/native) from the user's own disc.
Usage: gen_from_disc.py DISC.cue|data.iso [OUT_DIR]   (default OUT_DIR: harness/native)"""
import sys, os, re, zlib, tempfile, subprocess
here = os.path.dirname(os.path.abspath(__file__))

def data_track(path):
    if not path.lower().endswith('.cue'):
        return open(path, 'rb').read()
    cue = open(path, encoding='latin-1').read()
    img = os.path.join(os.path.dirname(path), re.search(r'FILE "([^"]+)"', cue).group(1))
    tracks = []
    for t in re.finditer(r'TRACK (\d+) (\S+)(.*?)(?=TRACK|\Z)', cue, re.S):
        m = re.search(r'INDEX 01 (\d+):(\d+):(\d+)', t.group(3))
        tracks.append((t.group(2), (int(m[1]) * 60 + int(m[2])) * 75 + int(m[3])))
    total = os.path.getsize(img) // 2352
    for k, (typ, start) in enumerate(tracks):
        if typ != 'AUDIO':
            end = tracks[k + 1][1] if k + 1 < len(tracks) else total
            with open(img, 'rb') as f:
                f.seek(start * 2352); raw = f.read((end - start) * 2352)
            return b''.join(raw[i * 2352 + 16:i * 2352 + 16 + 2048] for i in range(end - start))
    sys.exit('no data track in ' + path)

disc = data_track(sys.argv[1])
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '../harness/native')
tmp = tempfile.mkdtemp()
cur = None; bad = 0; n = 0
def finish():
    global bad, n
    if cur is None: return
    key, crc, cov, img = cur
    if zlib.crc32(bytes(img)) != crc: print('CRC mismatch', key, '(different disc version?) - skipped'); bad += 1; return
    open(os.path.join(tmp, f'bank_{key}.bin'), 'wb').write(img)
    open(os.path.join(tmp, f'cov_{key}.bin'), 'wb').write(cov); n += 1
for line in open(os.path.join(here, '../recipe/overlays.txt')):
    if line[0] == 'O':
        finish(); _, key, crc, cov = line.split()
        cur = (key, int(crc, 16), bytes.fromhex(cov), bytearray(0x2000))
    elif line[0] == 'S':
        _, o, ln, p = line.split(); o, ln, p = int(o, 16), int(ln), int(p, 16)
        cur[3][o:o + ln] = disc[p:p + ln]
finish()
print(f'{n} overlays rebuilt from disc, {bad} mismatches')
if os.path.isdir(out):
    for f in os.listdir(out):
        if f.endswith('.c'): os.remove(os.path.join(out, f))
subprocess.check_call([sys.executable, os.path.join(here, 'recomp.py'), tmp, out])
