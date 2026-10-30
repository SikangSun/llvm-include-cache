"""Checks BothnerTrace's record mode on the paper's cases
(COMPILE_SERVER_PLAN.md section 5): a fragment's hash changes exactly where
the parser would receive other tokens, and fragments inside a declaration are
marked as such.
usage: record.py <workdir>
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from compiles import CLANG, ENV, MOD

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
}
for name, text in files.items():
    os.makedirs(os.path.dirname(os.path.join(work, name)), exist_ok=True)
    open(os.path.join(work, name), "w").write(text)


def record(src):
    out = os.path.join(work, src + ".json")
    r = subprocess.run([CLANG, "-fsyntax-only"] + MOD + ["-w", src, "-fplugin=" + TRACE], cwd=work,
                       env=dict(ENV, BOTHNER_TRACE_OUT=out, BOTHNER_TRACE_RECORD="1"), capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{src} failed:\n{r.stderr}")
    d = json.load(open(out))
    frags = {}
    for f in d["frags"]:
        name = d["entries"][f[0]][0]  # as the compile opened it
        frags.setdefault(name, []).append({"start": f[1], "hash": f[7], "before": f[8], "after": f[9]})
    return d, frags


ok = True
def check(cond, what):
    global ok
    ok &= cond
    print(("ok   " if cond else "FAIL ") + what)

d1, f1 = record("m1.c")
d2, f2 = record("m2.c")
d3, f3 = record("m3.c")
check(d1["config"] == d2["config"] == d3["config"], f"one configuration for the three compiles ({d1['config']})")
check(f1 == f3, "the same compile twice: every fragment's hash and nesting are equal")
check(f1["./buf.h"][0]["hash"] != f2["./buf.h"][0]["hash"], "BUFSIZ 10 against 20: buffer's fragment differs (p.29)")
check(f1["./a.h"][0]["hash"] == f2["./a.h"][0]["hash"], "a.h, read before the #defines: equal")
check(f1["./b.h"][0]["hash"] != f2["./b.h"][0]["hash"], "b.h after `#define int size_t` and `#define i j`: differs (p.29)")
# en.h: `enum { E_A,` | `E_B` | `}; int after_enum;` -- the last two lines
# have no directive between them, so they are one fragment, entered inside the
# enum.
en = f1["./en.h"]
nest = [(x["before"], x["after"]) for x in en]
check(nest == [(1, 0), (0, 0), (0, 1)], f"en.h nesting before/after each fragment: {nest}")
check(f1["./f.h"][0]["hash"] != f2["./sub/../f.h"][0]["hash"],
      "f.h opened as ./sub/../f.h instead of ./f.h: __FILE__ differs, so the fragment differs")
print("all ok" if ok else "FAILURES")
sys.exit(0 if ok else 1)
