"""Checks simulate.py's decisions on two builds worked out by hand: the
recompilation's reasons, summed over its compiles, for every variant.

Build 1, tests/record.py's m1, m2, m3. m1 and m3 read buf.h (BUFSIZ 10),
a.h, b.h (uses a.h), en.h (three fragments, all inside the enum) and f.h; m2
reads buf.h with BUFSIZ 20, b.h after `#define int size_t` (so it uses the
main file), and f.h as ./sub/../f.h. One version per location, the latest
parse. After the first build buf.h and b.h hold m3's versions.
  R1 V1: m1 4 reused 3 nested; m2 buf macro, b main, a f reused, 3 nested;
         m3 buf b macro (m2's are stored), a f reused, 3 nested.
  R1 V0: as R1 V1, but with no macro check buf and b pass the rule and fail
         the oracle: unsafe instead of macro.
  ideal: every non-nested instance whose oracle was seen at its location:
         m2's b.h still uses the main file.

Build 2, q1 (M1), q2 (no M1), q3 (M1); each reads h1.h (p.27: A `typedef
int word` or B `typedef long word`, then C `extern word flags`), sz.h twice
(S: `typedef unsigned long sz_t;`, no guard) and use.h (U: `extern sz_t n;`).
  R1: C alternates between using A and B, so the stored C (one version)
      fails its uses whenever M1 changes. The second sz.h is a repeat; its
      parse equals the first, so the version stays and U stays valid.
      q1 4 reused 1 repeat; q2 3 reused 1 uses 1 repeat; q3 the same.
  R0: every parse is a new version: the second sz.h replaces the first, and
      the first's version used itself, so S and U fail their uses in every
      compile (the size_t collapse). q1 A C reused, S U uses, 1 repeat;
      q2 B reused, C S U uses, 1 repeat; q3 A reused, C S U uses, 1 repeat.
  R2: the second sz.h is a location of its own and is reused; C as in R1.
  ideal: both versions of C are kept: everything reused.
Option 3 (O3 R1 V1): a header's guard #define joins its first fragment, and
en.h's enum, #define lines and all, is one fragment, not nested.
  build 1: m1 all 5 reused; m2 buf macro, b and en main (the main file's
           size_t), a f reused; m3 buf b en macro (m2's are stored), a f reused.
  build 2: h1.h's guard #define is a fragment of its own, with no tokens,
           always reused; otherwise as R1: 13 reused, 2 uses, 3 repeats.
usage: simulate.py <workdir>   (runs tests/record.py into <workdir>/record first)
"""
import collections, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from compiles import CLANG, ENV, MODEL_MOD

TRACE = os.path.join(os.path.dirname(HERE), "BothnerTrace.so")
SIM = os.path.join(os.path.dirname(HERE), "simulate.py")
work = os.path.abspath(sys.argv[1])
rec = os.path.join(work, "record")
subprocess.run([sys.executable, "-I", os.path.join(HERE, "record.py"), rec], check=True, capture_output=True)

two = os.path.join(work, "two")
os.makedirs(two, exist_ok=True)
for name, text in {
    "h1.h": "#ifndef H1_H\n#define H1_H\n#if M1\ntypedef int word;\n#else\ntypedef long word;\n#endif\nextern word flags;\n#endif\n",
    "sz.h": "typedef unsigned long sz_t;\n",
    "use.h": "#ifndef USE_H\n#define USE_H\nextern sz_t n;\n#endif\n",
    "q1.c": '#define M1 1\n#include "h1.h"\n#include "sz.h"\n#include "sz.h"\n#include "use.h"\n',
    "q2.c": '#include "h1.h"\n#include "sz.h"\n#include "sz.h"\n#include "use.h"\n',
    "q3.c": '#define M1 1\n#include "h1.h"\n#include "sz.h"\n#include "sz.h"\n#include "use.h"\n',
}.items():
    open(os.path.join(two, name), "w").write(text)
for src in ("q1.c", "q2.c", "q3.c"):
    subprocess.run([CLANG, "-fsyntax-only"] + MODEL_MOD + ["-w", src, "-fplugin=" + TRACE], cwd=two, check=True,
                   env=dict(ENV, BOTHNER_TRACE_OUT=os.path.join(two, src + ".json"), BOTHNER_TRACE_RECORD="1"))


def run(srcdir, srcs):
    """simulate.py over these compiles, in order; the records serve as the timing."""
    build = os.path.join(work, "build-" + os.path.basename(srcdir))
    shutil.rmtree(build, ignore_errors=True)
    os.makedirs(build)
    for i, src in enumerate(srcs):
        d = json.load(open(os.path.join(srcdir, src + ".json")))
        json.dump({"meta": {"i": i, "src": src, "cwd": srcdir}, "data": d}, open(os.path.join(build, f"{i:03d}.json"), "w"))
    out = os.path.join(work, "sim-" + os.path.basename(srcdir) + ".json")
    subprocess.run([sys.executable, "-I", SIM, build, build, "--out", out], check=True, capture_output=True)
    res = json.load(open(out))
    return {v: sum((collections.Counter(c["why"]) for c in cs), collections.Counter()) for v, cs in res.items()}


want = {
    "record": {
        "R1 V1": {"reused": 8, "nested": 9, "main": 1, "macro": 3},
        "R1 V0": {"reused": 8, "nested": 9, "main": 1, "unsafe": 3},
        "tokens": {"reused": 8, "nested": 9, "main": 1, "macro": 3},
        "ideal": {"reused": 11, "nested": 9, "main": 1},
        "O3 R1 V1": {"reused": 9, "macro": 4, "main": 2},
    },
    "two": {
        "R1 V1": {"reused": 10, "uses": 2, "repeat": 3},
        "R0 V1": {"reused": 4, "uses": 8, "repeat": 3},
        "R2 V1": {"reused": 13, "uses": 2},
        "ideal": {"reused": 15},
        "O3 R1 V1": {"reused": 13, "uses": 2, "repeat": 3},
    },
}
ok = True
for d, srcs in (("record", ["m1.c", "m2.c", "m3.c"]), ("two", ["q1.c", "q2.c", "q3.c"])):
    got = run(os.path.join(work, d), srcs)
    for v, w in want[d].items():
        g = {k: n for k, n in got[v].items() if n}
        good = g == w
        ok = ok and good
        print(("ok   " if good else "FAIL ") + f"build {d}, {v}: {g}" + ("" if good else f", expected {w}"))
print("all ok" if ok else "FAILURES")
sys.exit(0 if ok else 1)
