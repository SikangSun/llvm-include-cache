"""From a MEASURE_NESTED run (LEVEL=2 logs): inside each header a compile
wrote (a miss at the .c file's #include, its subtree parsed as text), which
nested headers had a stored copy that fit. Lines come at file exit, children
first; 'total' includes the headers inside, the cache's own work excluded."""
import json, re, sys, collections
d = json.load(open(sys.argv[1])); S = d['S']['counter']
pat = re.compile(r'^  text (\S+) (\d+) depth (\d+) own (\d+) why (.*)$')
misses = 0; misses_fit = 0; miss_us = 0; fit_us = 0; nested_reasons = collections.Counter()
top_fit_hdr = collections.Counter()
for s in S:
    pending = collections.defaultdict(list)
    for line in s.split('\n'):
        m = pat.match(line)
        if not m: continue
        path, tot, dep, own, why = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)
        node = dict(path=path, tot=tot, why=why, kids=pending.pop(dep + 1, []))
        pending[dep].append(node)
        if dep == 1 and (why.startswith('rejected') or why.startswith('no-saved-PCM')):
            misses += 1; miss_us += tot
            # topmost fitting nested reads
            got = 0; stack = list(node['kids'])
            while stack:
                k = stack.pop()
                nested_reasons[k['why'].split('/', 1)[-1].split(':')[0]] += 1
                if k['why'].endswith('/fits'):
                    got += k['tot']; top_fit_hdr[k['path'].split('/bench-src/')[-1]] += k['tot']
                else:
                    stack.extend(k['kids'])
            fit_us += got; misses_fit += got > 0
print(json.dumps(dict(written_misses=misses, misses_with_a_fitting_nested_copy=misses_fit,
    pct=round(100 * misses_fit / max(misses, 1), 1), miss_text_s=round(miss_us / 1e6, 1),
    fitting_nested_s=round(fit_us / 1e6, 1), fitting_share_pct=round(100 * fit_us / max(miss_us, 1), 1))))
print(" nested read reasons:", dict(nested_reasons.most_common(6)))
print(" top fitting nested headers (s):", [(h, round(v / 1e6, 2)) for h, v in top_fit_hdr.most_common(6)])
