"""Checks the fields BothnerTrace's record mode keeps for the compile server's
checks (COMPILE_SERVER_DESIGN.md 2.6, 3): nesting under the fixed observer
(2.9), the full pragma state, never-reusable builtins, pending parser pragmas,
directives inside macro arguments, what each fragment declares and references
at file scope, the main file's declarations, keys and separators, and the
server's configuration key.
usage: server_record.py <workdir>
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from compiles import CLANG, ENV, MODEL_MOD

TRACE = os.path.join(os.path.dirname(HERE), "BothnerTrace.so")
SV_BUILTIN, SV_ARGS, SV_PENDING, SV_WEAK, SV_ATTRIBUTE, SV_UNDEFINED = 1, 2, 4, 8, 16, 32
SEP_COND, SEP_DEFINE, SEP_INCLUDE, SEP_PRAGMA, SEP_OTHER, SEP_FIRST = 1, 2, 4, 8, 16, 32
work = os.path.abspath(sys.argv[1])
os.makedirs(work, exist_ok=True)
files = {
    # The observer's two misses (2.9): after a struct body's '}', and between
    # decl-specifiers and declarators. A function body's '}' and a ';' at file
    # scope end a declaration; __extension__ starts one.
    "nest.h": "struct S { int a; }\n#if 1\ns1;\n#endif\nextern int\n#if 1\nx1;\n#endif\n"
              "static int f(void) { return 1; }\n#define NX 1\nint y1;\n#define NY 1\n"
              "__extension__\n#if 1\ntypedef long long ll;\n#endif\nint z1;\n",
    "en.h": "#ifndef EN_H\n#define EN_H\nenum {\n  E_A,\n#define E_A E_A\n  E_B\n#define E_B E_B\n};\nint after_enum;\n#endif\n",
    "n1.c": '#include "nest.h"\n#include "en.h"\n',
    # Pragma state the old record missed, and the states that refuse outright.
    "plain.h": "int plain_v;\n",
    "v1.c": '#pragma GCC visibility push(hidden)\nint d1;\n#include "plain.h"\n',
    "v2.c": 'int d1;\n#include "plain.h"\n',
    "an.h": "#pragma clang assume_nonnull begin\nint *an_p;\n#pragma clang assume_nonnull end\n",
    "a1.c": 'int d1;\n#include "an.h"\n',
    "o1.c": '#pragma clang optimize off\nint d1;\n#include "plain.h"\n',
    "at1.c": '#pragma clang attribute push (__attribute__((annotate("x"))), apply_to = variable)\n'
             'int d1;\n#include "plain.h"\n#pragma clang attribute pop\n',
    "wk1.c": '#pragma weak plain_v\nint d1;\n#include "plain.h"\n',
    # A parser pragma pending at a header's start (2.4), and not.
    "pv1.c": '#pragma GCC visibility push(default)\n#include "plain.h"\n',
    "pk1.c": '#pragma pack(1)\n#include "plain.h"\n',
    # Never-reusable builtins: __COUNTER__, _Pragma, __LINE__ after #line;
    # __LINE__ and __FILE__ without #line are fixed per key.
    "cnt.h": "static int c1 = __COUNTER__;\n",
    "prg.h": '_Pragma("GCC diagnostic push") static int p1;\n',
    "ln.h": "static int l1 = __LINE__;\nstatic const char *fl = __FILE__;\n#line 100\nstatic int l2 = __LINE__;\n",
    "b1.c": '#include "cnt.h"\n#include "prg.h"\n#include "ln.h"\n',
    # A directive inside a macro's arguments.
    "args.h": "#define ID(x) x\nID(\n#if 1\nint z;\n#endif\n) int w;\n",
    "g1.c": '#include "args.h"\n',
    # A fragment that ends inside a macro's arguments.
    "args2.h": "#define DECL(x) x\nint a0; DECL(\n#if 1\nint b0;\n#endif\n)\nint c0;\n#define AFTER 1\nint d0;\n",
    "g2.c": '#include "args2.h"\n',
    # A directive between K&R parameter declarations and the body: inside the
    # definition, though the observer reads a boundary.
    "kr.h": "int kr(a)\nint a;\n#if 1\n{ return a; }\n#endif\n",
    "k1.c": '#include "kr.h"\n',
    # cleanup's function, which the AST traversal does not visit; and
    # internal or inline functions declared without a definition (2.8).
    "cl.h": "void done(int *p);\n",
    "clu.h": "static inline void user(void) { int x __attribute__((cleanup(done))) = 0; (void)x; }\n",
    "und.h": "static int sf(void);\ninline int ig(void);\nextern int ef(void);\n#define SEP 1\n"
             "static int sf2(void) { return 1; }\nextern int ef2(void);\n",
    "c1.c": '#include "cl.h"\n#include "clu.h"\n#include "und.h"\n',
    # A prototype the same fragment defines (not flagged); one defined later.
    "undd.h": "static int sp(void);\nstatic int sp(void) { return 0; }\n",
    "undx.h": "static int sx(void);\n#define SEP2 1\nstatic int sx(void) { return 0; }\n",
    # Parser pragmas inside a header: a fragment right after one starts with
    # the pragma pending, and its pragma state is recorded at its first token.
    "pp.h": "#pragma pack(push, 1)\nstruct PP { char c; int i; };\n#pragma pack(pop)\nint after_pp;\n",
    # A directive with no callback (the null directive) inside a struct.
    "nd.h": "struct ns {\nint a;\n#\nint b;\n};\n",
    # A macro's arguments whose macro name came from another macro.
    "args3.h": "#define FOO(x) x\n#define CALL FOO\nint a1; CALL(\n#if 1\nint b1;\n#endif\n) int c1;\n",
    # __builtin_convertvector's destination type; a tag a failed lookup
    # declared at block scope.
    "cvt.h": "typedef float vf4 __attribute__((ext_vector_type(4)));\ntypedef int vi4 __attribute__((ext_vector_type(4)));\n",
    "cvu.h": "static inline int cvs(vi4 x) { return (int)sizeof(__builtin_convertvector(x, vf4)); }\n",
    "bt.h": "static inline int bt(void *v) { struct bx *p = v; return p != 0; }\n",
    # A directive between two K&R parameter declarations.
    "kr2.h": "int kr2(a, b)\nint a;\n#if 1\nint b;\n#endif\n{ return a + b; }\n",
    # One after a top-level #pragma and an empty declaration, which hand the
    # consumer nothing.
    "kr3.h": "#pragma pack(push, 1)\n#pragma pack(pop)\n;\n#if 1\nint kr3(a)\nint a;\n#endif\n{ return a; }\n",
    "c2.c": '#include "undd.h"\n#include "undx.h"\n#include "pp.h"\n#include "nd.h"\n#include "args3.h"\n'
            '#include "cvt.h"\n#include "cvu.h"\n#include "bt.h"\n#include "kr2.h"\n#include "kr3.h"\n',
    # What fragments declare and reference at file scope; the main file's
    # declarations.
    "dcl.h": "int da;\nstruct T { int x; struct N { int y; } n; };\nenum { E1, E2 };\ntypedef int I;\nstruct U *up;\n",
    "use.h": "static inline int g(struct T *t) { return da + t->x + E2; }\n",
    "d1.c": 'int m1;\n#include "dcl.h"\n#include "use.h"\nint m2;\n',
}
for name, text in files.items():
    os.makedirs(os.path.dirname(os.path.join(work, name)), exist_ok=True)
    open(os.path.join(work, name), "w").write(text)


def record(src, extra=()):
    out = os.path.join(work, src + ".json")
    r = subprocess.run([CLANG, "-fsyntax-only"] + MODEL_MOD + ["-w", src, "-fplugin=" + TRACE] + list(extra),
                       cwd=work, env=dict(ENV, BOTHNER_TRACE_OUT=out, BOTHNER_TRACE_RECORD="1"),
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{src} failed:\n{r.stderr}")
    d = json.load(open(out))
    names = d["names"]
    frags = {}
    for f in d["frags"]:
        frags.setdefault(d["entries"][f[0]][0], []).append({
            "start": f[1], "before": f[8], "after": f[9], "order": f[10], "pragma": f[11],
            "sbefore": f[17], "safter": f[18], "prag": f[19], "server": f[20],
            "binds": [(ns, names[n]) for ns, n in f[21]], "refs": sorted((ns, names[n]) for ns, n in f[22]),
            "key": f[23], "sep": f[24], "empty": f[8] == -1})
    spans = {}
    for s in d["spans"]:
        spans.setdefault(d["entries"][s[0]][0], []).append({"start": s[1], "defines": s[4], "key": s[15],
                                                            "sep": s[16], "sbefore": s[11], "safter": s[12]})
    main = [(o, ns, names[n]) for o, ns, n in d["main_decls"]]
    return d, frags, spans, main


ok = True
def check(cond, what):
    global ok
    ok = ok and bool(cond)
    print(("ok   " if cond else "FAIL ") + what)


d, f, sp, _ = record("n1.c")
nest = [(x["before"], x["after"], x["sbefore"], x["safter"]) for x in f["./nest.h"]]
# nest.h's fragments: `struct S {...}` | `s1;` | `extern int` | `x1;` |
# `static int f...}` | `int y1;` | `__extension__` | `typedef long long ll;` | `int z1;`
check([n[:2] for n in nest][:4] == [(1, 1), (1, 1), (1, 1), (1, 1)],
      f"the old observer reads `}}` of a struct body and `extern int` as between declarations: {nest[:4]}")
check([n[2:] for n in nest][:4] == [(1, 0), (0, 1), (1, 0), (0, 1)],
      f"the fixed observer reads them as inside one: {[n[2:] for n in nest][:4]}")
check(nest[4][3] == 1 and nest[5][2:] == (1, 1),
      f"a function body's '}}' and a ';' at file scope end a declaration: {nest[4:6]}")
check(nest[6][2:] == (1, 0) and nest[7][2] == 0 and nest[8][2:] == (1, 1),
      f"__extension__ starts one: {nest[6:]}")
en = [(x["sbefore"], x["safter"]) for x in f["./en.h"]]
check(en == [(1, 0), (0, 0), (0, 1)], f"en.h under the fixed observer as under the old one: {en}")
check([x["sep"] for x in f["./en.h"]] == [SEP_FIRST | SEP_COND | SEP_DEFINE, SEP_DEFINE, SEP_DEFINE],
      f"en.h separators: {[x['sep'] for x in f['./en.h']]}")
text = files["en.h"]
check([x["key"] for x in f["./en.h"]] == [text.index("enum"), text.index("  E_B"), text.index("};")],
      f"en.h keys are just after the directive line before each fragment: {[x['key'] for x in f['./en.h']]}")
e3 = sp["./en.h"]
check(len(e3) == 1 and e3[0]["defines"] == 3 and e3[0]["key"] == text.index("#define EN_H") and
      e3[0]["sep"] == SEP_FIRST | SEP_COND,
      f"en.h as one option-3 span from its guard's #define: {e3}")
check(d["full_config"] != d["config"] and d["pragma_tokens"] == 0 and d["line_directives"] == 0,
      "a full configuration key; no pragma tokens or #line")

p = {s: record(s)[1]["./plain.h"][0] for s in ("v1.c", "v2.c", "o1.c", "at1.c", "wk1.c", "pv1.c", "pk1.c")}
p["a1.c"] = record("a1.c")[1]["./an.h"][0]
check(p["v1.c"]["pragma"] == p["v2.c"]["pragma"] and p["v1.c"]["prag"] != p["v2.c"]["prag"],
      "#pragma GCC visibility: the old pragma state misses it, the full one does not")
check(len({p[s]["prag"] for s in ("v2.c", "a1.c", "o1.c", "at1.c")}) == 4,
      "assume_nonnull, clang optimize off and an active clang attribute each change the full pragma state")
check(p["at1.c"]["server"] & SV_ATTRIBUTE and p["wk1.c"]["server"] & SV_WEAK and not p["v2.c"]["server"],
      f"#pragma clang attribute active, #pragma weak pending: flags {p['at1.c']['server']}, {p['wk1.c']['server']}")
check(p["pv1.c"]["server"] & SV_PENDING and p["pk1.c"]["server"] & SV_PENDING and
      not p["v1.c"]["server"] & SV_PENDING,
      "a header right after #pragma GCC visibility or #pragma pack starts while the pragma is pending; "
      "not with a declaration between")

d, f, _, _ = record("b1.c")
check(f["./cnt.h"][0]["server"] & SV_BUILTIN and f["./prg.h"][0]["server"] & SV_BUILTIN,
      "__COUNTER__ and _Pragma: never reusable")
ln = f["./ln.h"]
check(not ln[0]["server"] & SV_BUILTIN and ln[-1]["server"] & SV_BUILTIN and d["line_directives"] == 1,
      f"__LINE__ and __FILE__ before #line are fixed, __LINE__ after it is not: {[x['server'] for x in ln]}")

_, f, _, _ = record("g1.c")
w = [x for x in f["./args.h"] if not x["empty"]]
check(w and w[-1]["server"] & SV_ARGS, f"`) int w;` starts at an #endif inside ID's arguments: "
      f"{[(x['start'], x['server']) for x in f['./args.h']]}")

_, f, _, _ = record("g2.c")
a2 = [x for x in f["./args2.h"] if not x["empty"]]
check([x["server"] & SV_ARGS for x in a2] == [SV_ARGS, SV_ARGS, 0],
      f"`int a0; DECL(` ends inside DECL's arguments, `) int c0;` starts inside them, `int d0;` is outside: "
      f"{[(x['start'], x['server']) for x in f['./args2.h']]}")

_, f, _, _ = record("k1.c")
kr = [(x["before"], x["after"], x["sbefore"], x["safter"]) for x in f["./kr.h"]]
check(kr == [(1, 1, 1, 0), (1, 1, 0, 1)], f"kr.h: the old observer reads the directive before the K&R body's "
      f"'{{' as a boundary, the fixed one does not: {kr}")

_, f, _, _ = record("c1.c")
check((0, "done") in f["./clu.h"][0]["refs"], f"clu.h references cleanup's done: {f['./clu.h'][0]['refs']}")
und = f["./und.h"]
check(und[0]["server"] & SV_UNDEFINED and not und[1]["server"] & SV_UNDEFINED,
      f"und.h: static and inline functions declared without a body, then a definition and an extern: "
      f"{[x['server'] for x in und]}")

_, f, _, _ = record("c2.c")
check(not f["./undd.h"][0]["server"] & SV_UNDEFINED, "undd.h: a prototype its own fragment defines is not flagged")
check(f["./undx.h"][0]["server"] & 96 == SV_UNDEFINED,
      f"undx.h: a prototype defined in a later fragment: SV_UNDEFINED, not _END: {f['./undx.h'][0]['server']}")
pp = f["./pp.h"]
check(pp[0]["server"] & SV_PENDING and pp[0]["prag"] != "0000000000000000" and pp[0]["prag"] != pp[1]["prag"],
      f"pp.h: struct PP starts while #pragma pack is pending, with its pragma state recorded: "
      f"{[(x['server'], x['prag']) for x in pp]}")
nd = [(x["sbefore"], x["safter"], x["before"], x["after"]) for x in f["./nd.h"]]
check(nd == [(1, 0, 1, 0), (0, 1, 0, 1)], f"nd.h: the null directive inside struct ns is inside a declaration: {nd}")
a3 = [x["server"] & SV_ARGS for x in f["./args3.h"] if not x["empty"]]
check(a3 == [SV_ARGS, SV_ARGS], f"args3.h: CALL(...)'s arguments, CALL expanding to FOO: {a3}")
check((0, "vf4") in f["./cvu.h"][0]["refs"], f"cvu.h references convertvector's vf4: {f['./cvu.h'][0]['refs']}")
check((1, "bx") in f["./bt.h"][0]["refs"], f"bt.h: struct bx, first named at block scope: {f['./bt.h'][0]['refs']}")
k2 = [(x["sbefore"], x["safter"]) for x in f["./kr2.h"]]
check(k2 == [(1, 0), (0, 0), (0, 1)], f"kr2.h: directives between K&R parameter declarations: {k2}")
k3 = [(x["sbefore"], x["safter"]) for x in f["./kr3.h"] if not x["empty"]][-2:]
check(k3 == [(1, 0), (0, 1)], f"kr3.h: the #if before a K&R definition stays a boundary: {k3}")

_, f, _, main = record("d1.c")
dcl = f["./dcl.h"][0]
check(dcl["binds"] == [(0, "da"), (1, "T"), (1, "N"), (0, "E1"), (0, "E2"), (0, "I"), (1, "U"), (0, "up")],
      f"dcl.h declares, in order: {dcl['binds']}")
use = f["./use.h"][0]
check(use["refs"] == [(0, "E2"), (0, "da"), (0, "g"), (1, "T")] or use["refs"] == [(0, "E2"), (0, "da"), (1, "T")],
      f"use.h references da, E2 and struct T: {use['refs']}")
check(use["binds"] == [(0, "g")], f"use.h declares g: {use['binds']}")
check([n for _, _, n in main] == ["m1", "m2"] and main[0][0] < dcl["order"] < use["order"] < main[1][0],
      f"the main file's declarations, in order around the headers: {main}")

_, _, _, _ = record("n1.c")
c1 = record("n1.c")[0]
c2 = record("n1.c", ["-fno-spell-checking"])[0]
check(c1["config"] == c2["config"] and c1["full_config"] != c2["full_config"],
      "a benign language option: the model's configuration is the same, the server's key is not")
print("all ok" if ok else "FAILURES")
sys.exit(0 if ok else 1)
