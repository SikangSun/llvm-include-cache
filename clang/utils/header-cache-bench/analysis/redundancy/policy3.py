"""Mixed fold/link with a real write policy (header-graph traces).
At each header read, top-down: a stored subtree copy with the same id serves
the whole subtree (1 load); else a stored unit (own lines, same outside
states) serves the own lines (1 load), and the reads inside are decided the
same way; else the own lines are parsed.
Writes, after each compile (sizes in source-location space):
  units: every parsed header's own lines, if that unit is new;
  P1: a subtree copy only at the topmost reads whose whole subtree was parsed;
  P2: a subtree copy at every read whose whole subtree was parsed.
Today (T): fold at the .c file's includes, write the subtree at each miss."""
import os, sys, hashlib, collections, json
tdir = sys.argv[1]
files = sorted((f for f in os.listdir(tdir) if f.endswith('.ddg')), key=lambda n: int(n.split('.')[0]))
def run(policy):
    sS=set(); sU=set(); W=0; free=0; loads=0; wsub=0; wunit=0; nC=0
    for fn in files:
        occ=[]; cur=None
        for line in open(os.path.join(tdir,fn),errors='replace'):
            p=line.rstrip('\n').split('\t')
            if p[0]=='R': cur=[p[1],p[2],p[3],int(p[4]),[]]; occ.append(cur)
            elif p[0]=='E' and cur is not None: cur[4].append('\t'.join((p[2],p[3],p[6] if len(p)>6 else '')))
        if not occ: continue
        nC+=1; n=len(occ); own=[o[3] for o in occ]; dep=[0]*n; kids=collections.defaultdict(list); st=[]; roots=[]
        for i,o in enumerate(occ):
            if o[3]>=(1<<63): own[i]=0; st=[i]; continue
            while st and occ[st[-1]][0]!=o[1]: st.pop()
            if st:
                j=st[-1]; dep[i]=dep[j]+1; kids[j].append(i)
                if occ[j][3]<(1<<63): own[j]-=o[3]
                if dep[i]==1: roots.append(i)
            st.append(i)
        U=[hashlib.blake2b((o[0]+'\0'+'\0'.join(sorted(o[4]))).encode(),digest_size=16).digest() for o in occ]
        for i in range(n):
            if dep[i]>0: W+=max(own[i],0)
        alltext={}
        newS=[]; newU=[]
        def visit(k):
            # returns True if the whole subtree of k was parsed as text
            nonlocal free, loads
            if policy!='T' and occ[k][2] in sS:
                sub=[k]; q=[k]
                while q:
                    x=q.pop()
                    for y in kids[x]: q.append(y); sub.append(y)
                free+=sum(max(own[x],0) for x in sub); loads+=1
                return False
            text=True
            if policy!='T' and U[k] in sU:
                free+=max(own[k],0); loads+=1; text=False
            else:
                newU.append(k)
            for c in kids[k]:
                if not visit(c): text=False
            alltext[k]=text
            return text
        for r in roots:
            if policy=='T':
                if occ[r][2] in sS:
                    q=[r]; sub=[r]
                    while q:
                        x=q.pop()
                        for y in kids[x]: q.append(y); sub.append(y)
                    free+=sum(max(own[x],0) for x in sub); loads+=1
                else:
                    newS.append(r)
                continue
            visit(r)
        if policy=='T':
            for r in newS:
                if occ[r][2] not in sS: sS.add(occ[r][2]); wsub+=occ[r][3]
            continue
        for k in newU:
            if U[k] not in sU: sU.add(U[k]); wunit+=max(own[k],0)
        # subtree copies
        def top_text(k, parent_text):
            nonlocal wsub
            t=alltext.get(k)
            if t is None: return
            ok = t or policy in ('P3a','P3b')
            if ok and (policy in ('P2','P3a') or (policy=='P3b' and dep[k]==1) or (policy=='P1' and not parent_text)):
                if occ[k][2] not in sS: sS.add(occ[k][2]); wsub+=occ[k][3]
            for c in kids[k]: top_text(c, t)
        for r in roots: top_text(r, False)
    return dict(free=round(100*free/W,1), loads=round(loads/nC,1), write_vs_work=round((wsub+wunit)/W,3))
res={p:run(p) for p in ['P3a','P3b']}
print(json.dumps(res))
