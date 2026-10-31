"""Misses caused by folding (header-graph traces, today's policy: a copy per
#include of the .c file, the whole subtree inside it). For each miss, each
header read inside it is one of:
  copy elsewhere: a stored copy of that header (written for another compile's
     #include of it) has the same id, so it fits; its subtree counts with it;
  parsed before: no copy of its own fits, but the same unit (file + outside
     states) was parsed by an earlier compile, inside some other copy;
  new: never seen.
Weights: file bytes."""
import os, sys, hashlib, collections, json
tdir=sys.argv[1]
files=sorted((f for f in os.listdir(tdir) if f.endswith('.ddg')), key=lambda n:int(n.split('.')[0]))
_sz={}
def fb(p):
    if p not in _sz:
        try:_sz[p]=os.path.getsize(p)
        except OSError:_sz[p]=0
    return _sz[p]
stored=set(); seenU=set()
misses=0; miss_with_copy=0; miss_all_covered=0
w_copy=w_unit=w_new=w_rootown=0
for fn in files:
    occ=[];cur=None
    for line in open(os.path.join(tdir,fn),errors='replace'):
        p=line.rstrip('\n').split('\t')
        if p[0]=='R': cur=[p[1],p[2],p[3],int(p[4]),[]]; occ.append(cur)
        elif p[0]=='E' and cur is not None: cur[4].append('\t'.join((p[2],p[3],p[6] if len(p)>6 else '')))
    if not occ: continue
    n=len(occ); dep=[0]*n; kids=collections.defaultdict(list); st=[]; roots=[]
    for i,o in enumerate(occ):
        if o[3]>=(1<<63): st=[i]; continue
        while st and occ[st[-1]][0]!=o[1]: st.pop()
        if st:
            j=st[-1]; dep[i]=dep[j]+1; kids[j].append(i)
            if dep[i]==1: roots.append(i)
        st.append(i)
    U=[hashlib.blake2b((o[0]+'\0'+'\0'.join(sorted(o[4]))).encode(),digest_size=16).digest() for o in occ]
    def sub(k):
        out=[k]; q=[k]
        while q:
            x=q.pop()
            for y in kids[x]: q.append(y); out.append(y)
        return out
    newstored=[]
    for r in roots:
        if occ[r][2] in stored: continue
        misses+=1; newstored.append(r)
        w_rootown+=fb(occ[r][0])
        has=False; covered=True
        if U[r] not in seenU: covered=False
        stack=list(kids[r])
        while stack:
            k=stack.pop()
            if occ[k][2] in stored:
                has=True; w_copy+=sum(fb(occ[x][0]) for x in sub(k)); continue
            if U[k] in seenU: w_unit+=fb(occ[k][0])
            else: w_new+=fb(occ[k][0]); covered=False
            stack.extend(kids[k])
        miss_with_copy+=has; miss_all_covered+= covered
    for r in newstored: stored.add(occ[r][2])
    for i in range(n):
        if dep[i]>0: seenU.add(U[i])
inside=w_copy+w_unit+w_new
print(json.dumps(dict(misses=misses, with_fitting_copy_inside=miss_with_copy,
  pct_misses_with_copy=round(100*miss_with_copy/max(misses,1),1),
  pct_misses_everything_seen=round(100*miss_all_covered/max(misses,1),1),
  inside_bytes_copy_elsewhere_pct=round(100*w_copy/inside,1),
  inside_bytes_parsed_before_pct=round(100*w_unit/inside,1),
  inside_bytes_new_pct=round(100*w_new/inside,1),
  requested_header_own_share_pct=round(100*w_rootown/(inside+w_rootown),1))))
