"""(K) start <-> key bijection per (config, file) across all compiles, for frags and spans;
(T1) units whose refs|binds hold a name its identifier tokens do not spell, and whether any earlier unit declares it;
(P) decisions identical in passes 2-5 (indices 1..4) in out-server-s0.json."""
import json, os, sys, collections
R, OUT = sys.argv[1], sys.argv[2]
k2s = collections.defaultdict(set); s2k = collections.defaultdict(set)
k2s3 = collections.defaultdict(set); s2k3 = collections.defaultdict(set)
t1 = collections.Counter(); t1_declared = collections.Counter(); nunits = 0
for f in sorted(os.listdir(R)):
    if not f.endswith('.json'): continue
    d = json.load(open(os.path.join(R, f)))['data']
    if not d: continue
    fc = d['full_config']; names = d['names']
    bound = set()
    frs = sorted(d['frags'], key=lambda a: a[10])
    for a in d['frags']:
        nm = d['entries'][a[0]][0]
        k2s[(fc, nm, a[23])].add(a[1]); s2k[(fc, nm, a[1])].add(a[23])
    for s in d['spans']:
        nm = d['entries'][s[0]][0]
        k2s3[(fc, nm, s[15])].add(s[1]); s2k3[(fc, nm, s[1])].add(s[15])
    for a in frs:
        if a[8] == -1: continue
        nunits += 1
        toks = {names[n] for n in a[25]}
        extra = {(ns, names[n]) for ns, n in a[21] + a[22] if names[n] not in toks}
        for e in extra:
            t1[e] += 1
            if e in bound: t1_declared[e] += 1
        for ns, n in a[21]: bound.add((ns, names[n]))
print('frags: keys with >1 start', sum(len(v) > 1 for v in k2s.values()), '/', len(k2s),
      '; starts with >1 key', sum(len(v) > 1 for v in s2k.values()), '/', len(s2k))
print('spans: keys with >1 start', sum(len(v) > 1 for v in k2s3.values()), '/', len(k2s3),
      '; starts with >1 key', sum(len(v) > 1 for v in s2k3.values()), '/', len(s2k3))
print('T1: non-empty frags', nunits, '; refs|binds names not spelled by its tokens:', sum(t1.values()),
      'distinct', len(t1), '; of which an earlier frag declares it:', sum(t1_declared.values()))
print('   examples', t1.most_common(8), t1_declared.most_common(5))
o = json.load(open(OUT))
for lab, passes in o.items():
    same = all(passes[p][c] == passes[1][c] for p in range(2, len(passes)) for c in range(len(passes[1])))
    if not same: print('passes 2-5 differ in', lab)
print('passes 2-5 identical check done for', len(o), 'labels')
