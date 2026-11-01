"""Per-event instruction costs of linked mode: profile kinds (cats.py) of a
psample run divided by that run's event counts (per-compile stats).
usage: icosts.py psum-dir result-dir proj..."""
import json, os, re, sys
pdir, rdir = sys.argv[1], sys.argv[2]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'perf', 'cats.py')).read().split("order = ")[0]
exec(src)
def kinds(path):
    tot = {}
    for line in open(path):
        if line.startswith('samples:'):
            sums = [int(x) for x in line.split()[1:]]; continue
        m = re.match(r'\s*(\d+)\s+(\d+)\s+[+-]\d+\s+(.*)', line)
        if not m: continue
        k = next((k for k, r in R if r.search(m[3])), 'rest')
        a = tot.setdefault(k, [0, 0]); a[0] += int(m[1]); a[1] += int(m[2])
    return sums, tot
hl = re.compile(r'header cache: (\d+) hits / (\d+) lookups')
lk = re.compile(r'header cache links: (\d+) loaded')
ex = re.compile(r'exported (\d+) parts')
out = {}
print(f"{'project':9s} {'lookups':>8s} {'check M/lookup':>14s} {'copies':>8s} {'load M/copy':>11s} {'parts':>6s} {'write M/part':>12s} {'plain G':>8s} {'link G':>7s}")
for p in sys.argv[3:]:
    sums, tot = kinds(f'{pdir}/{p}.txt')
    J = json.load(open(f'{rdir}/{p}.json'))
    h = l = links = parts = 0
    for s in J['S']['linkc']:
        if not s: continue
        m = hl.search(s)
        if m: h += int(m[1]); l += int(m[2])
        m = lk.search(s)
        if m: links += int(m[1])
        m = ex.search(s)
        if m: parts += int(m[1])
    g = lambda k: tot.get(k, [0, 0])[1] * 1e5
    C = g('check') / max(1, l); L = (g('load') + g('replay')) / max(1, h + links); W = (g('write') + g('track')) / max(1, parts)
    out[p] = dict(C=C, L=L, W=W, lookups=l, hits=h, links=links, parts=parts, plain=sums[0] * 1e5, link=sums[1] * 1e5,
                  plain_rest=tot['rest'][0] * 1e5, link_rest=tot['rest'][1] * 1e5)
    print(f"{p:9s} {l:8d} {C/1e6:14.2f} {h+links:8d} {L/1e6:11.2f} {parts:6d} {W/1e6:12.1f} {sums[0]/1e4:8.1f} {sums[1]/1e4:7.1f}")
json.dump(out, open('icosts.json', 'w'), indent=1)
