"""Checks BothnerTrace's record mode on the paper's cases
(COMPILE_SERVER_PLAN.md section 5): a fragment's token hash changes exactly
where the parser would receive other tokens; its macro list holds the state of
each name it used; fragments inside a declaration are marked; its uses are the
fragments its names resolved to; and its oracle changes wherever its parse
would mean something else, also where its tokens do not change.
usage: record.py <workdir>
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from compiles import CLANG, ENV, MODEL_MOD

TRACE = os.path.join(os.path.dirname(HERE), "BothnerTrace.so")
work = os.path.abspath(sys.argv[1])
os.makedirs(work, exist_ok=True)
files = {
    # p.29: BUFSIZ changed; p.29: `#define int size_t` between a.h and b.h.
    "buf.h": "#ifndef BUF_H\n#define BUF_H\nchar buffer[BUFSIZ];\n#endif\n",
    "a.h": "#ifndef A_H\n#define A_H\nint i, j;\n#endif\n",
    "b.h": "#ifndef B_H\n#define B_H\ninline int foo(void) { return i; }\n#endif\n",
    # p.32: an enum with #define lines inside (bits/siginfo.h, bits/confname.h).
    "en.h": "#ifndef EN_H\n#define EN_H\nenum {\n  E_A,\n#define E_A E_A\n  E_B\n#define E_B E_B\n};\nint after_enum;\n#endif\n",
    # __FILE__ in a header: the same text under another name is other tokens.
    "f.h": "static const char *where = __FILE__;\n",
    "sub/g.h": '#include "../f.h"\n',
    "m1.c": '#define BUFSIZ 10\n#include "buf.h"\n#include "a.h"\n#include "b.h"\n#include "en.h"\n#include "f.h"\n',
    "m2.c": '#define BUFSIZ 20\n#include "buf.h"\n#include "a.h"\n#define int size_t\n#define i j\ntypedef long size_t;\n#include "b.h"\n#include "en.h"\n#include "sub/g.h"\n',
    "m3.c": '#define BUFSIZ 10\n#include "buf.h"\n#include "a.h"\n#include "b.h"\n#include "en.h"\n#include "f.h"\n',
    # p.27: `flags` depends on which definition of `word` was read.
    "h1.h": "#ifndef H1_H\n#define H1_H\n#if M1\ntypedef int word;\n#else\ntypedef long word;\n#endif\nextern word flags;\n#endif\n",
    "w1.c": '#define M1 1\n#include "h1.h"\n',
    "w2.c": '#include "h1.h"\n',
    # The same tokens meaning something else, from an earlier declaration
    # (the hidden-module cases of section 1): an asm label, a weak attribute.
    "asm.h": 'extern int foo2_(void) __asm__("foo2");\n',
    "weak.h": "extern int bar(void) __attribute__((weak));\n",
    "call.h": "extern int foo2_(void);\nextern int bar(void);\nstatic inline int call(void) { return foo2_() + bar(); }\n",
    # p.28-29: a tag first declared in a parameter list; and one declared before.
    "p72.h": "extern int do_something(struct x *);\nstruct y;\nextern int other(struct y *);\n",
    "x1.c": '#include "asm.h"\n#include "weak.h"\n#include "call.h"\n#include "p72.h"\n',
    "x2.c": '#include "call.h"\n',
    # #pragma pack changes a struct's layout, not its tokens.
    "s.h": "struct S { char c; int i; };\n",
    "p1.c": '#pragma pack(1)\n#include "s.h"\n',
    "p2.c": '#include "s.h"\n',
    # What the meaning depends on without a lookup: the struct a typedef
    # names (defined elsewhere, wider under WIDE), a member access through a
    # pointer typedef; and a library builtin, valid to call only once a
    # header declared it.
    "td.h": "typedef struct L L;\n",
    "tdef.h": "#if WIDE\nstruct L { long a; };\n#else\nstruct L { int a; };\n#endif\n",
    "tuse.h": "extern L arr[2];\n",
    "mem.h": "#if WIDE\nstruct M2 { long x; };\n#else\nstruct M2 { int x; };\n#endif\n",
    "memuse.h": "typedef struct M2 *MP;\nstatic inline long getx(MP m) { return m->x; }\n",
    "mc.h": "void *memcpy(void *, const void *, unsigned long);\n",
    "mcuse.h": "static inline void cp(void *d, const void *s) { memcpy(d, s, 1); }\n",
    "t1.c": '#define WIDE 1\n#include "td.h"\n#include "tdef.h"\n#include "tuse.h"\n#include "mem.h"\n#include "memuse.h"\n#include "mc.h"\n#include "mcuse.h"\n',
    "t2.c": '#include "td.h"\n#include "tdef.h"\n#include "tuse.h"\n#include "mem.h"\n#include "memuse.h"\n#include "mc.h"\n#include "mcuse.h"\n',
    # The main file using a header's declaration (what blank.py must not blank).
    "r1.c": '#define BUFSIZ 10\n#include "a.h"\n#include "buf.h"\nint use(void) { return j; }\n',
}
for name, text in files.items():
    os.makedirs(os.path.dirname(os.path.join(work, name)), exist_ok=True)
    open(os.path.join(work, name), "w").write(text)


def record(src):
    """The compile's configuration and, per header as opened, its fragments."""
    out = os.path.join(work, src + ".json")
    r = subprocess.run([CLANG, "-fsyntax-only"] + MODEL_MOD + ["-w", src, "-fplugin=" + TRACE], cwd=work,
                       env=dict(ENV, BOTHNER_TRACE_OUT=out, BOTHNER_TRACE_RECORD="1"), capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{src} failed:\n{r.stderr}")
    d = json.load(open(out))
    where = [(d["entries"][f[0]][0], f[1]) for f in d["frags"]]
    frags = {}
    for f in d["frags"]:
        frags.setdefault(d["entries"][f[0]][0], []).append({
            "start": f[1], "hash": f[7], "before": f[8], "after": f[9], "pragma": f[11], "oracle": f[12],
            "flags": f[13], "uses": [where[u] for u in f[14]], "ids": {d["names"][n]: s for n, s in f[15]}})
    return d["config"], frags


ok = True
def check(cond, what):
    global ok
    ok = ok and bool(cond)
    print(("ok   " if cond else "FAIL ") + what)

c1, f1 = record("m1.c")
c2, f2 = record("m2.c")
c3, f3 = record("m3.c")
check(c1 == c2 == c3, f"one configuration for the three compiles ({c1})")
check(f1 == f3, "the same compile twice: every field of every fragment is equal")
buf1, buf2 = f1["./buf.h"][0], f2["./buf.h"][0]
check(buf1["hash"] != buf2["hash"], "BUFSIZ 10 against 20: buffer's fragment differs (p.29)")
check(buf1["ids"]["buffer"] == 0 and buf1["ids"]["BUFSIZ"] != 0 and buf1["ids"]["BUFSIZ"] != buf2["ids"]["BUFSIZ"],
      f"its macro list: `buffer` no macro, BUFSIZ a macro with another definition in m2 ({sorted(buf1['ids'])})")
check(f1["./a.h"][0]["hash"] == f2["./a.h"][0]["hash"], "a.h, read before the #defines: equal")
b2 = f2["./b.h"][0]
check(f1["./b.h"][0]["hash"] != b2["hash"], "b.h after `#define int size_t` and `#define i j`: differs (p.29)")
check(b2["ids"].get("i", 0) != 0 and b2["ids"].get("int", 0) != 0, "b.h's macro list in m2: `i` and `int` are macros")
check(b2["uses"] == [("./a.h", 24)] and b2["flags"] & 1,
      f"b.h in m2 uses a.h's `j` and the main file's size_t: uses {b2['uses']}, flags {b2['flags']}")
check(f1["./b.h"][0]["uses"] == [("./a.h", 24)] and not f1["./b.h"][0]["flags"] & 1, "b.h in m1 uses a.h's `i` only")
nest = [(x["before"], x["after"]) for x in f1["./en.h"]]
# en.h: `enum { E_A,` | `E_B` | `}; int after_enum;`: the last two lines have no
# directive between them, so they are one fragment, entered inside the enum.
check(nest == [(1, 0), (0, 0), (0, 1)], f"en.h nesting before/after each fragment: {nest}")
check(f1["./f.h"][0]["hash"] != f2["./sub/../f.h"][0]["hash"],
      "f.h opened as ./sub/../f.h instead of ./f.h: __FILE__ differs, so the fragment differs")

_, w1 = record("w1.c")
_, w2 = record("w2.c")
fl1, fl2 = w1["./h1.h"][-1], w2["./h1.h"][-1]
check(fl1["hash"] == fl2["hash"], "h1.h `extern word flags;` has the same tokens with M1 and without (p.27)")
check(fl1["uses"] != fl2["uses"] and len(fl1["uses"]) == len(fl2["uses"]) == 1,
      f"... but uses the other `word` fragment: {fl1['uses']} against {fl2['uses']}")
check(fl1["oracle"] != fl2["oracle"], "... and its oracle differs")

_, x1 = record("x1.c")
_, x2 = record("x2.c")
k1, k2 = x1["./call.h"][0], x2["./call.h"][0]
check(k1["hash"] == k2["hash"], "call.h has the same tokens after asm.h and weak.h and without them")
check(sorted(k1["uses"]) == [("./asm.h", 0), ("./weak.h", 0)] and k2["uses"] == [],
      f"... uses the first declarations of foo2_ and bar where they came first: {k1['uses']}")
check(k1["oracle"] != k2["oracle"], "... and its oracle differs (the asm label and weak are inherited)")
p72 = x1["./p72.h"][0]
check(p72["flags"] & 4, f"p72.h declares `struct x` in a parameter list (p.28-29): flags {p72['flags']}")
_, q1 = record("p1.c")
_, q2 = record("p2.c")
s1, s2 = q1["./s.h"][0], q2["./s.h"][0]
check(s1["hash"] == s2["hash"] and s1["pragma"] != s2["pragma"] and s1["oracle"] != s2["oracle"],
      "s.h under #pragma pack(1): same tokens, other pragma state, other oracle")
_, t1 = record("t1.c")
_, t2 = record("t2.c")
u1, u2 = t1["./tuse.h"][0], t2["./tuse.h"][0]
check(u1["hash"] == u2["hash"] and u1["uses"] == u2["uses"] == [("./td.h", 0)] and u1["oracle"] != u2["oracle"],
      "tuse.h `extern L arr[2];`: same tokens, uses the typedef only, but its oracle sees struct L change")
g1, g2 = t1["./memuse.h"][0], t2["./memuse.h"][0]
check(g1["hash"] == g2["hash"] and g1["oracle"] != g2["oracle"],
      "memuse.h `m->x` through a pointer typedef: same tokens, its oracle sees struct M2 change")
check(t1["./mcuse.h"][0]["uses"] == [("./mc.h", 0)],
      f"mcuse.h calls memcpy, which needs mc.h's declaration: uses {t1['./mcuse.h'][0]['uses']}")
_, r1 = record("r1.c")
check(r1["./a.h"][0]["flags"] & 8 and not r1["./buf.h"][0]["flags"] & 8,
      f"r1.c uses a.h's j, not buf.h's buffer: flags {r1['./a.h'][0]['flags']}, {r1['./buf.h'][0]['flags']}")
print("all ok" if ok else "FAILURES")
sys.exit(0 if ok else 1)
