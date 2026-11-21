"""Re-hash overlay snapshots after tools/volatile.txt changes: versions that only differed in newly volatile bytes
merge into one (coverage OR'ed). Usage: rekey.py IN_OVL_DIR OUT_OVL_DIR"""
import sys, os, glob
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
vol = {}
for line in open(os.path.join(os.path.dirname(__file__), 'volatile.txt')):
    line = line.split('#')[0].strip()
    if not line: continue
    b, rest = line.split(':'); o, n = (rest.split('+') + ['1'])[:2]
    vol.setdefault(int(b, 16), set()).update(range(int(o, 16), int(o, 16) + int(n)))
def fnv(p):
    h = 2166136261
    for x in p: h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h
out = {}
for f in sorted(glob.glob(os.path.join(src, 'bank_*.bin'))):
    key = os.path.basename(f)[5:-4]; bank = int(key[:2], 16)
    code = open(f, 'rb').read(); cov = open(os.path.join(src, f'cov_{key}.bin'), 'rb').read()
    v = vol.get(bank, ())
    h = fnv(bytes(0 if i in v else code[i] for i in range(0x2000))) ^ bank
    nk = f'{bank:02X}_{h:08X}'
    if nk in out: out[nk] = (out[nk][0], bytes(x | y for x, y in zip(out[nk][1], cov)))
    else: out[nk] = (code, cov)
for k, (code, cov) in out.items():
    open(os.path.join(dst, f'bank_{k}.bin'), 'wb').write(code); open(os.path.join(dst, f'cov_{k}.bin'), 'wb').write(cov)
print(len(glob.glob(os.path.join(src, 'bank_*.bin'))), '->', len(out), 'overlay versions')
