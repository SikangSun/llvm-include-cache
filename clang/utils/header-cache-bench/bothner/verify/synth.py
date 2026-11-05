"""Synthetic builds for paths the redis records never exercise: record each
compile with the repo's BothnerTrace.so (record mode), run simulate.py
--server-out and reimpl2.py on the same build dir, compare every field of
every pass and compile, and print each side's pass-1 reason totals.

usage: synth.py <workdir> [case ...] [-- reimpl2 options]"""
import collections, json, os, shutil, subprocess, sys

BOTH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLANG = os.path.join(BOTH, "../../../../build_rel/bin/clang")
TRACE = os.path.join(BOTH, "BothnerTrace.so")
SIM = os.path.join(BOTH, "simulate.py")
HERE = os.path.dirname(os.path.abspath(__file__))
REIMPL = os.path.join(HERE, "reimpl2.py")
ENV = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_PPH", "BOTHNER_"))}

CASES = {
    # G1: an empty fragment (EMPTY expands to nothing) inside a struct, between conditionals.
    "emptyin": ({"e.h": "#ifndef E_H\n#define E_H\nstruct es {\n  int a;\n#if 1\nEMPTY\n#endif\n  int b;\n};\n#endif\n",
                 "e1.c": '#define EMPTY\n#include "e.h"\n', "e2.c": '#define EMPTY\n#include "e.h"\n'},
                ["e1.c", "e2.c"]),
    # G1': the same fragment empty in e1, a member in e3.
    "emptyin2": ({"e.h": "#ifndef E_H\n#define E_H\nstruct es {\n  int a;\n#if 1\nEMPTY\n#endif\n  int b;\n};\n#endif\n",
                  "e1.c": '#define EMPTY\n#include "e.h"\n', "e3.c": '#define EMPTY int c;\n#include "e.h"\n'},
                 ["e1.c", "e3.c", "e1.c", "e3.c"]),
    # G1'': an empty fragment right after a nested one, ending the run.
    "emptyend": ({"f.h": "#ifndef F_H\n#define F_H\nstruct fs {\n  int a;\n#if 1\nEMPTY\n#endif\n#include \"g.h\"\n"
                         "  int b;\n};\n#endif\n", "g.h": "",
                  "f1.c": '#define EMPTY\n#include "f.h"\n', "f2.c": '#define EMPTY\n#include "f.h"\n'},
                 ["f1.c", "f2.c"]),
    # G2: a null directive inside a struct body.
    "nulldir": ({"nd.h": "struct ns {\n  int a;\n#\n  int b;\n};\n",
                 "n1.c": '#include "nd.h"\n', "n2.c": '#include "nd.h"\n'}, ["n1.c", "n2.c"]),
    # T2: N declared in the MAIN file before vec.h; the AST drops the reference.
    "mainvec": ({"vec.h": "typedef int vec_t __attribute__((vector_size(N * sizeof(int))));\n",
                 "w1.c": 'enum { N = 4 };\n#include "vec.h"\n', "w2.c": 'enum { N = 8 };\n#include "vec.h"\n'},
                ["w1.c", "w2.c"]),
    # the repo test's vs case (header-declared N).
    "vs": ({"n1.h": "enum { N = 4 };\n", "n2.h": "enum { N = 8 };\n",
            "vec.h": "typedef int vec_t __attribute__((vector_size(N * sizeof(int))));\n",
            "w1.c": '#include "n1.h"\n#include "vec.h"\n', "w2.c": '#include "n2.h"\n#include "vec.h"\n'},
           ["w1.c", "w2.c"]),
    # fallback: a compile that reuses a still-undefined static prototype along with other units.
    "undef": ({"und.h": "static int sf(void);\n", "a.h": "extern int av;\n", "b.h": "extern int bv;\n",
               "u1.c": '#include "a.h"\n#include "und.h"\n#include "b.h"\n',
               "u2.c": '#include "a.h"\n#include "und.h"\n#include "b.h"\n'}, ["u1.c", "u2.c"]),
    # undefined in one compile only (u4 defines sf): fallback only where it stays undefined.
    "undef3": ({"und.h": "static int sf(void);\n", "a.h": "extern int av;\n",
                "u3.c": '#include "a.h"\n#include "und.h"\n',
                "u4.c": '#include "a.h"\n#include "und.h"\nstatic int sf(void) { return 1; }\nint g(void) { return sf(); }\n'},
               ["u3.c", "u4.c", "u3.c"]),
    # param, pending, builtin, macro args, pragma weak / clang attribute.
    "flags": ({"plain.h": "int plain_v;\n", "cnt.h": "static int c1 = __COUNTER__;\n",
               "par.h": "void pf(struct PT *p);\n",
               "k1.c": '#pragma pack(1)\n#include "plain.h"\n#include "cnt.h"\n#include "par.h"\n',
               "k2.c": '#pragma pack(1)\n#include "plain.h"\n#include "cnt.h"\n#include "par.h"\n'}, ["k1.c", "k2.c"]),
    "args": ({"ar.h": "#define DECL(x) x\nDECL(\n#if 1\nint arg_a;\n#endif\n)\nint arg_b;\n",
              "r1.c": '#include "ar.h"\n', "r2.c": '#include "ar.h"\n'}, ["r1.c", "r2.c"]),
    "weak": ({"wk.h": "extern int wk_v;\n", "at.h": "int at_f(void);\n",
              "p1.c": '#pragma weak wk_v\n#include "wk.h"\n#pragma clang attribute push (__attribute__((annotate("x"))), apply_to = function)\n#include "at.h"\n#pragma clang attribute pop\n',
              "p2.c": '#pragma weak wk_v\n#include "wk.h"\n#pragma clang attribute push (__attribute__((annotate("x"))), apply_to = function)\n#include "at.h"\n#pragma clang attribute pop\n'},
             ["p1.c", "p2.c"]),
    # "pragma": a pending #pragma weak (SV_WEAK), an active clang attribute (SV_ATTRIBUTE), a pack state change.
    "pragma": ({"wk.h": "extern int wk_v;\n", "at.h": "int at_f(void);\n", "pk.h": "struct pk { char c; int i; };\n",
                "q1.c": '#pragma weak wk_v\nint pad1;\n#include "wk.h"\n#pragma clang attribute push (__attribute__((annotate("x"))), apply_to = function)\nint pad2;\n#include "at.h"\n#pragma clang attribute pop\nint pad3;\n#include "pk.h"\n',
                "q2.c": '#pragma weak wk_v\nint pad1;\n#include "wk.h"\n#pragma clang attribute push (__attribute__((annotate("x"))), apply_to = function)\nint pad2;\n#include "at.h"\n#pragma clang attribute pop\n#pragma pack(2)\nint pad3;\n#include "pk.h"\n'},
               ["q1.c", "q2.c", "q1.c"]),
    # repeats under occ, with groups: mathcalls-like header read twice under different macros.
    "rep": ({"mc.h": "struct R_(s) {\n  int a;\n#if 1\n  int b;\n#endif\n};\nextern int R_(f)(void);\n",
             "rp1.c": '#define R_(x) x##_1\n#include "mc.h"\n#undef R_\n#define R_(x) x##_2\n#include "mc.h"\n',
             "rp2.c": '#define R_(x) x##_2\n#include "mc.h"\n#undef R_\n#define R_(x) x##_1\n#include "mc.h"\n'},
            ["rp1.c", "rp2.c"]),
}


def record(root, files, srcs):
    os.makedirs(root, exist_ok=True)
    for name, text in files.items():
        open(os.path.join(root, name), "w").write(text)
    for src in sorted(set(srcs)):
        subprocess.run([CLANG, "-fsyntax-only", "-w", src, "-fplugin=" + TRACE], cwd=root, check=True,
                       env=dict(ENV, BOTHNER_TRACE_OUT=os.path.join(root, src + ".json"), BOTHNER_TRACE_RECORD="1"))


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        k = argv.index("--")
        argv, extra = argv[:k], argv[k + 1:]
    work = os.path.abspath(argv[0])
    names = argv[1:] or list(CASES)
    allok = True
    for name in names:
        files, srcs = CASES[name]
        root = os.path.join(work, name)
        record(root, files, srcs)
        build = os.path.join(work, "build-" + name)
        shutil.rmtree(build, ignore_errors=True)
        os.makedirs(build)
        for i, src in enumerate(srcs):
            d = json.load(open(os.path.join(root, src + ".json")))
            json.dump({"meta": {"i": i, "src": src, "cwd": root}, "data": d}, open(os.path.join(build, f"{i:03d}.json"), "w"))
        theirs_p = os.path.join(work, "theirs-" + name + ".json")
        mine_p = os.path.join(work, "mine-" + name + ".json")
        subprocess.run([sys.executable, "-I", SIM, build, build, "--server-out", theirs_p], check=True,
                       capture_output=True)
        subprocess.run([sys.executable, "-I", REIMPL, build, build, mine_p] + extra, check=True, capture_output=True)
        theirs, mine = json.load(open(theirs_p)), json.load(open(mine_p))
        print("==", name, srcs)
        for label in theirs:
            diffs = []
            for p, (pt, pm) in enumerate(zip(theirs[label], mine[label])):
                for ct, cm in zip(pt, pm):
                    fl = [f for f in sorted(set(ct) | set(cm)) if ct.get(f) != cm.get(f)]
                    if fl:
                        diffs.append((p, ct["i"], {f: (cm.get(f), ct.get(f)) for f in fl}))
            tt = sum((collections.Counter(c["why"]) for c in theirs[label][1]), collections.Counter())
            tm = sum((collections.Counter(c["why"]) for c in mine[label][1]), collections.Counter())
            u2t = sum(c["unsafe2"] for c in theirs[label][1])
            u2m = sum(c["unsafe2"] for c in mine[label][1])
            if diffs:
                allok = False
                print("  %-16s DIFF %d compile-passes; pass1 theirs %s u2 %d | mine %s u2 %d" %
                      (label, len(diffs), dict(tt), u2t, dict(tm), u2m))
                for dd in diffs[:2]:
                    print("      pass %d compile %d (mine, theirs): %s" % dd)
            else:
                print("  %-16s same; pass1 %s u2 %d" % (label, dict(tt), u2t))
    print("ALL SAME" if allok else "SOME DIFFER")


main()
