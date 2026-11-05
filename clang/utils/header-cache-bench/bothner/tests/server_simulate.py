"""Checks simulate.py's server rule (R0 with the checks of
COMPILE_SERVER_DESIGN.md 2.6) on builds worked out by hand: the reasons in the
recompilation (the second pass), summed over its compiles. No decision may be
unsafe.

record (tests/record.py's m1, m2, m3) and two (tests/simulate.py's q1, q2,
q3): as the model's R0, which has the same outcome as R1 V1 on record, and the
size_t collapse on two:
  S2: record 8 reused, 9 nested, 1 main, 3 macro; two 4 reused, 8 uses,
      3 repeats.
  S3: record 9 reused, 4 macro, 2 main (en.h is one span); two as S2, plus
      h1.h's guard #define, a span with no tokens, reused in each compile.
  No nested boundaries between units joinable as groups: S2G = S2, S3G = S3.
neg: n.h `extern int foo;`; c1 reads n.h, c2 pre.h (`extern int foo;`) then
  n.h. A version made in c2 uses pre.h's foo (its earlier declaration), so c1
  refuses it by uses; c2 refuses c1's version, which had no earlier foo:
  negative. 1 reused (pre.h), 1 uses, 1 negative.
pred: pb.h `extern int baz;` after p0.h (f1), or after p0.h and p1.h (f2),
  all declaring baz. Its uses (p0.h, the first declaration) hold in both;
  the declarations of baz before it do not. In f1, f2's version misses p1.h,
  an earlier declaration it redeclares: deps, checked first; in f2, f1's
  version would hide p1.h's: predecessor. 3 reused (p0.h twice, p1.h).
deps: du.h calls bar, declared in d1.h (e1), or in d1.h and again in d2.h
  with noreturn (e2): its uses (d1.h) hold, the earlier declarations of bar
  do not: 3 reused, 2 deps.
nest: sn.h `struct SN { int a; }`, then `s1;` inside #if 1, read by g1 and g2.
  Under the fixed observer both fragments are nested (4 nested); under the
  old one both are reused (4); as a group they are one unit, reused (2).
ts: ts.h, guarded, defines struct ts with `#if LITTLE` inside (p.32);
  h1 and h3 define LITTLE, h2 does not. Option 2 and 3: every unit is nested
  (9). Option 2 groups: the struct is one group, whose members change with
  LITTLE: h1 reuses h3's (1 reused), h2 and h3 refuse by conditionals (2).
  Option 3 groups: the guard's #define joins the struct's first span, which
  §11.2's rule keeps out of groups: 6 nested. Option 3 groups that may hold
  #define lines (S3G*): one group, as option 2's.
flags: plain.h right after `#pragma pack(1)` (pending), cnt.h with
  __COUNTER__ (builtin), in two compiles: 2 pending, 2 builtin.
mainref: mb.h uses mx, which the main file redeclares after ma.h: a main-file
  declaration, new in every compile, so never reused (2 main); ma.h reused.
undef: und.h declares a static function without defining it (2.8): never
  reused (2 undefined). With the fallback instead, each compile restores it,
  ends with it undefined, and runs again without reuse (2 fallback).
undef2: the same prototype, defined further down in the same header. 2.8's
  rule refuses the prototype and so the definition, which redeclares it
  (2 undefined, 2 uses); the fallback reuses both (4 reused).
pm: an option-3 span with a member that only expands _Pragma("push_macro")
  (no tokens for the parser): a reuse would skip the push, so the span is
  never reused (S3: 2 builtin; arr_out's span, 2 reused). Alone, as an
  option-2 fragment, that member is empty and read as usual (S2: 4 reused),
  as is POP_X's.
vs: vec.h's vector_size(N * sizeof(int)) with N from n1.h (4) or n2.h (8):
  the AST keeps no reference to N, so the rule reuses vec.h across the two
  (4 reused) and the oracle does not notice; the oracle over identifier
  tokens does (unsafe2: w2 reuses w1's version), and the rule with identifier
  tokens as references
  refuses (2 reused, 2 deps).
usage: server_simulate.py <workdir>
"""
import collections, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from compiles import CLANG, ENV, MODEL_MOD

TRACE = os.path.join(os.path.dirname(HERE), "BothnerTrace.so")
SIM = os.path.join(os.path.dirname(HERE), "simulate.py")
work = os.path.abspath(sys.argv[1])
os.makedirs(work, exist_ok=True)
subprocess.run([sys.executable, "-I", os.path.join(HERE, "record.py"), os.path.join(work, "record")], check=True,
               capture_output=True)
TS = ("#ifndef TS_H\n#define TS_H\nstruct ts {\n  int len;\n#if LITTLE\n  int flags;\n#else\n  int overflow;\n#endif\n"
      "  int data;\n};\n#endif\n")
BUILDS = {
    "two": ({"h1.h": "#ifndef H1_H\n#define H1_H\n#if M1\ntypedef int word;\n#else\ntypedef long word;\n#endif\n"
                     "extern word flags;\n#endif\n",
             "sz.h": "typedef unsigned long sz_t;\n",
             "use.h": "#ifndef USE_H\n#define USE_H\nextern sz_t n;\n#endif\n",
             "q1.c": '#define M1 1\n#include "h1.h"\n#include "sz.h"\n#include "sz.h"\n#include "use.h"\n',
             "q2.c": '#include "h1.h"\n#include "sz.h"\n#include "sz.h"\n#include "use.h"\n',
             "q3.c": '#define M1 1\n#include "h1.h"\n#include "sz.h"\n#include "sz.h"\n#include "use.h"\n'},
            ["q1.c", "q2.c", "q3.c"]),
    "neg": ({"n.h": "extern int foo;\n", "pre.h": "extern int foo;\n",
             "c1.c": '#include "n.h"\n', "c2.c": '#include "pre.h"\n#include "n.h"\n'}, ["c1.c", "c2.c"]),
    "pred": ({"p0.h": "extern int baz;\n", "p1.h": "extern int baz;\n", "pb.h": "extern int baz;\n",
              "f1.c": '#include "p0.h"\n#include "pb.h"\n', "f2.c": '#include "p0.h"\n#include "p1.h"\n#include "pb.h"\n'},
             ["f1.c", "f2.c"]),
    "deps": ({"d1.h": "extern int bar(void);\n", "d2.h": "extern int bar(void) __attribute__((noreturn));\n",
              "du.h": "static inline int callbar(void) { return bar(); }\n",
              "e1.c": '#include "d1.h"\n#include "du.h"\n', "e2.c": '#include "d1.h"\n#include "d2.h"\n#include "du.h"\n'},
             ["e1.c", "e2.c"]),
    "nest": ({"sn.h": "struct SN { int a; }\n#if 1\ns1;\n#endif\n", "g1.c": '#include "sn.h"\n',
              "g2.c": '#include "sn.h"\n'}, ["g1.c", "g2.c"]),
    "ts": ({"ts.h": TS, "h1.c": '#define LITTLE 1\n#include "ts.h"\n', "h2.c": '#include "ts.h"\n',
            "h3.c": '#define LITTLE 1\n#include "ts.h"\n'}, ["h1.c", "h2.c", "h3.c"]),
    "mainref": ({"ma.h": "extern int mx;\n", "mb.h": "static inline int getmx(void) { return mx; }\n",
                 "mm1.c": '#include "ma.h"\nextern int mx;\n#include "mb.h"\n',
                 "mm2.c": '#include "ma.h"\nextern int mx;\n#include "mb.h"\n'}, ["mm1.c", "mm2.c"]),
    "undef": ({"und.h": "static int sf(void);\n", "u1.c": '#include "und.h"\n', "u2.c": '#include "und.h"\n'},
              ["u1.c", "u2.c"]),
    "undef2": ({"und2.h": "static int sf(void);\n#define SEP 1\nstatic int sf(void) { return 0; }\n",
                "v1.c": '#include "und2.h"\n', "v2.c": '#include "und2.h"\n'}, ["v1.c", "v2.c"]),
    "pm": ({"pm.h": '#define X 1\n#define PUSH_X _Pragma("push_macro(\\"X\\")")\n'
                    '#define POP_X _Pragma("pop_macro(\\"X\\")")\nPUSH_X\n#undef X\n#define X 2\nint arr_in[X];\n'
                    '#if 1\nPOP_X\n#endif\nint arr_out[X];\n',
            "p1.c": '#include "pm.h"\n', "p2.c": '#include "pm.h"\n'}, ["p1.c", "p2.c"]),
    "vs": ({"n1.h": "enum { N = 4 };\n", "n2.h": "enum { N = 8 };\n",
            "vec.h": "typedef int vec_t __attribute__((vector_size(N * sizeof(int))));\n",
            "w1.c": '#include "n1.h"\n#include "vec.h"\n', "w2.c": '#include "n2.h"\n#include "vec.h"\n'},
           ["w1.c", "w2.c"]),
    "flags": ({"plain.h": "int plain_v;\n", "cnt.h": "static int c1 = __COUNTER__;\n",
               "k1.c": '#pragma pack(1)\n#include "plain.h"\n#include "cnt.h"\n',
               "k2.c": '#pragma pack(1)\n#include "plain.h"\n#include "cnt.h"\n'}, ["k1.c", "k2.c"]),
}
for d, (files, srcs) in BUILDS.items():
    root = os.path.join(work, d)
    os.makedirs(root, exist_ok=True)
    for name, text in files.items():
        open(os.path.join(root, name), "w").write(text)
    for src in srcs:
        subprocess.run([CLANG, "-fsyntax-only"] + MODEL_MOD + ["-w", src, "-fplugin=" + TRACE], cwd=root, check=True,
                       env=dict(ENV, BOTHNER_TRACE_OUT=os.path.join(root, src + ".json"), BOTHNER_TRACE_RECORD="1"))


def run(srcdir, srcs):
    """simulate.py's server rule over these compiles, in order: the
    recompilation's reasons per variant."""
    build = os.path.join(work, "build-" + os.path.basename(srcdir))
    shutil.rmtree(build, ignore_errors=True)
    os.makedirs(build)
    for i, src in enumerate(srcs):
        d = json.load(open(os.path.join(srcdir, src + ".json")))
        json.dump({"meta": {"i": i, "src": src, "cwd": srcdir}, "data": d}, open(os.path.join(build, f"{i:03d}.json"), "w"))
    out = os.path.join(work, "server-" + os.path.basename(srcdir) + ".json")
    subprocess.run([sys.executable, "-I", SIM, build, build, "--server-out", out], check=True, capture_output=True)
    res = json.load(open(out))
    return ({v: sum((collections.Counter(c["why"]) for c in passes[1]), collections.Counter())
             for v, passes in res.items()},
            {v: sum(c["unsafe2"] for c in passes[1]) for v, passes in res.items()})


S_REC = {"reused": 8, "nested": 9, "main": 1, "macro": 3}
S_REC3 = {"reused": 9, "macro": 4, "main": 2}
S_TWO = {"reused": 4, "uses": 8, "repeat": 3}
S_TWO3 = {"reused": 7, "uses": 8, "repeat": 3}
want = {
    "record": {"S2": S_REC, "S3": S_REC3, "S2G": S_REC, "S3G": S_REC3, "S3G*": S_REC3, "S2 old": S_REC},
    "two": {"S2": S_TWO, "S3": S_TWO3, "S2G": S_TWO, "S3G": S_TWO3, "S3G*": S_TWO3},
    "neg": {"S2": {"reused": 1, "uses": 1, "negative": 1}},
    "pred": {"S2": {"reused": 3, "deps": 1, "predecessor": 1}},
    "deps": {"S2": {"reused": 3, "deps": 2}},
    "nest": {"S2": {"nested": 4}, "S2 old": {"reused": 4}, "S2G": {"reused": 2}, "S3": {"nested": 4},
             "S3G": {"reused": 2}, "S3G*": {"reused": 2}},
    "ts": {"S2": {"nested": 9}, "S3": {"nested": 9}, "S2G": {"reused": 1, "conditionals": 2}, "S3G": {"nested": 6},
           "S3G*": {"reused": 1, "conditionals": 2}},
    "flags": {"S2": {"pending": 2, "builtin": 2}},
    "mainref": {"S2": {"reused": 2, "main": 2}, "S3G*": {"reused": 2, "main": 2}},
    "undef": {"S2": {"undefined": 2}, "S2 fb": {"fallback": 2}},
    "undef2": {"S2": {"undefined": 2, "uses": 2}, "S2 fb": {"reused": 4}},
    "vs": {"S2": {"reused": 4}, "S2 fb tok": {"reused": 2, "deps": 2}},
    "pm": {"S2": {"reused": 4}, "S3": {"builtin": 2, "reused": 2}},
}
ok = True
for d, w in want.items():
    srcs = ["m1.c", "m2.c", "m3.c"] if d == "record" else BUILDS[d][1]
    got, unsafe2 = run(os.path.join(work, d), srcs)
    for v, exp in w.items():
        g = {k: n for k, n in got[v].items() if n and k != "empty"}
        good = g == exp
        ok = ok and good
        print(("ok   " if good else "FAIL ") + f"{d}, {v}: {g}" + ("" if good else f", expected {exp}"))
    bad = [v for v, c in got.items() if c.get("unsafe")]
    ok = ok and not bad
    print(("ok   " if not bad else "FAIL ") + f"{d}: no unsafe decision" + (f" ({bad})" if bad else ""))
    if d == "vs":
        good = unsafe2["S2"] == 1 and unsafe2["S2 fb tok"] == 0  # w2 reuses w1's vec.h; w1 its own
        ok = ok and good
        print(("ok   " if good else "FAIL ") + f"vs: the identifier-token oracle sees what the AST dropped: "
              f"unsafe2 {unsafe2['S2']} (rule as designed), {unsafe2['S2 fb tok']} (identifier tokens as references)")
print("all ok" if ok else "FAILURES")
sys.exit(0 if ok else 1)
