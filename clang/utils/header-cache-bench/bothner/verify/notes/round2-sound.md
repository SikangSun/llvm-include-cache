# verify-sound2 NOTES (second soundness round; core 5)

## done / next
- [ ] 1. re-run A (args), C (cleanup), D (K&R) on fixed plugin + simulator
- [ ] 2a. S3G* attacks (groups with #define lines)
- [ ] 2b. occurrence-key attacks
- [ ] 3. gap B: unsafe2 names classification; real dropped deps in redis headers
- [ ] 5. fallback rule vs Used bit (Sema/CodeGen)
- [ ] 4. real-server implementability of S3G* and occ

## findings
### F1 (task 1): A, C, D are refused now (VERIFIED by running)
- Reproduction: verify-sound2/t1_acd.py (harness.py = old harness on the CURRENT simulate.py; simulate_detail.py = current
  simulate.py + per-unit log, made by mkdetail.py; asserts decisions equal the original's).
- A (args.h: `int a0; DECL(` / `#if 1 int a; #endif` / `) int b;`): both fragments carry SV_ARGS (server=2): `args.h:18`
  ends inside the args, `args.h:52` starts inside them; every variant refuses both as "macro args"; unsafe none.
- C (use.h's cleanup(done)): use.h now refs ['done'], uses [0], deps [0]; c2's use.h is refused "uses" (fn.h version
  differs) in every variant; never reused across the two done() definitions.
- D (K&R `int kr(a) int a;` / `#if 1` / `{...}`): the K&R head fragment now has safter=0 and the body fragment sbefore=0;
  option 2/3 refuse both "nested"; groups join them [0,25] and refuse "macro" (K differs). unsafe none.

### F2 (task 2, S3/S3G/S3G*): option-3 spans drop their EMPTY option-2 members' SV_* flags and macro lists (VERIFIED)
- simulate.py span loader: `ms = [m for m in allms if not m["empty"]]`; the span's flags, server (SV_*), ids (macro
  list) and tok are taken from `ms` only. An option-2 member that hands the parser no tokens but expands a macro
  whose effect is preprocessor-only (`_Pragma("push_macro")`, pop_macro, GCC poison, clang assume_nonnull,
  diagnostics, system_header, once) has SV_BUILTIN in its own record, but the span holding it does not.
- Case P1 (t2_pragma_span.py, cases/P1_pushmacro): pm.h span 128..156 = [PUSH_X (empty, server=1), #undef X,
  #define X 2]; span server=0; S3, S3G, S3G*, S3G* fb, S3G* fb occ all REUSE it (c1->c2 and pass 1), unsafe none.
- Consequence shown with plain clang: the reuse = skip the span text + replay its #undef/#define, i.e. pm.h without
  the PUSH_X line: then POP_X warns "could not pop 'X'", X stays 2, and the main file's `int c2[X]` has 2 elements
  (plain clang: 1) -> _Static_assert fails (c2_server.c). The model cannot show it: it compares macro and pragma
  states recorded from plain parses, never the server's own state after a replay.
- Fix: OR every member's server/flags into the span (empty ones too) and take ids from all members.
- Redis impact of F2 (scan_empty.py over out-record-s0): 499 empty members inside non-empty spans, NONE with
  SV_BUILTIN; their dropped macro ids are only __BEGIN_DECLS (421) / __END_DECLS (78). So no effect on the redis rows.

### Checked, NOT a gap: block-scope externs (t4_blockextern.py, cases/N1_blockextern)
- Suspected negative-lookup hole: `static inline int fail(int x){ extern void die(void); ... }` stored where no
  file-scope die() exists, reused where g.h declared `die` with an asm label. Not a hole: Clang makes a local extern
  semantically a TU member (adjustContextForLocalExternDecl), so the plugin's fileScope() sees it; refs has 'die' with
  an empty stored list, and c1 refuses "deps" in every variant.
- Residual (inferred, not built): a TAG first named inside a function body (`struct T *p;` at block scope) is not
  file scope, so no ref is kept when no file-scope tag exists; a later compile with a file-scope `struct T` would
  reuse it. Silent differences need contrived code (incomplete type uses only), so not pursued.

### Occurrence keys: small builds (t5_occ.py) behave (VERIFIED)
- o1 (no guard, read under MODE 1 then 2 in c0, once under MODE 2 in c1): occ reuses c1's m.h:60#1 / 109#1 from c0's
  first reading only where the checks match; no unsafe. o2 (identical double reading: typedef redefinition, tag
  redeclaration, extern redeclaration; c2 reads once): reuses by (loc, occ), predecessor lists match; no unsafe.
- No occ-specific blind spot found: occ only changes the key; every check still applies per reading (see task 4).

### F3 (task 3, gap B): __builtin_convertvector's type operand is a REAL dependency the visitor drops (VERIFIED)
- RecursiveASTVisitor.h:2998 `DEF_TRAVERSE_STMT(ConvertVectorExpr, {})`: the destination TypeSourceInfo is kept in the
  AST but never traversed, so UseVisitor records no ref to the typedef named there (unlike casts, sizeof, va_arg,
  offsetof, _Generic, typeof, which are traversed).
- Case B1 (t6_convertvector.py, cases/B1_convertvector): t.h gives `vt` = 4 x float (c0) or 4 x double (c1, WIDE);
  cv.h `static inline int cv_size(vi x){ return sizeof(__builtin_convertvector(x, vt)); }`. cv.h refs = ['vi'] only.
  Every non-tok variant (S2..S3G*, fb, occ) REUSES c0's cv.h in c1; oracle silent (unsafe none); unsafe2 flags it;
  the tok runs refuse "deps". Plain clang -O1: s0 returns 16, s1 returns 32 -> the reused body is wrong in c1.
- Redis: f16cintrin.h (`__builtin_convertvector((__v4fp16)__v, __v4sf)`, `__v8sf`), 5 instances each, are the only
  real ones among the 81 tok-only (file, name) pairs; in practice covered, since the same f16cintrin fragments ref
  __m128/__m256 etc. declared in the same xmmintrin.h/avxintrin.h fragments as __v4sf/__v8sf.
- Fix: VisitConvertVectorExpr -> TraverseTypeLoc(E->getTypeSourceInfo()->getTypeLoc()); also DeclArgument attrs
  (diagnose_as_builtin's function; none in redis headers).

### Gap B classification on redis (gapb_scan.py -> gapb_pairs.json, gapb_scan.txt) (VERIFIED on header text)
- 81 (file, name) pairs / 4715 option-2 instances where an identifier token's name is neither ref nor bind but an
  earlier unit (or the main file) declares it. Classified from the fragment text:
  fields (read/write/close/writev/free/dup/unlink/rename/sync/call/select/repl_state/failover_state/time/ctime/
  timezone/significand/rax/brk/log/link), parameters (index/link/user/signal/stat/dict/stream), locals (exp in
  expr.c, index in dict.c), attribute names/archetypes (format(printf,..) incl. via REDISMODULE_ATTR macros,
  __attribute__((malloc,...))), tag-vs-ordinary namespace; all coincidental (declarator names after a type
  specifier; no lookup decides anything). Only __v4sf/__v8sf (F3) are real.
- Constructs whose expression the AST drops, in the 557 headers redis enters: vector_size 14 files, ALL literal
  arguments; ext_vector_type, _BitInt, address_space, matrix_type: none; diagnose_as_builtin, type_tag: none;
  cleanup only in pthread.h macros (expanded in .c files). aligned(...) with names: kept in AlignedAttr (traversed).
