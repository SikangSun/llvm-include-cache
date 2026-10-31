"""Pieces made by folding upward until a piece passes the threshold, versus
today's top-level folding (header-graph traces results-2026-10-22/ddg3).

Weights: each header read's own parse time = median own time when the
COUNTER=1 run (results-2026-10-30/lay) read that file as text; files never
timed there get bytes x median us/byte.

Pieces (post-order per compile): acc = own + acc of children not cut;
a read with acc >= THRESH (depth >= 1) is cut: it roots a piece holding its
own lines and every folded read below it. Reads above no cut fold into the
.c file (always text).
Today: one copy per .c-level #include, holding the whole subtree.

Both: a root is asked only if its file was read by an earlier compile with a
first read time (subtree) >= 226 us (the ask-list). Key = hash of the
members' units in tree order (unit = file + every outside state read, with
its value). Hit: content not parsed. Miss: parsed, then written and stored.
Costs (fitted on the COUNTER=1 query lines and write lines):
  hit   = HIT_FIX + HIT_PER x parse    ask on a miss = MISS_ASK
  write = W_FIX + W_PER x parse"""
import os, sys, hashlib, collections, json, re, statistics
proj, tdir = sys.argv[1], sys.argv[2]
THRESH = float(os.environ.get('THRESH', 226))
E = os.environ.get
HIT_FIX, HIT_PER, MISS_ASK, W_FIX, W_PER = (float(E(k, d)) for k, d in (('HIT_FIX', 300), ('HIT_PER', 0.035), ('MISS_ASK', 105), ('W_FIX', 1200), ('W_PER', 0.45)))
RUN = E('RUN', '/home/sikang/c/code/scratch/e2e/multi/results-2026-10-30/lay')
def norm(p):
    p = p.replace('/home/sikang/c/benchmarks/', '').replace('/dev/shm/bench-src/', '')
    return re.sub(r'.*/lib/clang/\d+/include/', 'clang-include/', p)
own_us = collections.defaultdict(list)
J = json.load(open(f'{RUN}/{proj}.json'))
for s in J['S']['counter']:
    for x in re.finditer(r'\n  text (\S+) (\d+) depth (\d+) own (\d+)', s):
        own_us[norm(x.group(1))].append(int(x.group(4)))
med = {k: statistics.median(v) for k, v in own_us.items()}
_sz = {}
def fb(p):
    if p not in _sz:
        try: _sz[p] = os.path.getsize(p)
        except OSError: _sz[p] = 0
    return _sz[p]
files = sorted((f for f in os.listdir(tdir) if f.endswith('.ddg')), key=lambda n: int(n.split('.')[0]))
def load(fn):
    occ = []; cur = None
    for line in open(os.path.join(tdir, fn), errors='replace'):
        p = line.rstrip('\n').split('\t')
        if p[0] == 'R': cur = [p[1], p[2], p[3], int(p[4]), []]; occ.append(cur)
        elif p[0] == 'E' and cur is not None: cur[4].append('\t'.join((p[2], p[3], p[6] if len(p) > 6 else '')))
    return occ
# us per byte for untimed files: one pass over the first 50 traces
ratios = []
for fn in files[:50]:
    for o in load(fn):
        k = norm(o[0])
        if k in med and fb(o[0]) > 0: ratios.append(med[k] / fb(o[0]))
upb = statistics.median(ratios)
def H(b): return hashlib.blake2b(b, digest_size=16).digest()
class Pol:
    def __init__(s, name):
        s.name = name; s.stored = set(); s.unit_stored = collections.Counter()
        s.lookups = s.hits = s.writes = 0
        s.served = s.parsed_listed = s.written_t = s.written_b = 0.0
        s.hitcost = s.askcost = s.wcost = 0.0; s.unlisted_t = 0.0; s.below_t = 0.0
        s.oid_hits = 0; s.rep_text = 0.0
    def visit(s, key, oid, t, b, listed, members_units, members_t):
        if not listed:
            s.unlisted_t += t; return False
        s.lookups += 1
        if key in s.stored:
            s.hits += 1; s.served += t; s.hitcost += HIT_FIX + HIT_PER * t
            return True
        else:
            s.askcost += MISS_ASK; s.parsed_listed += t
            s.stored.add(key); s.writes += 1; s.written_t += t; s.written_b += b
            s.wcost += W_FIX + W_PER * t
            for u, ut in zip(members_units, members_t): s.unit_stored[u] += 1
            return False
first_time = {}       # path -> first read subtree time
listed_paths = set()
today, pieces = Pol('today'), Pol('pieces')
seen_units = {}       # unit -> (time, bytes) : distinct content ever parsed
tot_t = tot_b = 0.0; nreads = 0; npieces_per = []
for fn in files:
    occ = load(fn)
    if not occ: continue
    n = len(occ); dep = [0] * n; kids = collections.defaultdict(list); st = []; roots = []
    for i, o in enumerate(occ):
        if o[3] >= (1 << 63): st = [i]; continue
        while st and occ[st[-1]][0] != o[1]: st.pop()
        if st:
            j = st[-1]; dep[i] = dep[j] + 1; kids[j].append(i)
            if dep[i] == 1: roots.append(i)
        st.append(i)
    t = [0.0] * n; b = [0] * n
    for i, o in enumerate(occ):
        if dep[i] == 0: continue
        b[i] = fb(o[0]); k = norm(o[0])
        t[i] = med[k] if k in med else b[i] * upb
    U = [H((o[0] + '\0' + '\0'.join(sorted(o[4]))).encode()) for o in occ]
    sub_t = [0.0] * n
    for i in range(n - 1, -1, -1): sub_t[i] = t[i] + sum(sub_t[c] for c in kids[i])
    seen_before = set(seen_units)
    for i in range(n):
        if dep[i] > 0:
            tot_t += t[i]; tot_b += b[i]; nreads += 1
            if U[i] not in seen_units: seen_units[U[i]] = (t[i], b[i])
    # today: one copy per .c-level include, whole subtree
    def members(r, stop):
        out = []; q = [(r, 0)]
        while q:
            x, d = q.pop(); out.append((x, d))
            for c in reversed(kids[x]):
                if c not in stop: q.append((c, d + 1))
        return out
    served_today = set(); served_pieces = set()
    for r in roots:
        m = members(r, ())
        key = H(b''.join(U[x] + bytes([min(d, 255)]) for x, d in m))
        if today.visit(key, occ[r][2], sub_t[r], sum(b[x] for x, _ in m), occ[r][0] in listed_paths,
                    [U[x] for x, _ in m], [t[x] for x, _ in m]):
            served_today.update(x for x, _ in m)
    # pieces: fold upward until acc >= THRESH
    acc = [0.0] * n; cut = [False] * n
    for i in range(n - 1, 0, -1):
        if dep[i] == 0: continue
        acc[i] = t[i] + sum(acc[c] for c in kids[i] if not cut[c])
        cut[i] = acc[i] >= THRESH
    cutset = {i for i in range(n) if cut[i]}
    npieces_per.append(len(cutset))
    for i in range(n):
        if dep[i] == 0 and i not in cutset:
            pieces.below_t += acc_root if (acc_root := sum(acc[c] for c in kids[i] if not cut[c])) else 0
    for r in sorted(cutset):
        m = members(r, cutset)
        key = H(b''.join(U[x] + bytes([min(d, 255)]) for x, d in m))
        if pieces.visit(key, None, acc[r], sum(b[x] for x, _ in m), occ[r][0] in listed_paths,
                     [U[x] for x, _ in m], [t[x] for x, _ in m]):
            served_pieces.update(x for x, _ in m)
    for pol, sv in ((today, served_today), (pieces, served_pieces)):
        pol.rep_text += sum(t[i] for i in range(n) if dep[i] > 0 and i not in sv and U[i] in seen_before)
    for r in roots:
        if sub_t[r] < 226: today.below_t += sub_t[r]
    # ask-list: first read time of each path (subtree), this compile's reads
    for i in range(n):
        if dep[i] > 0 and occ[i][0] not in first_time:
            first_time[occ[i][0]] = sub_t[i]
            if sub_t[i] >= 226: listed_paths.add(occ[i][0])
uniq_t = sum(v[0] for v in seen_units.values()); uniq_b = sum(v[1] for v in seen_units.values())
out = {'proj': proj, 'compiles': len(npieces_per), 'header_time_s': round(tot_t / 1e6, 2), 'unique_units': len(seen_units),
       'unique_time_s': round(uniq_t / 1e6, 2), 'unique_MB': round(uniq_b / 1e6, 1),
       'pieces_per_compile': round(statistics.mean(npieces_per), 1)}
for p in (today, pieces):
    out[p.name] = dict(lookups=p.lookups, hits=p.hits, hit_pct=round(100 * p.hits / max(p.lookups, 1), 1),
        served_pct=round(100 * p.served / tot_t, 1), text_pct=round(100 * (tot_t - p.served) / tot_t, 1),
        writes=p.writes, written_s=round(p.written_t / 1e6, 2), stored_MB=round(p.written_b / 1e6, 1),
        stored_vs_unique_time=round(p.written_t / uniq_t, 2), stored_vs_unique_bytes=round(p.written_b / uniq_b, 2),
        saved_s=round(p.served / 1e6, 2), hitcost_s=round(p.hitcost / 1e6, 2), askcost_s=round(p.askcost / 1e6, 2),
        writecost_s=round(p.wcost / 1e6, 2),
        stored_copies_per_unit=round(sum(c * seen_units[u][0] for u, c in p.unit_stored.items()) / max(sum(seen_units[u][0] for u in p.unit_stored), 1), 2),
        repeated_text_s=round(p.rep_text / 1e6, 2), repeated_text_pct=round(100 * p.rep_text / tot_t, 1),
        net_s=round((p.served - p.hitcost - p.askcost - p.wcost) / 1e6, 2), unlisted_s=round(p.unlisted_t / 1e6, 2))
print(json.dumps(out))
