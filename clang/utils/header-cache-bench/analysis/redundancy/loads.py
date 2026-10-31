"""Loads per compile for three ways to store the same header work (from the
header-graph traces). A: fold at the .c file's #includes (today). B: link
every header (one stored unit per header read). C: fold where a whole
subtree repeats, else link the header's own unit and go inside.
Free work: share of header work (own source-location size) not parsed."""
import os, sys, hashlib, collections, json
tdir = sys.argv[1]
files = sorted((f for f in os.listdir(tdir) if f.endswith('.ddg')), key=lambda n: int(n.split('.')[0]))
seenU=set(); seenS=set(); seenS1=set()
W=0; fA=fB=fC=0; lA=lB=lC=0; nC=0
for fn in files:
    occ=[]; cur=None
    for line in open(os.path.join(tdir,fn),errors='replace'):
        p=line.rstrip('\n').split('\t')
        if p[0]=='R': cur=[p[1],p[2],p[3],int(p[4]),[]]; occ.append(cur)
        elif p[0]=='E' and cur is not None: cur[4].append('\t'.join((p[2],p[3],p[6] if len(p)>6 else '')))
    if not occ: continue
    nC+=1; n=len(occ); own=[o[3] for o in occ]; par=[-1]*n; dep=[0]*n; kids=collections.defaultdict(list); st=[]
    for i,o in enumerate(occ):
        if o[3]>=(1<<63): own[i]=0; st=[i]; continue
        while st and occ[st[-1]][0]!=o[1]: st.pop()
        if st:
            j=st[-1]; par[i]=j; dep[i]=dep[j]+1; kids[j].append(i)
            if occ[j][3]<(1<<63): own[j]-=o[3]
        st.append(i)
    unit=[hashlib.blake2b((o[0]+'\0'+'\0'.join(sorted(o[4]))).encode(),digest_size=16).digest() for o in occ]
    # A: fold at depth 1.
    freeA=[False]*n
    for i in range(n):
        if dep[i]==0: continue
        w=max(own[i],0); W+=w
        if dep[i]==1:
            if occ[i][2] in seenS1: freeA[i]=True; lA+=1
            else: seenS1.add(occ[i][2])
        else: freeA[i]=freeA[par[i]]
        if freeA[i]: fA+=w
        if unit[i] in seenU: fB+=w; lB+=1
    # C: top-down; a whole subtree served if its copy id was seen at any depth.
    def walkC(i):
        nonlocal_vals=[0,0]
        stack=[i]
        while stack:
            k=stack.pop()
            if occ[k][2] in seenS:
                # whole subtree free
                sub=[k]; q=[k]
                while q:
                    x=q.pop(); 
                    for y in kids[x]: q.append(y); sub.append(y)
                nonlocal_vals[0]+=sum(max(own[x],0) for x in sub); nonlocal_vals[1]+=1
                continue
            if unit[k] in seenU: nonlocal_vals[0]+=max(own[k],0); nonlocal_vals[1]+=1
            stack.extend(kids[k])
        return nonlocal_vals
    for i in range(n):
        if dep[i]==1:
            a,b=walkC(i); fC+=a; lC+=b
    # remember this compile's subtrees and units
    for i in range(n):
        if dep[i]>0: seenS.add(occ[i][2]); seenU.add(unit[i])
print(json.dumps(dict(compiles=nC, free_fold1=round(100*fA/W,1), loads_fold1=round(lA/nC,1),
      free_link=round(100*fB/W,1), loads_link=round(lB/nC,1), free_mixed=round(100*fC/W,1), loads_mixed=round(lC/nC,1))))
