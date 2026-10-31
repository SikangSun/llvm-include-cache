"""Per compile: model header time (trace reads x median own time) vs the
measured plain compile time and, where the COUNTER=1 compile served nothing,
its measured text time."""
import os, sys, re, json, statistics, collections
proj, tdir = sys.argv[1], sys.argv[2]
def norm(p):
    p = p.replace('/home/sikang/c/benchmarks/', '').replace('/dev/shm/bench-src/', '')
    return re.sub(r'.*/lib/clang/\d+/include/', 'clang-include/', p)
J = json.load(open(f'/home/sikang/c/code/scratch/e2e/multi/results-2026-10-30/lay/{proj}.json'))
own = collections.defaultdict(list); first = collections.defaultdict(list)
for s in J['S']['counter']:
    seen = set()
    for x in re.finditer(r'\n  text (\S+) (\d+) depth (\d+) own (\d+)', s):
        k = norm(x[1]); own[k].append(int(x[4]))
        if k not in seen: first[k].append(int(x[4])); seen.add(k)
med = {k: statistics.median(v) for k, v in own.items()}
medf = {k: statistics.median(v) for k, v in first.items()}
name2i = {c: i for i, c in enumerate(J['cmds'])}
rows = []
for fn in sorted(os.listdir(tdir)):
    if not fn.endswith('.ddg'): continue
    lines = open(os.path.join(tdir, fn), errors='replace').read().split('\n')
    c = lines[0].split('\t')[1] if lines[0].startswith('C\t') else None
    key = next((k for k in name2i if c and (c.endswith('/' + k) or c == k or k.endswith(c.lstrip('./')))), None)
    if key is None: continue
    i = name2i[key]
    reads = [l.split('\t')[1] for l in lines if l.startswith('R\t')][1:]
    m = sum(med.get(norm(p), 0) for p in reads)
    s = J['S']['counter'][i]
    hits = int(re.search(r'(\d+) hits', s)[1])
    text = float(re.search(r'text time: ([\d.]+) ms', s)[1])
    rows.append((key, J['T']['plain'][i], J['T']['counter'][i], m / 1e3, hits, text, len(reads), sum(1 for p in reads if norm(p) in med)))
print(proj, 'matched', len(rows))
r0 = [r for r in rows if r[4] == 0]
print('compiles with no hits:', len(r0))
for r in r0[:8]: print('  %-30s plain %7.1f ms counter %7.1f  model header %7.1f  measured text %7.1f  reads %d (timed %d)' % r[:4] + r[5:6] + r[6:8] if False else '  %-28s plain %7.1f counter %7.1f model %7.1f text %7.1f reads %d timed %d' % (r[0][-28:], r[1], r[2], r[3], r[5], r[6], r[7]))
print('sum model/plain over all: %.2f' % (sum(r[3] for r in rows) / sum(r[1] for r in rows)))
if r0: print('no-hit compiles: model/text %.2f, text/counter %.2f' % (sum(r[3] for r in r0) / sum(r[5] for r in r0), sum(r[5] for r in r0) / sum(r[2] for r in r0)))
