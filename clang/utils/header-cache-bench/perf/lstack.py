"""Resolves psample call-stack samples (PSAMPLE_STACK=1) once, into a
Counter of stacks of function names, innermost first, for quick analysis.
The kernel caps samples at perf_event_max_sample_rate (10,000 a second
here), so a period of 100,000 instructions keeps about half of them: the
shares hold, the totals do not; take totals from perfc.
usage: lstack.py BINARY 'samples-glob' out.pkl"""
import collections, glob, pickle, subprocess, sys
binary, spec, out = sys.argv[1:4]
base = 0x555555554000
maps = {}
stacks = collections.Counter()
for path in sorted(glob.glob(spec)):
    for line in open(path):
        if line.startswith('M '):
            _, start, length, pgoff, fname = line.rstrip('\n').split(' ', 4)
            maps[(int(start, 16), int(length, 16), int(pgoff, 16))] = fname
            continue
        f = tuple(int(x, 16) for x in line.split())
        if f:
            stacks[f] += 1
addrs = set(x for s in stacks for x in s)
names = {}
inside = sorted(x for x in addrs if base <= x < base + (1 << 30))
p = subprocess.run(['addr2line', '-f', '-C', '-e', binary], input='\n'.join(hex(x - base) for x in inside) + '\n',
                   capture_output=True, text=True)
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
    p = subprocess.run(['addr2line', '-f', '-C', '-e', fn], input='\n'.join(hex(o) for _, o in xs) + '\n',
                       capture_output=True, text=True)
    L = p.stdout.split('\n'); short = fn.rsplit('/', 1)[-1]
    for k, (x, _) in enumerate(xs):
        n = L[2 * k] if 2 * k < len(L) else '??'
        names[x] = short + ':' + (n.split('(')[0] if '(' in n and not n.startswith('(') else n)
named = collections.Counter()
for s, c in stacks.items():
    named[tuple(names.get(x, '[outside]') for x in s)] += c
pickle.dump(named, open(out, 'wb'))
print(out, sum(named.values()), 'samples', len(named), 'stacks')
