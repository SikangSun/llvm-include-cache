"""Call-stack profiles from psample with PSAMPLE_STACK=1.
usage: pstack.py BINARY 'glob-A' ['glob-B'] [--inclusive N] [--callers SUBSTR] [--depth K]
With two globs, columns are A, B and B-A."""
import argparse, collections, glob, subprocess
ap = argparse.ArgumentParser()
ap.add_argument('binary'); ap.add_argument('samples', nargs='+')
ap.add_argument('--inclusive', type=int, default=0)
ap.add_argument('--callers', action='append', default=[])
ap.add_argument('--depth', type=int, default=1)
ap.add_argument('--top', type=int, default=25)
ap.add_argument('--base', default='555555554000')
a = ap.parse_args()
base = int(a.base, 16)
runs = []
addrs = set()
maps = {}
for spec in a.samples:
    S = []
    for path in sorted(glob.glob(spec)):
        for line in open(path):
            if line.startswith('M '):
                _, start, length, pgoff, fname = line.rstrip('\n').split(' ', 4)
                maps[(int(start, 16), int(length, 16), int(pgoff, 16))] = fname
                continue
            f = [int(x, 16) for x in line.split()]
            if f:
                S.append(f); addrs.update(f)
    runs.append(S)
names = {}
inside = sorted(x for x in addrs if base <= x < base + (1 << 30))
if inside:
    p = subprocess.run(['addr2line', '-f', '-C', '-e', a.binary], input='\n'.join(hex(x - base) for x in inside) + '\n', capture_output=True, text=True)
    L = p.stdout.split('\n')
    for k, x in enumerate(inside):
        n = L[2 * k] if 2 * k < len(L) else '??'
        names[x] = n.split('(')[0] if '(' in n and not n.startswith('(') else n
bylib = collections.defaultdict(list)
for x in addrs:
    if x in names: continue
    for (st, ln, pg), fn in maps.items():
        if st <= x < st + ln and fn.startswith('/'):
            bylib[fn].append((x, x - st + pg)); break
for fn, xs in bylib.items():
    p = subprocess.run(['addr2line', '-f', '-C', '-e', fn], input='\n'.join(hex(o) for _, o in xs) + '\n', capture_output=True, text=True)
    L = p.stdout.split('\n'); short = fn.rsplit('/', 1)[-1]
    for k, (x, _) in enumerate(xs):
        n = L[2 * k] if 2 * k < len(L) else '??'
        names[x] = short + ':' + (n.split('(')[0] if '(' in n and not n.startswith('(') else n)
def nm(x): return names.get(x, '[outside]')[:120]
def show(title, counters):
    tot = collections.Counter()
    for c in counters: tot.update(c)
    key = (lambda k: counters[-1][k] - counters[0][k]) if len(counters) > 1 else (lambda k: counters[0][k])
    print(title)
    for k in sorted(tot, key=key, reverse=True)[:a.top]:
        print('  ' + '  '.join(f'{c[k]:7d}' for c in counters) + (f'  {counters[-1][k]-counters[0][k]:+7d}' if len(counters) > 1 else '') + '  ' + k)
print('samples: ' + '  '.join(str(len(S)) for S in runs))
if a.inclusive:
    cs = []
    for S in runs:
        c = collections.Counter()
        for f in S:
            c.update(set(nm(x) for x in f))
        cs.append(c)
    a.top = a.inclusive
    show('inclusive (sample counted once per function on its stack):', cs)
for sub in a.callers:
    cs = []
    for S in runs:
        c = collections.Counter()
        for f in S:
            fr = [nm(x) for x in f]
            # the innermost frame that matches, and the K frames above it that differ
            for i, n in enumerate(fr):
                if sub in n:
                    up = [m for m in fr[i + 1:] if sub not in m][:a.depth]
                    c[' <- '.join(up) if up else '(no caller)'] += 1
                    break
        cs.append(c)
    show(f'callers of {sub}:', cs)
