import json, os, sys, collections
R = sys.argv[1]
noafter = collections.Counter(); sep0 = collections.Counter(); n = 0
for f in sorted(os.listdir(R)):
    if not f.endswith('.json'): continue
    r = json.load(open(os.path.join(R, f)))
    d = r['data']
    if not d: continue
    for a in d['frags']:
        if a[8] != -1 and (a[9] == -1 or a[18] == -1):
            noafter[(d['entries'][a[0]][0], a[1], a[9], a[18])] += 1
        if a[24] == 0:
            sep0[(d['entries'][a[0]][0], a[1], a[8], a[17])] += 1
print('non-empty frags with After/SAfter == -1:', sum(noafter.values()), 'distinct', len(noafter))
for k, v in noafter.most_common(15): print('  ', v, k)
print('frags with sep == 0:', sum(sep0.values()), 'distinct', len(sep0))
for k, v in sep0.most_common(15): print('  ', v, k)
