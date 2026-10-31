"""Turns psample addresses into a flat profile by function.
usage: psum.py BINARY samples... [--top N] [--base HEX]
Each argument's samples are summed separately and shown side by side."""
import argparse, collections, subprocess

ap = argparse.ArgumentParser()
ap.add_argument('binary')
ap.add_argument('samples', nargs='+')
ap.add_argument('--top', type=int, default=40)
ap.add_argument('--base', default='555555554000')
a = ap.parse_args()
base = int(a.base, 16)

counts = []
addrs = set()
maps = {}
import glob
for spec in a.samples:
    c = collections.Counter()
    for path in sorted(glob.glob(spec)):
      for line in open(path):
        line = line.strip()
        if line.startswith('M '):
            _, start, length, pgoff, fname = line.split(' ', 4)
            maps[(int(start, 16), int(length, 16), int(pgoff, 16))] = fname
        elif line:
            c[int(line, 16)] += 1
    counts.append(c)
    addrs.update(c)

inside = sorted(x for x in addrs if base <= x < base + (1 << 30))
names = {}
if inside:
    # llvm-symbolizer-18 cannot read this binary; binutils can.
    p = subprocess.run(['addr2line', '-f', '-C', '-e', a.binary],
                       input='\n'.join(hex(x - base) for x in inside) + '\n', capture_output=True, text=True)
    lines = p.stdout.split('\n')
    for k, x in enumerate(inside):
        n = lines[2 * k] if 2 * k < len(lines) else '??'
        names[x] = n.split('(')[0] if '(' in n and not n.startswith('(') else n

# Library addresses: by the mapping each falls in, named with addr2line on
# the library (a text segment's file offset equals its address there).
outside = sorted(x for x in addrs if x not in names)
bylib = collections.defaultdict(list)
for x in outside:
    for (start, length, pgoff), fname in maps.items():
        if start <= x < start + length and fname.startswith('/'):
            bylib[fname].append((x, x - start + pgoff))
            break
for fname, xs in bylib.items():
    p = subprocess.run(['addr2line', '-f', '-C', '-e', fname], input='\n'.join(hex(o) for _, o in xs) + '\n',
                       capture_output=True, text=True)
    lines = p.stdout.split('\n')
    short = fname.rsplit('/', 1)[-1]
    for k, (x, _) in enumerate(xs):
        n = lines[2 * k] if 2 * k < len(lines) else '??'
        names[x] = short + ':' + (n.split('(')[0] if '(' in n and not n.startswith('(') else n)

per = []
for c in counts:
    f = collections.Counter()
    for x, n in c.items():
        f[names.get(x, '[outside the binary]')] += n
    per.append(f)
total = collections.Counter()
for f in per:
    total.update(f)
# Sort by the largest difference between the last and the first sample set.
key = (lambda k: per[-1][k] - per[0][k]) if len(per) > 1 else (lambda k: per[0][k])
sums = [sum(f.values()) for f in per]
print('samples: ' + '  '.join(f'{s}' for s in sums))
for k in sorted(total, key=key, reverse=True)[:a.top]:
    print('  '.join(f'{f[k]:7d}' for f in per) + ('  ' + f'{per[-1][k]-per[0][k]:+7d}' if len(per) > 1 else '') + '  ' + k[:110])
