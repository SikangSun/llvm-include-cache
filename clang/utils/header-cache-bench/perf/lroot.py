"""Reader work by root cause, from lstack.py's call stacks: a sample under
a reader entry point counts for the outermost one on its stack, whatever
function it is in; another sample in the reader (cats.py 'load') counts as
other. Prints % of each project's samples.
usage: lroot.py st_<proj>.pkl..."""
import os, pickle, re, sys, collections
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cats.py')).read().split("order = ")[0])
ROOTS = [
 ('reading copies at a hit', r'ASTReader::ReadASTs'),
 ('first use of a name', r'ASTReader::get$'),
 ('name a new copy also has', r'updateOutOfDateIdentifier'),
 ('redeclarations: anonymous member', r'ASTReader::CompleteRedeclChain'),
 ('fields of a struct', r'LoadFieldsFromExternalStorage'),
 ('header info', r'GetHeaderFileInfo'),
 ('source locations', r'getSLocEntryID|ReadSLocEntry|getLoadedSLocEntry'),
 ('macro definitions', r'resolvePendingMacro|ReadMacroRecord|getMacro\b'),
 ('member lookup', r'FindExternalVisibleDecls'),
 ('declaration by ID', r'ASTReader::GetDecl|ASTReader::GetExternalDecl|ReadDeclRecord'),
 ('types', r'ASTReader::GetType|readTypeRecord'),
]
RR = [(n, re.compile(p)) for n, p in ROOTS]
res = {}
for path in sys.argv[1:]:
    st = pickle.load(open(path, 'rb')); tot = sum(st.values())
    c = collections.Counter(); load = 0
    for s, n in st.items():
        lab = None
        for f in reversed(s):
            m = next((nm for nm, r in RR if r.search(f)), None)
            if m: lab = m; break
        if lab is None:
            k = next((k for k, r in R if r.search(s[0])), 'rest')
            if k != 'load': continue
            lab = 'other'
        load += n
        c[lab] += n
    res[path[3:-4]] = (tot, load, c)
ps = list(res)
print(f"{'root cause, % of the compile':38s} " + ' '.join(f'{p:>8s}' for p in ps))
print(f"{'all reader work':38s} " + ' '.join(f'{100*res[p][1]/res[p][0]:8.1f}' for p in ps))
for n, _ in ROOTS + [('other', '')]:
    print(f'{n:38s} ' + ' '.join(f'{100*res[p][2][n]/res[p][0]:8.1f}' for p in ps))
