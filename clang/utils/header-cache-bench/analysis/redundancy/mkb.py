import sys
for src,dst in (('policy.py','policyb.py'),('policy3.py','policy3b.py')):
    s=open(src).read()
    a="import os, sys, hashlib, collections, json\n"
    assert a in s
    s=s.replace(a, a+"_sz={}\ndef fb(p):\n    if p not in _sz:\n        try: _sz[p]=os.path.getsize(p)\n        except OSError: _sz[p]=0\n    return _sz[p]\n",1)
    b="        U=[hashlib.blake2b("
    assert b in s, src
    s=s.replace(b,"        own=[fb(o[0]) if dep[i]>0 else 0 for i,o in enumerate(occ)]\n        subsz=[0]*n\n        for i in range(n-1,-1,-1):\n            subsz[i]=own[i]+sum(subsz[c] for c in kids[i])\n"+b,1)
    s=s.replace("wsub+=occ[r][3]","wsub+=subsz[r]").replace("wsub+=occ[k][3]","wsub+=subsz[k]")
    open(dst,'w').write(s)
print("ok")
