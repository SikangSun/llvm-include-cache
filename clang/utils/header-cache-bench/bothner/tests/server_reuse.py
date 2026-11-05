"""The compile server's reuse checked on small cases (COMPILE_SERVER_DESIGN.md
section 5): the paper's cases and the cross-compile ones.

Each case is a few files and a sequence of steps: compile a .c file with some
flags, or edit a file (the next compile then starts a new build, so the
server checks its files, 2.10). Every compile runs first as plain clang, which
writes its LLVM IR (-O0, no LLVM passes) and BothnerServer.so's declaration
summary, and then through one server with reuse on, which writes the same two.
They must be byte-identical, and the server's diagnostics must be the text
plain clang printed. The sequence of each case runs twice, so that the
second round can reuse what the first parsed. A case may also require reuse
to have happened, or never to happen in some compile.

usage: server_reuse.py <workdir> [case ...]
env: BIN (clang's bin directory; default build_assert/bin of this checkout),
     SO (BothnerServer.so built against BIN), OPTION (2, the default, or 3:
     #define and #undef lines join fragments, design 2.7), FRAGMENT_GROUPS (1:
     conditionals join fragments while the parser is nested, 2.7)
"""
import json, os, shlex, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "../../../../.."))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build_assert/bin"))
CLANG = os.path.join(BIN, "clang")
SO = os.environ.get("SO", os.path.join(HERE, "..", "BothnerServer.so"))
ENV = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "BOTHNER_"))}
FLAGS = ["-std=gnu11"]
OPTION = os.environ.get("OPTION", "2")
GROUPS = os.environ.get("FRAGMENT_GROUPS") == "1"
sys.path.insert(0, os.path.join(HERE, ".."))
import diagtext  # noqa: E402

# name: (files, steps, expectations). A step is ("c", file, [flags]) or
# ("edit", file, text). Expectation "reuse": some compile of the second round
# reused a fragment; ("never", file, header): no compile of that .c file
# reused a fragment of the header named; ("reused", file, header): some
# compile of that .c file in the second round reused a fragment of the header
# named; ("refused", file, round): the server
# must refuse that compile in that round (status 2), and the driver then
# compiles it itself.
CASES = {
    # p.26: D1/D2 under M1/M2, and a guarded header.
    "m1m2": ({
        "h.h": "#ifndef H_H\n#define H_H\ntypedef int D1;\n#if M1\ntypedef long D2;\n#else\ntypedef short D2;\n#endif\nD2 h_get(void);\n#endif\n",
        "a.c": "#define M1 1\n#include \"h.h\"\nD2 x = 1;\nD1 y;\nD2 h_get(void) { return x; }\n",
        "b.c": "#define M1 0\n#include \"h.h\"\nD2 x = 2;\nD2 h_get(void) { return x; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "a.c", [])], ["reuse"]),
    # p.27 and p.29: a macro's value changes what a fragment means.
    "macro_value": ({
        "h1.h": "int buf[BUFSZ];\nint f(int a[BUFSZ]);\n",
        "a.c": "#define BUFSZ 10\n#include \"h1.h\"\n_Static_assert(sizeof(buf) == 40, \"\");\n",
        "b.c": "#define BUFSZ 20\n#include \"h1.h\"\n_Static_assert(sizeof(buf) == 80, \"\");\n",
        "s.h": "int strcmp(const char *, const char *);\nint usecmp(const char *a) { return strcmp(a, \"x\"); }\n",
        "c.c": "#define strcmp strcmp\n#include \"s.h\"\n",
        "d.c": "#include \"s.h\"\n",
        "v.h": "int i;\nint *vp = &i;\n",
        "e.c": "#define i j\n#include \"v.h\"\nint *p = &j;\n",
        "f.c": "#include \"v.h\"\nint *p = &i;\n",
        "t.h": "T tv;\nT tf(T);\n",
        "g.c": "#define T int\n#include \"t.h\"\n",
        "h.c": "#define T long\n#include \"t.h\"\n",
    }, [("c", x, []) for x in ("a.c", "b.c", "c.c", "d.c", "e.c", "f.c", "g.c", "h.c")], []),
    # p.28: a tag first declared in a parameter list.
    "param_tag": ({
        "p.h": "void pf(struct s *p);\n",
        "q.h": "struct s { int a; };\n",
        "a.c": "#include \"p.h\"\n#include \"q.h\"\nstruct s sx;\nint use(void) { return sx.a; }\n",
    }, [("c", "a.c", [])], [("never", "a.c", "p.h")]),
    # p.32: an enum with #define lines (option 2 splits it).
    "enum_defines": ({
        "e.h": "enum e {\n  E_A,\n#define E_A E_A\n  E_B,\n#define E_B E_B\n};\nint e_val(enum e);\nint e_more;\n",
        "a.c": "#include \"e.h\"\nint k = E_B;\nint use(void) { return e_val(E_A) + e_more; }\n",
    }, [("c", "a.c", [])], []),
    # p.33: struct timestamp, a struct with #if lines inside.
    "struct_if": ({
        "t.h": "struct timestamp {\n  int sec;\n#if WITH_NS\n  long ns;\n#endif\n};\nstruct timestamp ts_now(void);\n",
        "a.c": "#define WITH_NS 1\n#include \"t.h\"\n_Static_assert(sizeof(struct timestamp) == 16, \"\");\nlong g(void) { return ts_now().ns; }\n",
        "b.c": "#define WITH_NS 0\n#include \"t.h\"\n_Static_assert(sizeof(struct timestamp) == 4, \"\");\nint g(void) { return ts_now().sec; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # p.33: stdio.h and stdlib.h, each defining size_t, in either order.
    "size_t_order": ({
        "st1.h": "#ifndef SZ\n#define SZ\ntypedef unsigned long size_t;\n#endif\nsize_t f1(void);\n",
        "st2.h": "#ifndef SZ\n#define SZ\ntypedef unsigned long size_t;\n#endif\nsize_t f2(void);\n",
        "a.c": "#include \"st1.h\"\n#include \"st2.h\"\nsize_t g(void) { return f1() + f2(); }\n",
        "b.c": "#include \"st2.h\"\n#include \"st1.h\"\nsize_t g(void) { return f1() + f2(); }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    # The hidden-fragment tests: an asm label, #pragma weak, #pragma pack.
    "asm_label": ({
        "al.h": "int foo(void) __asm__(\"bar\");\n",
        "a.c": "#include \"al.h\"\nint call(void) { return foo(); }\n",
    }, [("c", "a.c", [])], ["reuse"]),
    "pragma_weak": ({
        "w.h": "int wfun(void);\nint wvar;\n",
        "a.c": "#include \"w.h\"\n#pragma weak wfun\nint use(void) { return wfun() + wvar; }\n",
        "b.c": "#include \"w.h\"\nint use(void) { return wfun() + wvar; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    "pragma_pack": ({
        "pk.h": "struct pk { char c; int i; };\nstruct pk pk_make(void);\n",
        "a.c": "#pragma pack(push, 1)\n#include \"pk.h\"\n#pragma pack(pop)\n_Static_assert(sizeof(struct pk) == 5, \"\");\nint g(void) { return pk_make().i; }\n",
        "b.c": "#include \"pk.h\"\n_Static_assert(sizeof(struct pk) == 8, \"\");\nint g(void) { return pk_make().i; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # A struct completed in one compile and not in the next.
    "complete_then_not": ({
        "fwd.h": "struct s;\nstruct s *sp;\n",
        "def.h": "struct s { int x; };\n",
        "a.c": "#include \"fwd.h\"\n#include \"def.h\"\nint g(void) { return sp->x; }\n",
        "b.c": "#include \"fwd.h\"\nint g(void) { return sp != 0; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    # Two different definitions, then sizeof through a common header.
    "two_definitions": ({
        "d1.h": "struct t { int a; };\n",
        "d2.h": "struct t { long a; long b; };\n",
        "c.h": "struct t *tp;\nstatic inline unsigned long tsize(void) { return sizeof(struct t); }\n",
        "a.c": "#include \"d1.h\"\n#include \"c.h\"\n_Static_assert(sizeof(struct t) == 4, \"\");\nunsigned long g(void) { return tsize(); }\n",
        "b.c": "#include \"d2.h\"\n#include \"c.h\"\n_Static_assert(sizeof(struct t) == 16, \"\");\nunsigned long g(void) { return tsize(); }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # A definition in each of two compiles.
    "definition_each": ({
        "g.h": "static int gv = 1;\nstatic inline int gf(void) { return gv; }\nint gd = 5;\n",
        "a.c": "#include \"g.h\"\nint a(void) { return gf(); }\n",
        "b.c": "#include \"g.h\"\nint b(void) { return gf() + gd; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    # A block-scope extern in a header's function, then a file-scope one.
    "block_extern": ({
        "be.h": "static inline int bef(void) { extern int ext; return ext; }\n",
        "a.c": "#include \"be.h\"\nint a(void) { return bef(); }\n",
        "b.c": "#include \"be.h\"\nint ext = 3;\nint b(void) { return bef(); }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # The FILE typedef present, then absent.
    "file_typedef": ({
        "f.h": "typedef struct _IO_FILE FILE;\nstruct _IO_FILE { int fd; };\nint fprintf(FILE *, const char *, ...);\nextern FILE *errf;\n",
        "a.c": "#include \"f.h\"\nint a(void) { return fprintf(errf, \"x\"); }\n",
        "b.c": "int fputs(const char *, void *);\nint b(void *f) { return fputs(\"x\", f); }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    # Tokens after an include guard's #endif: the header is not guarded.
    "after_guard": ({
        "g.h": "#ifndef G_H\n#define G_H\nint ga;\n#endif\nint gafter;\n",
        "a.c": "#include \"g.h\"\n#include \"g.h\"\nint *p = &gafter;\n",
    }, [("c", "a.c", [])], ["reuse"]),
    # A directive between decl-specifiers.
    "declspec_directive": ({
        "ds.h": "extern\n#if 1\nint\n#endif\ndsv;\nint dsafter;\n",
        "a.c": "#include \"ds.h\"\nint *p = &dsv;\n",
    }, [("c", "a.c", [])], []),
    # #pragma GCC visibility push before an #include.
    "visibility": ({
        "v.h": "int vfun(void);\nextern int vvar;\n",
        "a.c": "#pragma GCC visibility push(hidden)\n#include \"v.h\"\n#pragma GCC visibility pop\nint use(void) { return vfun() + vvar; }\n",
        "b.c": "#include \"v.h\"\nint use(void) { return vfun() + vvar; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # A directive right after a function whose parameter is named like a
    # header global.
    "param_like_global": ({
        "pg1.h": "int g;\n",
        "pg2.h": "static inline int pf(int g) { return g; }\n#include \"pg3.h\"\n",
        "pg3.h": "int pg3 = 0;\nint *gp = &g;\n",
        "a.c": "#include \"pg1.h\"\n#include \"pg2.h\"\nint use(void) { return pf(g) + pg3 + *gp; }\n",
    }, [("c", "a.c", [])], ["reuse"]),
    # A header static declared without a definition (2.8).
    "static_undefined": ({
        "su.h": "static int helper(void);\nstatic inline int callh(void) { return helper(); }\n",
        "su2.h": "static int helper(void) { return 7; }\n",
        "a.c": "#include \"su.h\"\n#include \"su2.h\"\nint f(void) { return callh(); }\n",
        "b.c": "#include \"su.h\"\n#include \"su2.h\"\nint f(void) { return callh() + helper(); }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], [("never", "a.c", "su.h"), ("never", "b.c", "su.h")]),
    # A header edited between builds: same size with a new mtime, then a new
    # size; and a .c file edited.
    "edits": ({
        "e.h": "int ea = 1;\n",
        "a.c": "#include \"e.h\"\nint *p = &ea;\n",
        "b.c": "#include \"e.h\"\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("edit", "e.h", "int eb = 1;\n"), ("c", "b.c", []),
        ("edit", "e.h", "int ecc = 12;\n"), ("c", "b.c", []), ("edit", "a.c", "#include \"e.h\"\nint *p = &ecc;\n"),
        ("c", "a.c", []), ("edit", "e.h", "int ea = 1;\n"), ("edit", "a.c", "#include \"e.h\"\nint *p = &ea;\n"),
        ("c", "a.c", [])], []),
    # A directive inside macro arguments.
    "macro_args": ({
        "ma.h": "#define CALL(x) x\nint before_ma;\nCALL(\n#ifdef Y\nint ma1\n#else\nint ma2\n#endif\n);\nint after_ma;\n",
        "a.c": "#define Y\n#include \"ma.h\"\nint *p = &ma1;\n",
        "b.c": "#include \"ma.h\"\nint *p = &ma2;\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # __COUNTER__, and __LINE__ after #line.
    "counter_line": ({
        "cl.h": "int c1 = __COUNTER__;\nint cl_plain;\n#line 100 \"other.h\"\nint l1 = __LINE__;\n",
        "a.c": "int before = __COUNTER__;\n#include \"cl.h\"\n",
        "b.c": "#include \"cl.h\"\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], [("never", "a.c", "cl.h"), ("never", "b.c", "cl.h")]),
    # A redeclaration chain across compiles: a restored prototype, then a
    # definition in the .c file, then the header's definition.
    "chain": ({
        "r1.h": "int rf(int);\n",
        "r2.h": "int rf(int x);\nint rf(int x) { return x; }\n",
        "a.c": "#include \"r1.h\"\n#include \"r2.h\"\nint use(void) { return rf(1); }\n",
        "b.c": "#include \"r1.h\"\nint rf(int x) { return x + 1; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    # A tentative definition of incomplete array type, then a complete one.
    "tentative_array": ({
        "ta.h": "int arr[];\n",
        "a.c": "#include \"ta.h\"\nint *p = arr;\n",
        "b.c": "#include \"ta.h\"\nint arr[5];\nint n = sizeof(arr);\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # Two configurations in one server, and library builtins declared by a
    # header in one compile and used without it in the next.
    "configs_builtins": ({
        "str.h": "typedef unsigned long size_t;\nvoid *memcpy(void *, const void *, size_t);\nsize_t strlen(const char *);\n",
        "a.c": "#include \"str.h\"\nvoid c(char *d, const char *s) { memcpy(d, s, strlen(s)); }\n",
        "b.c": "void c(char *d, const char *s) { __builtin_memcpy(d, s, 4); }\n",
    }, [("c", "a.c", []), ("c", "a.c", ["-funsigned-char"]), ("c", "b.c", []), ("c", "a.c", [])], ["reuse"]),
    # A name the parse looks up but the AST drops: the enumerator in a
    # vector_size argument (decision 9, test vs of the simulator).
    "vector_size": ({
        "n.h": "enum { N = VAL };\n",
        "v.h": "typedef int vs __attribute__((vector_size(N * sizeof(int))));\nint vsz = sizeof(vs);\n",
        "a.c": "#define VAL 4\n#include \"n.h\"\n#include \"v.h\"\n",
        "b.c": "#define VAL 2\n#include \"n.h\"\n#include \"v.h\"\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # One header reached by two paths: __FILE__, __builtin_FILE(), the debug
    # information and a quoted #include's first directory follow the path
    # the compile used.
    "two_paths": ({
        "inc/f.h": "static const char *fname = __FILE__;\nint fget(int i) { return fname[i]; }\n",
        "inc/g.h": "int gtwice(int x) { return 2 * x; }\n#include \"n.h\"\n",
        "inc/n.h": "static const char *nname = __FILE__;\nint nget(int i) { return nname[i]; }\n",
        "inc/h.h": "static const char *bfile = __builtin_FILE();\nint hget(int i) { return bfile[i]; }\n",
        "a.c": "#include \"inc/f.h\"\n#include \"inc/g.h\"\n#include \"inc/h.h\"\n",
        "sub/b.c": "#include \"../inc/f.h\"\n#include \"../inc/g.h\"\n#include \"../inc/h.h\"\n",
    }, [("c", "a.c", []), ("c", "a.c", ["-g"]), ("c", "sub/b.c", []), ("c", "sub/b.c", ["-g"])],
       ["reuse", ("never", "a.c", "h.h"), ("never", "sub/b.c", "h.h")]),
    # A struct the debug information first saw incomplete, then completed by
    # a restored definition: the code generator hears of the completion.
    "debug_completion": ({
        "fw.h": "struct dc;\nvoid fwf(struct dc *p) {}\n",
        "df.h": "struct dc { int a; struct { int b; } in; };\nenum de { DE_A, DE_B };\n",
        "a.c": "#include \"fw.h\"\n#include \"df.h\"\nint g(struct dc *s) { return s->a + s->in.b + DE_B; }\n",
    }, [("c", "a.c", ["-g"]), ("c", "a.c", ["-g"])], ["reuse"]),
    # Section names of one compile must not conflict with the next's (the
    # ASTContext records each section's kind).
    "sections": ({
        "sec.h": "static const int seca __attribute__((section(\"sx\"))) = 1;\nstatic inline int secf(void) { return seca; }\n",
        "a.c": "#include \"sec.h\"\nconst int ca __attribute__((section(\"sy\"))) = 2;\nint a(void) { return secf() + ca; }\n",
        "b.c": "int vb __attribute__((section(\"sy\"))) = 3;\nint vx __attribute__((section(\"sx\"))) = 4;\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "a.c", [])], ["reuse"]),
    # A struct body with a directive inside, whose member declares a new tag
    # at file scope; then a header that declares the same tag (curl's
    # struct curl_thrdq, git's struct strmap).
    "tag_in_split_body": ({
        "m.h": "struct holder {\n  int a;\n#define HX 1\n  struct fwd *p;\n  enum { HE_A, HE_B } e;\n};\n",
        "t.h": "struct fwd;\nint tf(struct fwd *);\nenum { HE_C = 7 };\n",
        "a.c": "#include \"t.h\"\n#include \"m.h\"\nint a(struct holder *h) { struct fwd *q = h->p; return tf(q) + HE_B; }\n",
        "b.c": "#include \"m.h\"\n#include \"t.h\"\nint b(struct holder *h) { struct fwd *q = h->p; return tf(q) + HE_C; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "a.c", [])], []),
    # A function used in one compile, then given an asm label after a
    # restored declaration in the next (glibc's __REDIRECT_NTH after a plain
    # declaration): the Used bit of the first compile must not leak; and a
    # restored body's use must count as a parse's would.
    "used_bit": ({
        "d.h": "unsigned long long sx(const char *);\n",
        "r.h": "#ifdef REDIRECT\nunsigned long long sx(const char *) __asm__(\"sx_new\");\n#endif\nint rr;\n",
        "u.h": "static inline unsigned long long usx(void) { return sx(\"1\"); }\n",
        "a.c": "#include \"d.h\"\n#include \"r.h\"\nunsigned long long a(void) { return sx(\"7\"); }\n",
        "b.c": "#define REDIRECT\n#include \"d.h\"\n#include \"r.h\"\nunsigned long long b(void) { return sx(\"8\"); }\n",
        "c.c": "#include \"d.h\"\n#include \"u.h\"\nunsigned long long c(void) { return usx(); }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "c.c", []), ("c", "b.c", [])], ["reuse"]),
    # Macro definitions with the same body tokens that still differ: the
    # order of the parameters, and the spaces a stringification keeps.
    "macro_identity": ({
        "mi.h": "#define STR(x) #x\n#define XSTR(x) STR(x)\nint selv = SEL(10, 20);\nconst char *opv = XSTR(OP);\n",
        "a.c": "#define SEL(x, y) x\n#define OP a+b\n#include \"mi.h\"\n",
        "b.c": "#define SEL(y, x) x\n#define OP a + b\n#include \"mi.h\"\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], []),
    # Option 3: #define and #undef lines between declarations, a macro the
    # header redefines, one a .c file defined first, and the guard.
    "macro_lines": ({
        "o3.h": "#ifndef O3_H\n#define O3_H\nenum { O3A,\n#define O3A O3A\n O3B };\n"
                "#define O3_TWICE(x) ((x) * 2)\nint o3v = O3_TWICE(O3B);\n#undef O3_TWICE\n"
                "#define O3_TWICE(x) ((x) + (x) + 1)\nint o3w = O3_TWICE(3);\n"
                "#define O3_ALIAS OUTSIDE_VAL\nint o3x = O3_ALIAS;\n#endif\n",
        "a.c": "#define OUTSIDE_VAL 1\n#include \"o3.h\"\n#include \"o3.h\"\nint a(void) { return o3v + o3w + o3x + O3A; }\n"
               "#ifdef O3_TWICE\nint t = O3_TWICE(5);\n#endif\n",
        "b.c": "#define OUTSIDE_VAL 2\n#define O3_TWICE(x) 0\n#include \"o3.h\"\nint b(void) { return O3_TWICE(4) + O3_ALIAS; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "a.c", [])], ["reuse"]),
    # Groups: conditionals inside a struct body, under macros that change
    # between compiles, tested by #if, #ifdef and defined(); a nested #if, a
    # false block with its own conditionals, and an #elif.
    "struct_groups": ({
        "sg.h": "struct sg {\n  int a;\n#ifdef SG_B\n  int b;\n#endif\n#if defined(SG_C) && SG_C > 1\n  long c;\n"
                "#elif SG_D\n  short d;\n#else\n  char e;\n#endif\n#if 0\n#if SG_B\n  int never;\n#endif\n#endif\n};\n"
                "int sgsize = sizeof(struct sg);\n",
        "a.c": "#define SG_B\n#define SG_C 2\n#include \"sg.h\"\nint a(struct sg *p) { return p->b + (int)p->c; }\n",
        "b.c": "#define SG_D 1\n#include \"sg.h\"\nint b(struct sg *p) { return p->d; }\n",
        "c.c": "#include \"sg.h\"\nint c(struct sg *p) { return p->e; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "c.c", []), ("c", "a.c", []), ("c", "a.c", [])],
       # Every fragment of sg.h starts or ends inside the struct body, so
       # only groups can reuse.
       ["reuse"] if GROUPS else []),
    # A dependency file (-MD), with headers parsed, reused and skipped by
    # their guards.
    "dependency_file": ({
        "dep.h": "#ifndef DEP_H\n#define DEP_H\nint depv;\n#endif\n",
        "dep2.h": "#include \"dep.h\"\nint dep2v;\n",
        "a.c": "#include \"dep.h\"\n#include \"dep2.h\"\nint *p = &depv;\n",
    }, [("c", "a.c", ["-MD"]), ("c", "a.c", ["-MD"])], ["reuse"]),
    # A #line that only some compiles see: it moves the lines and the name of
    # what follows, for __LINE__ and for the debug information.
    "line_moved": ({
        "lm.h": "#ifdef MOVE\n#line 500\n#endif\nstruct lm { int a; };\nstatic inline int lmf(void) { return __LINE__; }\nint lmv;\n",
        "ln.h": "#ifdef MOVE\n#line 700 \"moved.h\"\n#endif\nstruct ln { int b; };\nint lnv;\n",
        "a.c": "#include \"lm.h\"\n#include \"ln.h\"\nint a(struct lm *s, struct ln *t) { return s->a + t->b + lmf() + lmv + lnv; }\n",
        "b.c": "#define MOVE\n#include \"lm.h\"\n#include \"ln.h\"\nint b(struct lm *s, struct ln *t) { return s->a + t->b + lmf() + lmv + lnv; }\n",
    }, [("c", "a.c", ["-g"]), ("c", "b.c", ["-g"]), ("c", "a.c", ["-g"])], ["reuse"]),
    # A #pragma GCC system_header that only some compiles see: profile
    # instrumentation skips the functions of a system header.
    "system_pragma": ({
        "sp.h": "#ifdef SYS\n#pragma GCC system_header\n#endif\nint spf(int x) { return x + 1; }\nint spv;\n",
        "a.c": "#include \"sp.h\"\nint a(void) { return spf(spv); }\n",
        "b.c": "#define SYS\n#include \"sp.h\"\nint b(void) { return spf(spv); }\n",
    }, [("c", "a.c", ["-fprofile-instr-generate"]), ("c", "b.c", ["-fprofile-instr-generate"]), ("c", "a.c", ["-fprofile-instr-generate"])], ["reuse"]),
    # Diagnostics: a reuse prints nothing, so a fragment that printed a
    # warning is parsed every time, and a reuse needs the warning options
    # and #pragma diagnostic state its parse had.
    "warning_reuse": ({
        "w.h": "int wv;\n#include \"e.h\"\nstatic inline int wsh(void) { return 1 << 40; }\n",
        "e.h": "",
        "a.c": "#include \"w.h\"\nint a(void) { return wsh() + wv; }\n",
        "b.c": "#include \"w.h\"\nint b(void) { return wsh() + wv; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse"]),
    "warning_options": ({
        "w.h": "int wv;\nstatic inline int wsh(void) { return 1 << 40; }\n",
        "a.c": "#include \"w.h\"\nint a(void) { return wsh() + wv; }\n",
    }, [("c", "a.c", ["-Wno-shift-count-overflow"]), ("c", "a.c", []), ("c", "a.c", ["-w"]),
        ("c", "a.c", ["-Wno-shift-count-overflow"])], ["reuse"]),
    "warning_pragma": ({
        "w.h": "int wv;\nstatic inline int wsh(void) { return 1 << 40; }\n",
        "a.c": "#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Wshift-count-overflow\"\n"
               "#include \"w.h\"\n#pragma clang diagnostic pop\nint a(void) { return wsh() + wv; }\n",
        "b.c": "#include \"w.h\"\nint b(void) { return wsh() + wv; }\n",
        "c.c": "#pragma GCC diagnostic ignored \"-Wshift-count-overflow\"\n#include \"w.h\"\nint c(void) { return wsh() + wv; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "c.c", []), ("c", "a.c", [])], ["reuse"]),
    # -Wpadded reports where a struct is first laid out in the compile.
    "padded": ({
        "p.h": "struct pe { char c; int i; };\n#include \"e.h\"\nint pv;\n",
        "e.h": "",
        "a.c": "#include \"p.h\"\nint a = sizeof(struct pe);\n",
        "b.c": "#include \"p.h\"\nint b(void) { return pv; }\n",
    }, [("c", "b.c", ["-Wpadded"]), ("c", "a.c", ["-Wpadded"]), ("c", "b.c", ["-Wpadded"])], ["reuse"]),
    # Option 3: a #define in a fragment, where the macro had another
    # definition before (-Wmacro-redefined), or an unused one of the main
    # file (-Wunused-macros).
    "macro_redefined": ({
        "h.h": "int hv;\n#define HM 1\nint hw;\n",
        "a.c": "#include \"h.h\"\nint a(void) { return hv + hw + HM; }\n",
        "b.c": "#define HM 2\n#include \"h.h\"\nint b(void) { return hv + hw + HM; }\n",
        "c.c": "#define HM 1\n#include \"h.h\"\nint c(void) { return hv + hw; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "c.c", ["-Wunused-macros"]), ("c", "a.c", [])], ["reuse"]),
    # A static function a restored fragment refers to only where it is not
    # evaluated: referenced, not used, so plain clang says it is not needed
    # (-Wunneeded-internal-declaration), not unused.
    "referenced_bit": ({
        "rb.h": "static int rf(void) { return 1; }\nstatic int rv = sizeof(rf());\n#include \"e.h\"\nint hv;\n",
        "e.h": "",
        "a.c": "#include \"rb.h\"\nint a(void) { return rf() + rv + hv; }\n",
        "b.c": "#include \"rb.h\"\nint b(void) { return rv + hv; }\n",
    }, [("c", "a.c", ["-Wunused-function"]), ("c", "b.c", ["-Wunused-function"])], ["reuse"]),
    # A name a pragma of the compile marked: a deprecated macro warns where
    # a fragment expands it.
    "pragma_marked_name": ({
        "dm.h": "int dv;\n#include \"e.h\"\nstatic inline int df(void) { return DM + dv; }\n",
        "e.h": "",
        "a.c": "#define DM 1\n#include \"dm.h\"\nint a(void) { return df(); }\n",
        "b.c": "#define DM 1\n#pragma clang deprecated(DM)\n#include \"dm.h\"\nint b(void) { return df(); }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "a.c", [])], ["reuse"]),
    # The token count that -Wmax-tokens compares includes reused fragments.
    "max_tokens": ({
        "mt.h": "int t1, t2, t3;\n#include \"e.h\"\nint t4(int a) { return a + t1 + t2 + t3; }\n",
        "e.h": "",
        "a.c": "#include \"mt.h\"\n#pragma clang max_tokens_here 5\nint a(void) { return t4(1); }\n",
        "b.c": "#include \"mt.h\"\n#pragma clang max_tokens_here 5\nint b(void) { return t4(2); }\n",
    }, [("c", "a.c", ["-Wmax-tokens"]), ("c", "b.c", ["-Wmax-tokens"]), ("c", "a.c", ["-Wmax-tokens"])], ["reuse"]),
    # A documentation command one compile found unknown must be unknown in
    # the next compile too.
    "doc_command": ({
        "dc.h": "int dcv;\n#include \"e.h\"\n/**\n * Returns {@literal >=} 0.\n */\nint dcf(int x);\n",
        "e.h": "",
        "a.c": "#include \"dc.h\"\nint a(void) { return dcf(dcv); }\n",
        "b.c": "#include \"dc.h\"\nint b(void) { return dcf(dcv); }\n",
    }, [("c", "a.c", ["-Wdocumentation", "-Wdocumentation-unknown-command"]),
        ("c", "b.c", ["-Wdocumentation", "-Wdocumentation-unknown-command"])], ["reuse"]),
    # A warning that points into a restored declaration later (here -Wpadded
    # at the first use) gets the state of the warning options, so a fragment
    # parsed under a #pragma diagnostic is never reused.
    "pragma_late": ({
        "pl.h": "int plv;\n#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wpadded\"\n"
                "struct pl { char c; int i; };\n#pragma GCC diagnostic pop\nint plw;\n",
        "a.c": "#include \"pl.h\"\nint a = sizeof(struct pl);\n",
        "b.c": "#include \"pl.h\"\nint b(void) { return plv + plw; }\n",
    }, [("c", "b.c", ["-Wpadded"]), ("c", "a.c", ["-Wpadded"]), ("c", "a.c", ["-Wpadded"])], ["reuse"]),
    # C89 implicit function declarations inside a header's function, then in
    # the .c file.
    # Option 3: what a #define line finds before it decides only whether it
    # reports a redefinition. glibc's bits/wordsize.h, only #define lines and
    # read several times a compile, makes the same definitions each time: the
    # first reading finds none, a later one its own. The store keeps the
    # later reading's version, and the next compile's first reading reuses
    # it. A different earlier definition (c.c) is reported, and the fragment
    # parsed. (Option 2 has no fragment there.)
    "prior_repeat": ({
        "w.h": "#define WSIZE 64\n#define WSIZE_COMPAT 1\n",
        "a.c": "#include \"w.h\"\n#include \"w.h\"\nint a(void) { return WSIZE + WSIZE_COMPAT; }\n",
        "b.c": "#define WSIZE 64\n#include \"w.h\"\nint b(void) { return WSIZE_COMPAT; }\n",
        "c.c": "#define WSIZE 32\n#include \"w.h\"\nint c(void) { return WSIZE; }\n",
    }, [("c", "a.c", []), ("c", "b.c", []), ("c", "c.c", []), ("c", "a.c", [])],
        ["reuse", ("reused", "a.c", "w.h"), ("reused", "b.c", "w.h")] if OPTION == "3" else []),
    # An #undef line does not compare what it removes with anything.
    "prior_undef": ({
        "u.h": "extern int uv;\n#undef UX\n#define UX 1\nextern int uw;\n",
        "a.c": "#define UX 5\n#include \"u.h\"\nint a(void) { return uv + UX; }\n",
        "b.c": "#include \"u.h\"\nint b(void) { return uw + UX; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], ["reuse", ("reused", "a.c", "u.h")]),
    # Source locations are 32-bit and never freed (2.2). a.c takes 1.2 GiB of
    # them: one compile fits, and plain clang compiles it, but the second in
    # one server does not. Clang stops that compile with a fatal error (2.10),
    # so the server must refuse it, and b.c must still be served correctly.
    "space_ends": ({
        "big.h": "/*\n" + ("x" * 1023 + "\n") * 4096 + "*/\n",
        "many.h": "#include \"big.h\"\n" * 300,
        "a.c": "#include \"many.h\"\nint a;\n",
        "b.c": "int b(void) { return 1; }\n",
    }, [("c", "a.c", []), ("c", "b.c", [])], [("refused", "a.c", 2)]),
    "implicit_c89": ({
        "im.h": "static int imf(void) { return undeclared_fn(1); }\nint imv;\n",
        "a.c": "#include \"im.h\"\nint a(void) { return imf() + undeclared_fn(2) + imv; }\n",
    }, [("c", "a.c", ["-std=gnu89"]), ("c", "a.c", ["-std=gnu89"])], []),
}


def cc1_args(cwd, argv):
    r = subprocess.run(argv + ["-###"], cwd=cwd, env=ENV, capture_output=True, text=True)
    for line in r.stderr.splitlines():
        parts = shlex.split(line)
        if len(parts) > 1 and parts[1] == "-cc1":
            return parts[2:]
    raise RuntimeError(r.stderr)


class Server:
    def __init__(self):
        self.p = subprocess.Popen(
            [CLANG, "-cc1", "-load", SO, "-plugin", "bothner-server",
             "-plugin-arg-bothner-server", "requests=-", "-plugin-arg-bothner-server", "results=-",
             "-plugin-arg-bothner-server", "reuse=1"]
            + (["-plugin-arg-bothner-server", "option=3"] if OPTION == "3" else [])
            + (["-plugin-arg-bothner-server", "groups=1"] if GROUPS else []) + ["-x", "c", "/dev/null"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=ENV, text=True)

    def serve(self, req):
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("the server died (exit %s)" % self.p.wait())
        return json.loads(line)

    def close(self):
        self.p.stdin.close()
        return self.p.wait()


def run_case(work, name):
    files, steps, expect = CASES[name]
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for f, text in files.items():
        os.makedirs(os.path.dirname(os.path.join(d, f)), exist_ok=True)
        open(os.path.join(d, f), "w").write(text)
    srv = Server()
    failures, known, reused_round2, build, n = [], [], 0, 0, 0
    reused_by = set()
    for rnd in range(2):
        for step in steps:
            if step[0] == "edit":
                path = os.path.join(d, step[1])
                old = os.stat(path)
                open(path, "w").write(step[2])
                # A new modification time even within the same second.
                os.utime(path, ns=(old.st_atime_ns, max(old.st_mtime_ns, time.time_ns()) + 2_000_000_000))
                build += 1
                continue
            _, cfile, flags = step
            n += 1
            out = os.path.join(d, f"{n:03d}-{cfile.replace('/', '_')}")
            deps = "-MD" in flags
            argv = [CLANG, "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-llvm-passes"] + FLAGS + flags + [cfile, "-o", out + ".ref.ll"]
            if deps:
                argv += ["-MF", out + ".ref.d"]
            r = subprocess.run(argv + ["-fplugin=" + SO], cwd=d, env=dict(ENV, BOTHNER_DUMP_OUT=out + ".ref.decls"),
                               capture_output=True, text=True)
            if r.returncode:
                failures.append(f"{cfile}: plain clang failed: {r.stderr[-400:]}")
                continue
            plain_diags = diagtext.cc1_text(r.stderr)
            args = cc1_args(d, argv)
            args[args.index("-o") + 1] = out + ".srv.ll"
            if deps:
                args[args.index("-dependency-file") + 1] = out + ".srv.d"
            res = srv.serve({"cwd": d, "args": args, "dump": out + ".srv.decls", "build": f"b{build}"})
            refused = ("refused", cfile, rnd + 1) in expect
            if refused and res.get("status") == 2:
                continue
            if refused:
                failures.append(f"{cfile} (round {rnd + 1}): not refused, status {res.get('status')} {res.get('diags', '')[-300:]}")
                continue
            if res.get("status"):
                failures.append(f"{cfile} (round {rnd + 1}): server status {res.get('status')} {res.get('error', '')} {res.get('diags', '')[-400:]}")
                continue
            how = diagtext.compare(plain_diags, diagtext.server_text(res))
            if how != "same":
                open(out + ".ref.diags", "w").write(plain_diags)
                open(out + ".srv.diags", "w").write(diagtext.server_text(res))
                if how == "differ":
                    failures.append(f"{cfile} (round {rnd + 1}): diagnostics differ: diff {out}.ref.diags {out}.srv.diags")
                else:
                    known.append(f"{cfile} (round {rnd + 1}, {how})")
            for ext in (".ll", ".decls") + ((".d",) if deps else ()):
                a, b = open(out + ".ref" + ext).read(), open(out + ".srv" + ext).read()
                if a != b:
                    failures.append(f"{cfile} (round {rnd + 1}): {ext} differs: diff {out}.ref{ext} {out}.srv{ext}")
            if rnd == 1:
                reused_round2 += res.get("reused", 0)
                reused_by |= {(cfile, h) for h in res.get("reused_files", {})}
            for e in expect:
                if isinstance(e, tuple) and e[0] == "never" and e[1] == cfile and e[2] in res.get("reused_files", {}):
                    failures.append(f"{cfile} (round {rnd + 1}): a fragment of {e[2]} was reused")
    code = srv.close()
    if code:
        failures.append(f"server exit {code}")
    if "reuse" in expect and not reused_round2:
        failures.append("no fragment was reused in the second round")
    for e in expect:
        if isinstance(e, tuple) and e[0] == "reused" and (e[1], e[2]) not in reused_by:
            failures.append(f"no compile of {e[1]} in the second round reused a fragment of {e[2]}")
    return failures, known, reused_round2


def main():
    work = os.path.abspath(sys.argv[1])
    names = sys.argv[2:] or list(CASES)
    bad = 0
    for name in names:
        failures, known, reused = run_case(work, name)
        status = "ok" if not failures else "FAIL"
        print(f"{name:20} {status:4}  reused in round 2: {reused}")
        for f in failures:
            print("    " + f)
        if known:
            print("    known differences only: " + ", ".join(known))
        bad += bool(failures)
    print(f"{len(names) - bad} of {len(names)} cases pass")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
