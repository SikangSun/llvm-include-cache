"""Duplicated header work in a first build, from the header-graph traces
(results-2026-10-22/ddg3: every compile, every header read as text).

Each header read ("occurrence") in each compile has:
  - its own size: source-location space of its own lines and the macro
    expansions in them, without the headers inside it (proxy for work);
  - its subtree size: its own plus everything inside it;
  - copy id: hash of the metadata its folded copy would carry here (R line);
  - unit id: the file plus every outside state its own lines read, with the
    value seen (E lines, setter ignored).
Ideal "link everything", zero overhead: a read is free when an earlier read
had the same unit id. Folded copies, zero overhead, perfect fit: a header
read straight from the .c file is free when an earlier read had the same
copy id; then nothing inside it is parsed. Inside a parsed header nothing is
served (the current rule). Compiles in benchmark order.
usage: dup.py <tracedir> <cmdlog-dir>"""
import os, sys, hashlib, collections, json

tdir = sys.argv[1]
files = sorted((f for f in os.listdir(tdir) if f.endswith('.ddg')), key=lambda n: int(n.split('.')[0]))
# Benchmark order: by pid order is the launch order of ddg_run's pool; good enough.

seen_unit = set(); seen_copy = set()
tot_own = 0; free_link = 0; free_fold = 0; tot_sub1 = 0
reads = 0; units = 0; copies1 = 0
store_link = 0; store_fold = 0
per_hdr = collections.defaultdict(lambda: [0, 0, 0])  # own work, free link, free fold
for fn in files:
    occ = []  # (path, parent, copyhash, subtree, edges)
    cur = None
    with open(os.path.join(tdir, fn), errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if p[0] == 'R':
                cur = [p[1], p[2], p[3], int(p[4]), []]
                occ.append(cur)
            elif p[0] == 'E' and cur is not None:
                cur[4].append((p[2], p[3], p[6] if len(p) > 6 else ''))
    if not occ:
        continue
    # Nesting and own sizes by a stack over the R lines in entry order.
    n = len(occ)
    own = [o[3] for o in occ]
    parent_idx = [-1] * n
    depth = [0] * n
    stack = []
    for i, o in enumerate(occ):
        if o[3] >= (1 << 63):
            own[i] = 0; stack = [i]; continue
        while stack and occ[stack[-1]][0] != o[1]:
            stack.pop()
        if stack:
            j = stack[-1]; parent_idx[i] = j; depth[i] = depth[j] + 1
            if own[j] >= (1 << 63) or occ[j][3] >= (1 << 63):
                pass
            else:
                own[j] -= o[3]
        stack.append(i)
    # Which reads are free under each scheme.
    fold_free = [False] * n
    for i, o in enumerate(occ):
        if depth[i] == 0:
            continue
        w = max(own[i], 0)
        tot_own += w; reads += 1
        h = per_hdr[o[0]]; h[0] += w
        key = hashlib.blake2b((o[0] + '\0' + '\0'.join(sorted('\t'.join(e) for e in o[4]))).encode(), digest_size=16).digest()
        if key in seen_unit:
            free_link += w; h[1] += w
        else:
            seen_unit.add(key); units += 1; store_link += w
        # folded: a depth-1 read decides for its whole subtree
        if depth[i] == 1:
            tot_sub1 += o[3]
            if o[2] in seen_copy:
                fold_free[i] = True
            else:
                seen_copy.add(o[2]); copies1 += 1; store_fold += o[3]
        else:
            fold_free[i] = fold_free[parent_idx[i]]
        if fold_free[i]:
            free_fold += w; h[2] += w
res = dict(compiles=len(files), reads=reads, units=units, copies=copies1,
           own_work=tot_own, link_free_pct=round(100 * free_link / tot_own, 1),
           fold_free_pct=round(100 * free_fold / tot_own, 1),
           store_link=store_link, store_fold=store_fold)
print(json.dumps(res))
top = sorted(per_hdr.items(), key=lambda kv: -(kv[1][1] - kv[1][2]))[:15]
for path, (w, l, fo) in top:
    print(f"  gap {100*(l-fo)/tot_own:5.2f}%  own {100*w/tot_own:5.2f}%  link-free {100*l/max(w,1):5.1f}%  fold-free {100*fo/max(w,1):5.1f}%  {path[-70:]}")
