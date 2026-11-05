# verify-trace2: adversarial review of BothnerTrace.cpp record mode (S0 + fixes)

Core 2. Working dir: this directory. Plugin copy: BT2.cpp / BT2.so (if built).

## Done / next
- [ ] read design 2.4-2.9, 3, 8
- [ ] read plugin record-mode code
- [ ] Q1 strict nesting
- [ ] Q2 pending pragma
- [ ] Q3 bindings / refs
- [ ] Q4 SV_UNDEFINED
- [ ] Q5 builtins, #line, invocations, Key/Sep
- [ ] Q6 pragma state
- [ ] Q7 fullConfiguration

## Findings

### Data facts (verified by reading out-record-s0, all 187 compiles)
- pragma_tokens = 0 and line_directives = 0 in every redis record: no parser pragma and no #line/line
  marker anywhere in redis's compiles. So pragma-window / Prag / #line issues cannot move redis rows.
- SV bits in redis: only SV_UNDEFINED (83 instances): keymeta.h@3770 (75), strbuf.h@1590 (2),
  redismodule.h@98820 (3, two paths), hiredis dict.c@2124 (1), hiredis dict.h@4081 (1),
  vector-sets fastjson.c@1406 (1). No SV_BUILTIN/ARGS/PENDING/WEAK/ATTRIBUTE/UNDEFINED_END.

### F1 (candidate, checking) SV_UNDEFINED flags a fragment that ALSO defines the function
- declare() sets SV_UNDEFINED for every bodiless internal/inline FunctionDecl, without checking whether
  the same fragment (or unit) defines it. Design 2.8 / the plugin's own comment: "declare ... without
  defining it". keymeta.h@3770 is ONE option-2 fragment (lines 80-181, no directive inside) holding
  `static inline uint32_t getNumMeta(uint16_t);` (l.146) AND its definition (l.173). So it is flagged,
  though restoring it always restores the definition too (no -Wundefined-internal can be lost).

### F2 (REPRODUCED) Fragment right after a parser pragma line: Prag recorded as 0, pragma check vacuous
- Mechanism (BothnerTrace.cpp:245-257, 1083-1095): Prag/PF flags are taken only at the first token
  after a directive event (SinceDir). After a `#pragma` whose handler pushes an annotation token, that
  first token IS the annotation (on the pragma line, owned by no fragment, counted as a loose token),
  so the real fragment's first token gets Prag = 0 and no SV_WEAK/SV_ATTRIBUTE. Its SBefore/Server come
  from the D_pragma event, which fires before the handler runs, so it is not PENDING either.
- Repro: work/ via r_pragma.py, r_pragma2.py (harness.py). Results (pass 2, every variant):
  * pragma_push: pk.h `#pragma pack(push)\nstruct S{char c;int i;};` ; a.c `#pragma pack(1)` first
    (sizeof 5), b.c not (sizeof 8, both _Static_assert-checked by plain clang). b.c REUSES a.c's
    version; no "unsafe" (oracle uses the same prag=0), unsafe2=0.
  * pragma_vis2 (-fvisibility=hidden): vh.h `push(hidden) int vis_x; pop int vis_y;`; a.c pushes
    default first. Plain clang IR: a.c `@vis_y = dso_local global`, b.c `@vis_y = hidden global`.
    Model reuses vis_y's fragment across them (c1 reused 2), no unsafe.
  * pragma_macro: pk2.h `#pragma pack(push, PK)` with PK 1 (a.c) / 2 (b.c): sizeof 5 vs 6; reused
    in both compiles. Here the PRE-pragma state is equal, and the pragma line's macro expansions belong
    to no fragment's macro list, so the DESIGN's server (hook at the end of the #pragma directive,
    pending mark only once the annotation is handed out, 2.4) would reuse it too: a design gap.
- Design note: at the hook after a parser pragma's own directive, the annotation is queued but not
  handed out, so 2.4's pending mark is not yet set and Sema's state is the pre-pragma state. Comparing
  the pre-pragma state is sound only if the pragma line's macros are checked too.
- Fix: record: take Prag/flags at the first token handed out that is not a pragma annotation or on its
  pragma line (or mark the fragment after a pragma line that queued an annotation SV_PENDING); server:
  decline at a hook while a pragma annotation is queued (mark when the handler pushes it), or add the
  pragma line's macro uses to the next fragment's macro list.
- Redis effect: NONE (pragma_tokens = 0 in all 187 records).

### Data facts 2 (verified by running)
- embedded.py: all 187 redis compiles, plain clang, `-Wno-everything -Wembedded-directive`: ZERO
  warnings (18.5 s). No directive is ever processed inside macro arguments in redis, so no VIEW_ARGS
  question can move redis rows.
- Scan of the 557 distinct entry files (entry_files.txt): no null directive, no #line, no line marker,
  no #ident/#sccs/#assert/#unassert/#embed; only #error (235) and #warning (10) lines, all in skipped
  regions (redis compiles print nothing). So directives without a PPCallbacks event cannot move redis.

### F3 (REPRODUCED) K&R: a directive between two K&R parameter declarations reads VIEW_STRICT
- The fix of round 1 only un-strictens a directive whose NEXT token is '{'. In
  `int kr(a,b,c)\nint a;\n#if 1\nint b;\n#endif\nint c;\n{...}` the #if and #endif events both have
  prev ';' with CurContext = TU (ParseKNRParamDeclarations runs before ActOnStartOfFunctionDef; the
  observer's InDeclGroup was restored before ParseFunctionDefinition), next token `int` -> STRICT.
  r_kr.py: kr.h fragments #0,#1,#2 all strict=(1,1) and reused in both compiles (S2 reused 4); the
  design's fix (flag raised through K&R params) would refuse all three. A server restoring #1 would
  silently drop `int b;` (gnu89: b defaults to int, exit 0); restoring #0/#2 puts a restore token
  inside a function definition.
- Fix: make STRICT false for every directive between a K&R declarator's ')' and its body '{' (e.g.
  scan forward: a directive is not strict if the next file-scope '{' is reached before any ';'-at-TU
  that is followed by a non-declaration-specifier... simplest: record Sema/Parser state - in K&R param
  parsing, Actions.getCurFunctionDecl() is null but the parser is inside ParseFunctionDefinition; or
  treat ';' as ending a declaration only if the next token is not a decl-specifier followed by ... ).
  Practical: mark every directive event between a K&R identifier list's ')' and the body '{' (both
  visible in the token stream: ')' then a decl-specifier at TU) as not strict.
- Redis effect: NONE: `-Wdeprecated-non-prototype` on all 187 compiles: zero warnings (no K&R def).

### F4 (REPRODUCED) VIEW_ARGS misses invocations whose macro name comes from another expansion
- CB::MacroExpands records [begin, ')'] only if Range.getBegin().isFileID(). For `#define CALL FOO`
  `CALL(\n#if 1\nb\n#endif\n)`, FOO's begin is a macro location: not recorded, so the #if/#endif
  events inside FOO's arguments get no VIEW_ARGS. r_args.py: args.h #0 `int a0; CALL(` (ends inside
  the arguments) and #2 `) int c;` (starts inside them) have sv=- and are reused in every compile;
  the same text with FOO written directly (args2.h) is flagged ARGS both ways. Same for
  `#define OPEN FOO(` + `OPEN\n#if..`.
- Fix: record [SM.getExpansionLoc(Range.getBegin()), Range.getEnd()] whenever Range.getEnd() is a
  file location (or query the preprocessor's InMacroArgs at each directive event).
- Redis effect: NONE (no directive inside macro arguments in redis at all, see Data facts 2).
