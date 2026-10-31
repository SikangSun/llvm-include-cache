"""dup.py and policy3.py with two real weights per header read:
  bytes: the header file's size on disk;
  time:  the header's median own parse time (us) when the COUNTER=1 run of
         results-2026-10-30/lay read it as text (headers inside excluded);
         a header never read as text there gets bytes x the median us/byte.
Reports: repeated share, today's folded ceiling, and the piece schemes."""
import os, sys, hashlib, collections, json, re, statistics
proj = sys.argv[1]; tdir = sys.argv[2]
def norm(p):
    p = p.replace('/home/sikang/c/benchmarks/', '').replace('/dev/shm/bench-src/', '')
    p = re.sub(r'.*/lib/clang/\d+/include/', 'clang-include/', p)
    return p
# measured own times
own_us = collections.defaultdict(list)
J = json.load(open(f'/home/sikang/c/code/scratch/e2e/multi/results-2026-10-30/lay/{proj}.json'))
for s in J['S']['counter']:
    for x in re.finditer(r'\n  text (\S+) (\d+) depth (\d+) own (\d+)', s):
        own_us[norm(x.group(1))].append(int(x.group(4)))
med = {k: statistics.median(v) for k, v in own_us.items()}
size_cache = {}
def fbytes(p):
    if p not in size_cache:
        try: size_cache[p] = os.path.getsize(p)
        except OSError: size_cache[p] = 0
    return size_cache[p]
files = sorted((f for f in os.listdir(tdir) if f.endswith('.ddg')), key=lambda n: int(n.split('.')[0]))
# us per byte, for headers with no measured time
pairs = []
def wt_time(p):
    k = norm(p)
    if k in med: return med[k]
    return None
seenU=set(); seenS1=set(); seenS=set()
tot={'bytes':0,'time':0}; repeat={'bytes':0,'time':0}; fold={'bytes':0,'time':0}
missing_time=0; reads=0
occs_all=[]
for fn in files:
    occ=[]; cur=None
    for line in open(os.path.join(tdir,fn),errors='replace'):
        p=line.rstrip('\n').split('\t')
        if p[0]=='R': cur=[p[1],p[2],p[3],int(p[4]),[]]; occ.append(cur)
        elif p[0]=='E' and cur is not None: cur[4].append('\t'.join((p[2],p[3],p[6] if len(p)>6 else '')))
    occs_all.append(occ)
# us/byte from headers with both
ratios=[med[norm(p)]/fbytes(p) for p in set(o[0] for occ in occs_all for o in occ) if norm(p) in med and fbytes(p)>0]
upb = statistics.median(ratios) if ratios else 0
for occ in occs_all:
    if not occ: continue
    n=len(occ); par=[-1]*n; dep=[0]*n; st=[]
    for i,o in enumerate(occ):
        if o[3]>=(1<<63): st=[i]; continue
        while st and occ[st[-1]][0]!=o[1]: st.pop()
        if st: j=st[-1]; par[i]=j; dep[i]=dep[j]+1
        st.append(i)
    freeA=[False]*n
    for i,o in enumerate(occ):
        if dep[i]==0: continue
        reads+=1
        wb=fbytes(o[0]); t=wt_time(o[0])
        if t is None: t=wb*upb; missing_time+=1
        w={'bytes':wb,'time':t}
        key=hashlib.blake2b((o[0]+'\0'+'\0'.join(sorted(o[4]))).encode(),digest_size=16).digest()
        rep = key in seenU; seenU.add(key)
        if dep[i]==1:
            if o[2] in seenS1: freeA[i]=True
            else: seenS1.add(o[2])
        else: freeA[i]=freeA[par[i]]
        for k in w:
            tot[k]+=w[k]
            if rep: repeat[k]+=w[k]
            if freeA[i]: fold[k]+=w[k]
print(json.dumps({'reads':reads,'reads_without_measured_time_pct':round(100*missing_time/reads,1),
  'repeated_bytes_pct':round(100*repeat['bytes']/tot['bytes'],1),'fold_ceiling_bytes_pct':round(100*fold['bytes']/tot['bytes'],1),
  'repeated_time_pct':round(100*repeat['time']/tot['time'],1),'fold_ceiling_time_pct':round(100*fold['time']/tot['time'],1),
  'header_time_s':round(tot['time']/1e6,1)}))
