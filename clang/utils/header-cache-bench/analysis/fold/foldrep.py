"""Per project and mode of a fold run: exact instructions, speedup over
plain clang, failures, the daemon's ask-list counts, and the copies hits
loaded. usage: foldrep.py results-dir proj..."""
import json, re, sys, math, collections
d = sys.argv[1]
rows = {}
for p in sys.argv[2:]:
    try: J = json.load(open(f'{d}/{p}.json'))
    except Exception as e: print(p, 'missing', e); continue
    I = J['I']; plain = sum(I['plain'])
    for m in J['modes'][1:]:
        log = J['daemon'].get(m, '')
        t = next((l for l in log.splitlines() if l.startswith('timed')), '')
        listed = re.search(r'(\d+) on the ask-list', t); floor = re.search(r'(\d+) below the fold floor', t)
        folded = re.search(r'(\d+) folded by their losses', t)
        st = next((l for l in log.splitlines() if l.startswith('stored')), '')
        stored = re.search(r'stored\s+(\d+) entries', st)
        hits = links = parts = 0
        for s in J['S'][m]:
            a = re.search(r'header cache: (\d+) hits', s); hits += int(a[1]) if a else 0
            b = re.search(r'header cache links: (\d+) loaded', s); links += int(b[1]) if b else 0
            c = re.search(r'exported (\d+) parts', s); parts += int(c[1]) if c else 0
        nf = sum(1 for f in J['failed'] if f[0] == m)
        rows[(p, m)] = dict(G=sum(I[m]) / 1e9, x=plain / sum(I[m]), failed=nf, listed=int(listed[1]) if listed else 0,
                            floor=int(floor[1]) if floor else 0, folded=int(folded[1]) if folded else 0,
                            stored=int(stored[1]) if stored else 0, hits=hits, links=links, parts=parts, plainG=plain / 1e9)
projs = [p for p in sys.argv[2:] if any(k[0] == p for k in rows)]
modes = sorted({k[1] for k in rows}, key=lambda m: list(json.load(open(f'{d}/{projs[0]}.json'))['modes']).index(m) if m in json.load(open(f'{d}/{projs[0]}.json'))['modes'] else 99)
print('speedup over plain clang, instructions (G instructions in brackets)')
print(f"{'mode':14s} " + ' '.join(f'{p:>16s}' for p in projs) + '   geomean   worst')
for m in modes:
    xs = [rows[(p, m)]['x'] for p in projs if (p, m) in rows]
    print(f'{m:14s} ' + ' '.join(f"{rows[(p, m)]['x']:6.2f}x ({rows[(p, m)]['G']:6.1f})" if (p, m) in rows else ' ' * 16 for p in projs)
          + f'   {math.exp(sum(map(math.log, xs)) / len(xs)):6.2f}x  {min(xs):5.2f}x')
print(f"{'plain G':14s} " + ' '.join(f"{rows[(p, modes[0])]['plainG']:16.1f}" for p in projs))
for key in ('failed', 'listed', 'floor', 'folded', 'stored', 'parts', 'hits', 'links'):
    print(f'-- {key}')
    for m in modes:
        print(f'{m:14s} ' + ' '.join(f"{rows[(p, m)][key]:16d}" if (p, m) in rows else ' ' * 16 for p in projs))
