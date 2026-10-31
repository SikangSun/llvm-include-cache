import json,re,collections,os,sys
os.environ["GUARDRULE"]="anywhere"
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ddg_classes import file_info
R="/home/sikang/c/code/scratch/e2e/multi/results-2026-10-22/misscause"
Q = re.compile(r"^  query (\S+) (hit|miss) total=\d+ ask=\d+ check=\d+ checked=(\d+) load=\d+ replay=\d+ bytes=\d+(.*)$")
C = re.compile(r"^  check (\S+) lookup=(\d+) cand=(\d+)/(\d+) .* fails=(\S*) rest=(\d+)(?: plan=(\d+))? detail=(.*)$")
TX = re.compile(r"^  text (\S+) (\d+) depth (\d+) own (\d+) why (\S*)")
ORDER=["guards only","guards + what the guarded header provides","guards + other macros","no guard involved","no copy yet","plan failed after its sections"]
for p in ["postgres","git","cpython","curl","redis","nginx"]:
    D=f"/home/sikang/c/code/scratch/e2e/multi/results-2026-10-22/ddg3/{p}"; files=set()
    for fn in os.listdir(D):
        if fn.endswith(".ddg"):
            for l in open(f"{D}/{fn}",errors="replace"):
                if l[0]=="R": files.add(l.split("\t",2)[1])
    G={file_info(f)[0] for f in files}-{None}
    j=json.load(open(f"{R}/{p}.json"))
    for m in ["top","piecesB"]:
        c=collections.Counter(); t=collections.Counter(); req=0
        for e in j["S"][m]:
            lines=e.splitlines(); checks=collections.defaultdict(list); text={}
            for l in lines:
                if (g:=C.match(l)): checks[int(g.group(2))].append(g)
                if (g:=TX.match(l)): text.setdefault(g.group(1), int(g.group(2))/1e6)
            k=0; plan=None
            for l in lines:
                g=Q.match(l)
                if not g: continue
                k+=1; path=g.group(1)
                if "#section:" in path: continue
                if plan==path:
                    plan=None
                    if g.group(2)=="miss": c[ORDER[5]]+=1; t[ORDER[5]]+=text.get(path,0)
                    continue
                req+=1
                if " pieces=" in g.group(4): plan=path; continue
                if g.group(2)=="hit": continue
                if int(g.group(3))==0: cls=ORDER[4]
                else:
                    best=3
                    for x in checks[k]:
                        f=[n for n in x.group(5).split(",") if n]; r=int(x.group(6))
                        gs=[n for n in f if n[:-1] in G]
                        if gs and len(gs)==len(f) and r==0: best=min(best,0)
                        elif gs and len(gs)==len(f): best=min(best,1)
                        elif gs: best=min(best,2)
                    cls=ORDER[best]
                c[cls]+=1; t[cls]+=text.get(path,0)
        miss=sum(c.values()); tt=sum(t.values())
        print(f"{p:9s} {m:8s} misses {miss} ({100*miss/req:.1f}% of {req} requests), missed headers' parse {tt:.1f} s")
        for k in ORDER:
            if c[k]: print(f"      {k:45s} {c[k]:5d} = {100*c[k]/miss:4.0f}% of misses ({100*c[k]/req:4.1f}% of requests), parse {t[k]:5.2f} s = {100*t[k]/max(tt,1e-9):3.0f}%")
