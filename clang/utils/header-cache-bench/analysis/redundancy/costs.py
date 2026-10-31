"""Per-hit and per-write costs of COUNTER=1 from results-2026-10-30/lay stats."""
import json, re, sys
for proj in sys.argv[1:]:
    J = json.load(open(f'/home/sikang/c/code/scratch/e2e/multi/results-2026-10-30/lay/{proj}.json'))
    t = dict(hits=0, lookups=0, look=0, wait=0, check=0, load=0, replay=0, text=0, write=0, clang=0, parts=0)
    for s in J['S']['counter']:
        m = re.search(r'header cache: (\d+) hits / (\d+) lookups', s); t['hits'] += int(m[1]); t['lookups'] += int(m[2])
        m = re.search(r'time: ([\d.]+) ms in lookups \(([\d.]+) waiting for the daemon, ([\d.]+) checking candidates, ([\d.]+) loading\).*?([\d.]+) replaying', s)
        t['look'] += float(m[1]); t['wait'] += float(m[2]); t['check'] += float(m[3]); t['load'] += float(m[4]); t['replay'] += float(m[5])
        m = re.search(r'text time: ([\d.]+) ms', s); t['text'] += float(m[1])
        m = re.search(r'exported (\d+) parts.*?([\d.]+) ms writing and pushing', s); t['parts'] += int(m[1]); t['write'] += float(m[2])
        m = re.search(r'write time: ([\d.]+) ms in Clang', s); t['clang'] += float(m[1])
    h = max(t['hits'], 1)
    print(proj, json.dumps({k: round(v, 1) for k, v in t.items()}),
          'per lookup ms: wait %.3f check %.3f; per hit ms: load %.3f replay %.3f; per write ms %.2f' % (
          t['wait']/t['lookups'], t['check']/t['lookups'], t['load']/h, t['replay']/h, t['write']/max(t['parts'],1)))
