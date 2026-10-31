"""Linked-unit design, simulated on the header-graph traces (ddg3).
Weights: median own read time per file in the quiet COUNTER=1 run (RUN),
scaled by K so the header total matches plain clang's measured header share.
Units: fold bottom-up until own + folded time >= THRESH; that read roots a
unit (its own lines + the folded reads below it). Unit key = its members'
units (file + every outside value read) in order.
Asks happen only at units whose parent unit was read as text.
  free:    a unit fits if any stored unit has its key (children may differ);
           a request is served if every unit below it fits, else its own
           lines are read as text and its child units are asked
  partial: one ask per top unit; each fitting unit below is loaded, the
           rest read as text
  tied:    a unit's payload works only with the children it was written
           with: served if this exact combination was stored before
  merged:  free, and a combination served twice becomes one flat copy
  today:   one copy per .c-level include, whole subtree, ask-list rule
Writes become visible to later compiles."""
import os, sys, hashlib, collections, json, re, statistics
proj, tdir = sys.argv[1], sys.argv[2]
E = os.environ.get
THRESH, K, RUN = float(E('THRESH', 226)), float(E('K', 1)), E('RUN')
def norm(p):
    p = p.replace('/home/sikang/c/benchmarks/', '').replace('/dev/shm/bench-src/', '')
    return re.sub(r'.*/lib/clang/\d+/include/', 'clang-include/', p)
own_us = collections.defaultdict(list)
for s in json.load(open(f'{RUN}/{proj}.json'))['S']['counter']:
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
ratios = [med[norm(o[0])] / fb(o[0]) for fn in files[:50] for o in load(fn) if norm(o[0]) in med and fb(o[0]) > 0]
upb = statistics.median(ratios)
def H(b): return hashlib.blake2b(b, digest_size=16).digest()
stored_units, stored_combo, today_stored, merged = set(), set(), set(), set()
combo_count = collections.Counter(); tied_copies = collections.Counter()
first_time, listed = {}, set()
R = {v: collections.Counter() for v in ('today', 'free', 'partial', 'tied', 'merged')}
ncomp = 0; tot_t = 0.0; units_per = []
for fn in files:
    occ = load(fn)
    if not occ: continue
    ncomp += 1
    n = len(occ); dep = [0] * n; par = [-1] * n; kids = collections.defaultdict(list); st = []; roots = []
    for i, o in enumerate(occ):
        if o[3] >= (1 << 63): st = [i]; continue
        while st and occ[st[-1]][0] != o[1]: st.pop()
        if st:
            j = st[-1]; dep[i] = dep[j] + 1; par[i] = j; kids[j].append(i)
            if dep[i] == 1: roots.append(i)
        st.append(i)
    t = [0.0] * n
    for i, o in enumerate(occ):
        if dep[i] == 0: continue
        k = norm(o[0]); t[i] = K * (med[k] if k in med else fb(o[0]) * upb)
    U = [H((o[0] + '\0' + '\0'.join(sorted(o[4]))).encode()) for o in occ]
    sub_t = [0.0] * n
    for i in range(n - 1, -1, -1): sub_t[i] = t[i] + sum(sub_t[c] for c in kids[i])
    tot_t += sum(t[i] for i in range(n) if dep[i] > 0)
    # today: one copy per .c-level include
    T = R['today']
    for r in roots:
        if occ[r][0] not in listed: T['text_t'] += sub_t[r]; continue
        m = []; q = [r]
        while q:
            x = q.pop(); m.append(x); q.extend(reversed(kids[x]))
        key = H(b''.join(U[x] + bytes([min(dep[x] - dep[r], 255)]) for x in m))
        T['asks'] += 1
        if key in today_stored: T['hits'] += 1; T['served_t'] += sub_t[r]; T['loads'] += 1
        else: T['text_t'] += sub_t[r]; T['writes'] += 1; T['written_t'] += sub_t[r]; today_stored.add(key)
    # units
    acc = [0.0] * n; cut = [False] * n
    for i in range(n - 1, 0, -1):
        if dep[i] == 0: continue
        acc[i] = t[i] + sum(acc[c] for c in kids[i] if not cut[c]); cut[i] = acc[i] >= THRESH
    near = [-1] * n; cp = collections.defaultdict(list); tops = []; members = collections.defaultdict(list)
    for i in range(n):
        if dep[i] == 0: continue
        a = near[par[i]] if par[i] >= 0 else -1
        if cut[i]:
            (tops if a == -1 else cp[a]).append(i); near[i] = i
        else: near[i] = a
        if near[i] >= 0: members[near[i]].append(i)
    pieces = sorted(members); units_per.append(len(pieces))
    key = {r: H(b''.join(U[x] + bytes([min(dep[x] - dep[r], 255)]) for x in members[r])) for r in pieces}
    combo = {}; fits = {}; allfit = {}
    for r in reversed(pieces):
        combo[r] = H(key[r] + b''.join(combo[c] for c in cp[r]))
        fits[r] = key[r] in stored_units
        allfit[r] = fits[r] and all(allfit[c] for c in cp[r])
    def subtree(r):
        out = [r]; q = [r]
        while q:
            x = q.pop()
            for c in cp[x]: out.append(c); q.append(c)
        return out
    new_units, new_combos = set(), set()
    def free(r, v):
        S = R[v]; S['asks'] += 1
        if allfit[r]:
            s = subtree(r); w = sum(acc[x] for x in s); S['served_t'] += w; S['hits'] += 1
            if v == 'merged':
                c = combo[r]
                if c in merged: S['loads'] += 1; S['merged_serves'] += 1
                else: S['loads'] += len(s)
                combo_count[c] += 1
                if combo_count[c] == 2:
                    merged.add(c); S['merge_builds'] += 1; S['merge_t'] += w; S['merge_units'] += len(s)
            else: S['loads'] += len(s)
            return
        S['text_t'] += acc[r]
        if not fits[r]: S['writes'] += 1; S['written_t'] += acc[r]; new_units.add(key[r])
        for c in cp[r]: free(c, v)
    for r in tops: free(r, 'free'); free(r, 'merged')
    P = R['partial']
    for r in tops:
        P['asks'] += 1
        for x in subtree(r):
            if fits[x]: P['loads'] += 1; P['served_t'] += acc[x]
            else: P['text_t'] += acc[x]; P['writes'] += 1; P['written_t'] += acc[x]
    Q = R['tied']
    def tied(r):
        Q['asks'] += 1
        if combo[r] in stored_combo:
            s = subtree(r); Q['loads'] += len(s); Q['served_t'] += sum(acc[x] for x in s); Q['hits'] += 1; return
        Q['text_t'] += acc[r]; Q['writes'] += 1; Q['written_t'] += acc[r]; new_combos.add(combo[r]); tied_copies[key[r]] += 1
        for c in cp[r]: tied(c)
    for r in tops: tied(r)
    stored_units |= new_units; stored_combo |= new_combos
    for i in range(n):
        if dep[i] > 0 and occ[i][0] not in first_time:
            first_time[occ[i][0]] = sub_t[i]
            if sub_t[i] >= THRESH: listed.add(occ[i][0])
out = {'proj': proj, 'compiles': ncomp, 'header_s': round(tot_t / 1e6, 2), 'units_per_compile': round(statistics.mean(units_per), 1),
       'stored_unit_versions': len(stored_units), 'tied_copies_per_version': round(sum(tied_copies.values()) / max(len(tied_copies), 1), 2)}
for v, S in R.items():
    out[v] = {k: (round(x / 1e6, 3) if k.endswith('_t') else x) for k, x in S.items()}
print(json.dumps(out))
