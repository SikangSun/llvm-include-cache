"""Header share of plain clang's front end: every 10th compile of a project,
plain clang -fsyntax-only -ftime-trace (granularity 0); header time = sum of
outermost "Source" events, total = "Total Frontend"."""
import json, os, subprocess, sys, tempfile
proj, core = sys.argv[1], sys.argv[2]
BIN = '/home/sikang/c/code/scratch/bin-lay/bin'; SRC = '/dev/shm/bench-src/'
d = f'/home/sikang/c/code/scratch/e2e/multi/log-{proj}'
cmds = []
for name in sorted(os.listdir(d), key=lambda n: int(n.split('.')[0])):
    parts = open(f'{d}/{name}', 'rb').read().decode(errors='replace').split('\0')[:-1]
    cwd, args = parts[0].replace('/home/sikang/c/benchmarks/', SRC), [a.replace('/home/sikang/c/benchmarks/', SRC) for a in parts[1:]]
    if '-c' not in args or '-E' in args: continue
    srcs = [a for i, a in enumerate(args) if a.endswith('.c') and args[i - 1] not in ('-o', '-MF', '-MT', '-MQ')]
    if len(srcs) != 1 or 'conftest' in srcs[0]: continue
    cmds.append((cwd, args))
tmp = tempfile.mkdtemp(prefix='hs-', dir='/dev/shm')
H = T = 0.0; n = 0
for j, (cwd, args) in enumerate(cmds[::10]):
    a = list(args)
    if '-o' in a: i = a.index('-o'); del a[i:i + 2]
    a[a.index('-c')] = '-fsyntax-only'
    tf = f'{tmp}/{j}.json'
    r = subprocess.run(['taskset', '-c', core, f'{BIN}/clang'] + a + ['-Xclang', f'-ftime-trace={tf}', '-Xclang', '-ftime-trace-granularity=0'], cwd=cwd, capture_output=True)
    if r.returncode or not os.path.exists(tf): continue
    evs = json.load(open(tf))['traceEvents']
    ev = sorted((e['ts'], 0 if e['ph'] == 'b' else 1) for e in evs if e.get('name') == 'Source' and e.get('ph') in ('b', 'e'))
    depth = 0; h = 0; start = 0
    for ts, kind in sorted((e['ts'], e['ph']) for e in evs if e.get('name') == 'Source' and e.get('ph') in ('b', 'e')):
        if kind == 'b':
            if depth == 0: start = ts
            depth += 1
        else:
            depth -= 1
            if depth == 0: h += ts - start
    tot = max([e['dur'] for e in evs if e.get('ph') == 'X' and e.get('name') in ('Frontend', 'ExecuteCompiler')] or [0])
    if tot: H += h; T += tot; n += 1
    os.remove(tf)
print(json.dumps({'proj': proj, 'compiles': n, 'header_s': round(H / 1e6, 3), 'frontend_s': round(T / 1e6, 3), 'share': round(H / T, 3)}))
