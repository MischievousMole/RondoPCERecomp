"""Merge per-overlay coverage from several trace runs into build/ovl_all/."""
import sys, os, glob, shutil
out = sys.argv[1]; runs = sys.argv[2:]
os.makedirs(out, exist_ok=True)
cov = {}
for r in runs:
    for f in glob.glob(os.path.join(r, 'ovl', 'cov_*.bin')):
        key = os.path.basename(f)[4:-4]
        b = open(f, 'rb').read()
        cov[key] = bytes(x | y for x, y in zip(cov.get(key, bytes(1024)), b))
        src = os.path.join(r, 'ovl', f'bank_{key}.bin')
        if not os.path.exists(os.path.join(out, f'bank_{key}.bin')): shutil.copy(src, out)
smc = set()
for r in runs:
    f = os.path.join(r, 'ovl', 'smc.txt')
    if os.path.exists(f): smc |= set(l.strip() for l in open(f) if l.strip())
if smc: open(os.path.join(out, 'smc.txt'), 'w').write('\n'.join(sorted(smc)) + '\n')
tot = 0
for key in sorted(cov):
    open(os.path.join(out, f'cov_{key}.bin'), 'wb').write(cov[key])
    n = sum(bin(x).count('1') for x in cov[key]); tot += n
    print(key, n)
print('total executed instruction addresses', tot, 'overlays', len(cov))
