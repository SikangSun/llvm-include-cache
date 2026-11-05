"""Bothner's compile server replayed over a recorded build
(COMPILE_SERVER_PLAN.md 3.3, step D5).

The build's compiles run in log order twice: a first build fills the server,
then the recompilation is scored. Inside a compile, fragment instances run in
the order the compile processed them, with its #define and #undef between
them. The server holds, per fragment location (configuration, file as opened,
offset), one stored version: the latest parse (p.26). A version keeps its
macro list, its uses (each used location with the version this compile had
there), and, for the scoring only, its oracle (see oracles()).

A fragment instance that hands the parser no tokens (__BEGIN_DECLS, empty in
C; no nesting recorded, no time) is "empty": neither reused nor parsed. Any
other is reused when the variant's rule allows it. In order of the reasons
given when it does not:
  nested   the parser was inside a declaration where it starts or ends (p.33);
  param    it declares a tag in a parameter list (p.28-29);
  main     it uses a main-file declaration, which is parsed in every compile
           (ours, from p.27's precondition);
  repeat   this compile processed the location already (p.28);
  new      nothing is stored there;
  macro    the macro check fails;
  uses     a location it used was not processed in this compile at the
           version it saw: p.28's timestamp rule, read through version
           identity, which one version per location makes equivalent;
  unsafe   the rule allows it, but the stored version's oracle differs: its
           parse would mean something else. Counted, and charged as a parse,
           as a correct server must.
Whatever is not reused is parsed and becomes the stored version.

Rules for a parse and for repeats:
  R0  the paper's: every parse is a new version, and a repeat is parsed;
  R1  ours: a parse identical to the stored version (same oracle) keeps its
      version number, so its users stay valid, while its macro list and uses
      become this parse's (p.28 rebuilds uses_fragments at every parse); and
      a version's own location is left out of its uses (otherwise a header
      read twice depends on itself and is never valid again);
  R2  as R1, but locations are numbered by occurrence in the compile, so the
      second reading of a header is a location of its own: the variant most
      favourable to Bothner.
Macro checks:
  V0  none, as the prototype ("does not yet check", p.29);
  V1  the macro list: every identifier the fragment sent to the
      macro-expansion decision must be in the same state (no macro, or the
      same definition): p.30's "obvious solution", with expands-to (ours);
  V2  p.30's two bits per identifier, kept per configuration: used as a
      non-macro, then also defined as a macro, which invalidates every stored
      version; afterwards a fragment checks the state of such identifiers.
Option 3 (p.26, "O3" variants): #define and #undef belong to fragments, and
only other directives end one. An option-3 instance is a span of option-2
fragments and #define/#undef lines (BothnerTrace's spans): its tokens, uses
and dependencies are its members', less those inside it; its macro list
leaves out the names it defines itself, which a reuse replays; it is nested
if the parser is inside a declaration at its start or end, not between its
members (so an enum with #define lines inside is one fragment, the paper's
remedy for siginfo, p.32); and reusing it also saves its #define lines' time.
Bounds, beside the variants:
  tokens  D2's v0: R1, the tokens must match, no uses and no oracle check (a
          token mismatch is reported as "macro");
  ideal   any earlier parse at the location (numbered as R2) with the same
          oracle: the best a cache of fragments could do with free restore.

Time: plain clang's front end for each compile (as a user runs it, without
the modules flags), less the measured time of its reused fragments, at no
cost for restoring them, less a credit for what a persistent server skips
besides parsing. The headline credit is CREDIT_MS of compiler start-up (p.24;
his driver still ran for every request, p.22-23; measured 1.57-1.68 ms with
redis's flags); the markdown table also gives none, start-up plus a cache of
header text (at most the calibrated cost of each #include and file entry),
and the whole process start. Shares are given by time, and by instance.

The compile server's rule (COMPILE_SERVER_DESIGN.md 2.6, S0), where the
records have its fields: R0 with every check a correct server needs, keyed by
the server's configuration key, over PHASES passes of the build (the first
fills the server; the second is the recompilation the server will be measured
on; the rest show whether its decisions settle). Units: option 2 ("S2"),
option 3 ("S3"), and fragment groups (2.7, p.33) over either ("S2G", "S3G"):
a maximal run of a header entry's units joined where the parser is inside a
declaration at the boundary between them, with only conditionals between and
no #define or #undef inside; a group is reused only when the same members
(its conditionals' outcome) are processed. "S3G*": option-3 groups that may
hold #define lines, replayed in order as the group's conditionals are checked.
Reasons, in the order checked:
  nested       the parser is inside a declaration at its start or end, under
               the fixed observer (2.9);
  param        as above;
  main         it uses a main-file declaration, or an earlier declaration of
               what it references is one: a main file is parsed in every
               compile, so its declarations are new objects each time;
  builtin      a never-reusable builtin or _Pragma (2.4);
  undefined    it declares an internal or inline function without defining
               it (2.8: Used bits leak between compiles);
  macro args   it starts or ends inside a function-like macro's arguments;
  pending      it starts while a parser pragma is pending;
  repeat, new  as above;
  conditionals a group's members differ from the stored group's;
  uses         as above (p.28);
  macro        V1's macro list;
  pragma       the full pragma state differs, or a #pragma weak or
               redefine_extname is pending, or a #pragma clang attribute is
               active;
  negative     a name it declares had no earlier declaration when stored, and
               has one now;
  deps         the earlier declarations of an entity it references are not
               the same ones at the same versions (Clang's redeclaration chains);
  predecessor  the earlier declarations of a name it declares are not the
               stored ones (its restored declaration would hide a newer one);
  unsafe       every check passes but the oracle differs: a check is missing.
               Charged as a parse.

usage: simulate.py <record dir> <timing dir> [recompile json]
                   [--trace INDEX] [--variant NAME] [--out per-compile.json]
                   [--reused reused.json] [--markdown table.md]
                   [--server-out server.json] [--server-markdown table.md]
The recompile JSON (recompile.py) gives plain clang's and the cache's
recompilation times; without it, the timing run's plain clang with modules.
env: CREDIT_MS (default 1.65); SAVED_SCALE (default 1): multiplies the time
every reused fragment saves, to widen the bound by the time model's error
(blank.py measured it); PHASES (default 5): passes of the server's rule.
"""
import argparse, collections, json, os, statistics, sys

ap = argparse.ArgumentParser()
ap.add_argument("records")
ap.add_argument("timing")
ap.add_argument("recompile", nargs="?")
ap.add_argument("--trace", type=int, help="print every decision of this compile (log index)")
ap.add_argument("--variant", default="R1 V1", help="the variant --trace follows")
ap.add_argument("--out", help="write per-compile results here")
ap.add_argument("--markdown", help="write the paper's table here")
ap.add_argument("--reused", help="write, per compile, the instances --variant reuses in the recompilation "
                "(file as opened, offset, occurrence): blank.py's input")
ap.add_argument("--server-out", help="write the server rule's per-compile decisions here")
ap.add_argument("--server-markdown", help="write the server rule's table here")
args = ap.parse_args()
recomp = json.load(open(args.recompile)) if args.recompile else None
CREDIT = float(os.environ.get("CREDIT_MS", "1.65")) / 1e3
SCALE = float(os.environ.get("SAVED_SCALE", "1"))
PHASES = int(os.environ.get("PHASES", "5"))
SV_BUILTIN, SV_ARGS, SV_PENDING, SV_WEAK, SV_ATTRIBUTE, SV_UNDEFINED, SV_UNDEFINED_END = 1, 2, 4, 8, 16, 32, 64
SEP_COND, SEP_DEFINE, SEP_FIRST = 1, 2, 32                              # and SEP_*

VARIANTS = {  # name: (rule, macro check, option 3)
    "R1 V1": ("R1", "V1", False), "R0 V1": ("R0", "V1", False), "R2 V1": ("R2", "V1", False),
    "R1 V0": ("R1", "V0", False), "R1 V2": ("R1", "V2", False),
    "tokens": ("R1", "tokens", False), "ideal": ("R2", "ideal", False),
    "O3 R1 V1": ("R1", "V1", True), "O3 R2 V1": ("R2", "V1", True), "O3 ideal": ("R2", "ideal", True),
}

# --- The recorded build ---------------------------------------------------
intern = {}
def name_of(s):
    return intern.setdefault(s, s)

def groups_of(units, defines=False):
    """Fragment groups (2.7, p.33) over one compile's units, given in
    processing order: maximal runs of a header entry's units, in text order,
    joined where the parser is inside a declaration at the boundary (the end of
    the run's last non-empty unit, under the fixed observer), with only
    conditionals between them and, following p.33, no #define or #undef
    inside. An empty unit joins a run without changing its boundary. With
    defines, option-3 units with #define lines join too: a reuse replays them
    in order while it checks the conditionals between them, and the group's
    macro list leaves out names an earlier member defined. The groups come
    back in processing order, with uses and deps between groups and
    occurrences."""
    by_entry = collections.defaultdict(list)
    for p, u in enumerate(units):
        by_entry[u["entry"]].append(p)
    runs = []
    for ps in by_entry.values():
        ps.sort(key=lambda p: units[p]["rec"])
        run, last = [], None  # last: the run's last non-empty unit
        for p in ps:
            u = units[p]
            if not (last is not None and last["safter"] == 0 and not u["sep"] & ~SEP_COND
                    and (defines or not u["defines"] and not any(units[q]["defines"] for q in run))):
                if run:
                    runs.append(run)
                run, last = [], None
            run.append(p)
            if not u["empty"]:
                last = u
        if run:
            runs.append(run)
    groups = []
    for run in runs:
        ms = [units[p] for p in run]
        if len(ms) == 1:
            g = dict(ms[0])
        else:
            ne = [m for m in ms if not m["empty"]]
            first, last = ne[0], ne[-1]
            ids, defined = {}, set()
            for m in ms:
                for n, v in m["ids"]:
                    if n not in defined:
                        ids.setdefault(n, v)
                defined |= m["defnames"]
            ids = tuple(sorted(ids.items()))
            flags = sv = 0
            for m in ms:
                flags |= m["flags"]
                sv |= m["server"]
            g = {"loc": ms[0]["loc"], "tok": hash(tuple(m["tok"] for m in ms)), "pragma": first["pragma"],
                 "prag": first["prag"], "empty": False,
                 "nested": not (first["before"] == 1 and last["after"] == 1),
                 "sbefore": first["sbefore"], "safter": last["safter"],
                 "snested": not (first["sbefore"] == 1 and last["safter"] == 1),
                 "order": min(m["order"] for m in ne), "flags": flags, "server": sv, "ids": ids,
                 "mlist": hash(ids), "ns": sum(m["ns"] for m in ms), "defns": sum(m.get("defns", 0) for m in ms),
                 "binds": [bd for m in ms for bd in m["binds"]], "refs": frozenset().union(*(m["refs"] for m in ms)),
                 "toknames": frozenset().union(*(m["toknames"] for m in ms)), "entry": ms[0]["entry"], "defines": sum(m["defines"] for m in ms),
                 "defnames": frozenset().union(*(m["defnames"] for m in ms)), "shape": tuple(m["loc"] for m in ms)}
        g["units"] = run
        groups.append(g)
    groups.sort(key=lambda g: g["order"])
    pos = {p: gi for gi, g in enumerate(groups) for p in g["units"]}
    seen = collections.Counter()
    for gi, g in enumerate(groups):
        for key in ("uses", "deps"):
            g[key] = sorted({pos[u] for p in g["units"] for u in units[p][key]} - {gi})
        seen[g["loc"]] += 1
        g["occ"] = seen[g["loc"]]
    return groups


builds = []
for f in sorted(os.listdir(args.records)):
    if not f.endswith(".json"):
        continue
    r = json.load(open(os.path.join(args.records, f)))
    t = json.load(open(os.path.join(args.timing, f)))
    if not r["data"] or not t["data"]:
        continue
    rd, td = r["data"], t["data"]
    assert len(rd["frags"]) == len(td["frags"]), f
    names = [name_of(n) for n in rd["names"]]
    server = all(len(a) > 24 for a in rd["frags"]) and "main_decls" in rd
    xs = []
    for rec, (a, b) in enumerate(zip(rd["frags"], td["frags"])):
        name = name_of(rd["entries"][a[0]][0])
        assert (name, a[1]) == (td["entries"][b[0]][0], b[1]), f
        # By name: the record lists them by a name id each compile assigns.
        ids = tuple(sorted((names[n], s) for n, s in a[15]))
        # A fragment that hands the parser no tokens (__BEGIN_DECLS, empty in
        # C) has no nesting recorded (-1) and no time: "empty", not nested.
        x = {"loc": (rd["config"], name, a[1]), "tok": a[7], "pragma": a[11], "empty": a[8] == -1,
             "nested": not (a[8] == 1 and a[9] == 1), "before": a[8], "after": a[9], "order": a[10],
             "flags": a[13], "uses": a[14], "deps": a[16], "ids": ids, "mlist": hash(ids), "ns": b[5],
             "entry": a[0], "rec": rec}
        if server:
            # The server's fields: nesting under the fixed observer, the full
            # pragma state, SV_* flags, what it declares and references at
            # file scope, its separator from the previous fragment.
            x.update(sbefore=a[17], safter=a[18], snested=not (a[17] == 1 and a[18] == 1), prag=a[19],
                     server=a[20], binds=[(ns, names[n]) for ns, n in a[21]],
                     refs=frozenset((ns, names[n]) for ns, n in a[22]), sep=a[24], defines=0, defnames=frozenset(),
                     toknames=frozenset(names[n] for n in a[25]) if len(a) > 25 else frozenset())
            x["shape"] = (x["loc"],)
        xs.append(x)
    byorig = list(xs)
    order = sorted(range(len(xs)), key=lambda k: xs[k]["order"])
    pos = {k: p for p, k in enumerate(order)}
    xs = [xs[k] for k in order]
    seen = collections.Counter()
    for x in xs:
        x["uses"] = [pos[u] for u in x["uses"]]
        x["deps"] = [pos[u] for u in x["deps"]]
        seen[x["loc"]] += 1
        x["occ"] = seen[x["loc"]]
    # Fragments and #define/#undef in processing order.
    seq = [(x["order"], 1, x) for x in xs] + [(e, 0, (names[n], d)) for e, n, d in rd["defs"]]
    seq.sort(key=lambda s: (s[0], s[1]))

    # Option 3: the spans, from their option-2 members.
    assert len(rd["spans"]) == len(td["spans"]), f
    xs3, span_of = [], {}
    for rec, (sr, st) in enumerate(zip(rd["spans"], td["spans"])):
        name = name_of(rd["entries"][sr[0]][0])
        assert (name, sr[1]) == (td["entries"][st[0]][0], st[1]), f
        allms = [byorig[m] for m in sr[3]]
        ms = sorted((m for m in allms if not m["empty"]), key=lambda x: x["order"])
        defs = sorted((e, names[n]) for e, n in sr[10])  # its #defines and #undefs, in order
        defs_first = sr[6] >= 0 and (not ms or sr[6] < ms[0]["order"])
        start = sr[7] if defs_first else (ms[0]["before"] if ms else -1)
        end = ms[-1]["after"] if ms else sr[8]
        # Its macro list: a name its own #define or #undef set before the use
        # is replayed with it, so left out; a name used before that is the
        # outside's, as p.30's lists would keep it. An empty member's names
        # (its macros expanded to nothing) are all kept: where it falls among
        # the #define lines is not recorded, and a reuse skips its expansions.
        first = {}
        for m in ms:
            set_before = {n for e, n in defs if e < m["order"]}
            for n, v in m["ids"]:
                if n not in set_before:
                    first.setdefault(n, v)
        for m in allms:
            if m["empty"]:
                for n, v in m["ids"]:
                    first.setdefault(n, v)
        ids = tuple(sorted(first.items()))
        flags = 0
        for m in ms:
            flags |= m["flags"]
        y = {"loc": (rd["config"], name, sr[1]), "tok": hash((tuple(m["tok"] for m in ms), sr[4])),
             "pragma": sr[9] if defs_first else (ms[0]["pragma"] if ms else ""),
             "empty": not ms and sr[6] < 0, "nested": not (start == 1 and end == 1),
             "order": min(([sr[6]] if sr[6] >= 0 else []) + [m["order"] for m in ms] or [allms[0]["order"]]),
             "flags": flags, "ids": ids, "mlist": hash(ids), "ns": sum(m["ns"] for m in ms) + st[5],
             "defns": st[5], "members": allms, "entry": sr[0], "rec": rec, "before": start, "after": end}
        if server:
            # Its start is its first #define's, or its first member's; its
            # end, its last member's, or its last #define's. SV_* flags of
            # any member count (conservatively), with those at its start.
            sstart = sr[11] if defs_first else (ms[0]["sbefore"] if ms else -1)
            send = ms[-1]["safter"] if ms else sr[12]
            sv = sr[14] if defs_first else (ms[0]["server"] if ms else 0)
            for m in allms:  # empty members too: a reuse skips their _Pragma or builtins
                sv |= m["server"]
            y.update(sbefore=sstart, safter=send, snested=not (sstart == 1 and send == 1),
                     prag=sr[13] if defs_first else (ms[0]["prag"] if ms else ""), server=sv,
                     binds=[bd for m in ms for bd in m["binds"]], refs=frozenset().union(*(m["refs"] for m in ms)),
                     toknames=frozenset().union(*(m["toknames"] for m in ms)),
                     sep=sr[16], defines=sr[4], defnames=frozenset(n for _, n in defs), shape=(y["loc"],))
        for m in ms:
            span_of[id(m)] = y
        xs3.append(y)
    xs3.sort(key=lambda y: y["order"])
    pos3 = {id(y): p for p, y in enumerate(xs3)}
    seen = collections.Counter()
    for y in xs3:
        for key in ("uses", "deps"):
            out = set()
            for m in y["members"]:
                for u in m[key]:
                    p = pos3[id(span_of[id(xs[u])])]
                    if xs3[p] is not y:
                        out.add(p)
            y[key] = sorted(out)
        seen[y["loc"]] += 1
        y["occ"] = seen[y["loc"]]
    # What a server keeping header files in memory (p.24) would also skip, at
    # most: the calibrated cost of each #include and file entry.
    files = sum(td["dir_events"][k] * td["calib_ns"][k] for k in ("include", "enter")) / 1e9
    build = {"i": r["meta"]["i"], "src": r["meta"]["src"], "cwd": r["meta"]["cwd"],
             "config": rd["config"], "xs": xs, "seq": seq, "span": td["span_ns"] / 1e9,
             "pre": td["own_ns"]["pre"] / 1e9, "files": files,
             "xs3": xs3, "seq3": [(y["order"], 1, y) for y in xs3],
             "plainM": t["meta"].get("plain_wall", 0), "server": server}
    if server:
        build.update(fconfig=rd["full_config"], xg2=groups_of(xs), xg3=groups_of(xs3), xg3d=groups_of(xs3, True),
                     mains=[(o, (ns, names[n])) for o, ns, n in rd["main_decls"]])
    builds.append(build)
HAS_SERVER = bool(builds) and all(b["server"] for b in builds)


def oracles(b, xs):
    """Each instance's oracle: its tokens, its pragma state, the distinct
    oracles of the fragments holding earlier declarations of what it used, and
    the main file where it depends on that. Earlier readings of its own
    location are left out, whatever numbering a rule uses: a header read twice
    redeclares its own first reading. Each oracle counts once: Clang's
    __stddef_size_t.h, read a varying number of times, gives a varying number
    of identical earlier declarations."""
    out = []
    for x in xs:
        deps = sorted({out[d] for d in x["deps"] if xs[d]["loc"] != x["loc"]})
        out.append(hash((x["tok"], x["pragma"], tuple(deps), (b["cwd"], b["src"]) if x["flags"] & 2 else None)))
    return out


# --- The server ------------------------------------------------------------
class Version:
    __slots__ = ("id", "oracle", "mlist", "tok", "pragma", "uses", "ts", "check")


def simulate(rule, macro, opt3=False, strict=False, trace=None):
    store = {}                       # key -> Version, or for "ideal" key -> set of oracles
    clock, ids = 0, 0
    nonmacro = collections.defaultdict(set)    # V2, per configuration
    alsomacro = collections.defaultdict(set)
    first_valid = collections.defaultdict(int)
    out = []
    for phase in (0, 1):
        for b in builds:
            clock += 1
            cfg = b["config"]
            processed = {}           # key -> version id this compile has there
            xs, seq = (b["xs3"], b["seq3"]) if opt3 else (b["xs"], b["seq"])
            ver = [None] * len(xs)
            key_of = (lambda x: (x["loc"], x["occ"])) if rule == "R2" else (lambda x: x["loc"])
            orc = oracles(b, xs)
            why, why_ns, unsafe, reused = collections.Counter(), collections.Counter(), collections.Counter(), []
            saved = saved_def = 0
            p = 0
            for _, kind, item in seq:
                if kind == 0:        # #define or #undef
                    name, defined = item
                    if macro == "V2" and defined and name in nonmacro[cfg] and name not in alsomacro[cfg]:
                        alsomacro[cfg].add(name)
                        clock += 1
                        first_valid[cfg] = clock
                    continue
                x, k = item, p
                p += 1
                key = key_of(x)
                v = store.get(key)
                if x["empty"]:       # hands the parser nothing: neither reused nor parsed
                    why["empty"] += 1
                    continue
                if x["snested"] if strict else x["nested"]:
                    reason = "nested"
                elif x["flags"] & 4:
                    reason = "param"
                elif x["flags"] & 1:
                    reason = "main"
                elif key in processed:
                    reason = "repeat"
                elif v is None:
                    reason = "new"
                elif macro == "ideal":
                    reason = None if orc[k] in v else "changed"
                elif macro == "tokens":
                    reason = None if v.tok == x["tok"] else "macro"
                else:
                    reason = None
                    if macro == "V1" and v.mlist != x["mlist"]:
                        reason = "macro"
                    elif macro == "V2":
                        now = dict(x["ids"])
                        if v.ts <= first_valid[cfg] or any(now.get(n, -1) != s for n, s in v.check):
                            reason = "macro"
                    if reason is None and any(processed.get(lu) != vu for lu, vu in v.uses):
                        reason = "uses"
                    if reason is None and v.oracle != orc[k]:
                        reason = "unsafe"
                        # Why the parse would differ, and where.
                        cause = ("tokens" if v.tok != x["tok"] else "pragma" if v.pragma != x["pragma"]
                                 else "earlier declarations")
                        unsafe[(cause, x["loc"][1])] += 1
                if macro == "V2":
                    nonmacro[cfg].update(n for n, s in x["ids"] if s == 0)
                if reason is None:   # reused
                    if macro == "ideal":
                        processed[key] = orc[k]
                    else:
                        processed[key] = v.id
                        ver[k] = v.id
                    saved += x["ns"]
                    saved_def += x.get("defns", 0)
                    why["reused"] += 1
                    why_ns["reused"] += x["ns"]
                    reused.append(k)
                else:                # parsed: the stored version from now on
                    why[reason] += 1
                    why_ns[reason] += x["ns"]
                    clock += 1
                    if macro == "ideal":
                        store.setdefault(key, set()).add(orc[k])
                        processed[key] = orc[k]
                    else:
                        uses = []
                        for u in x["uses"]:
                            lu = key_of(xs[u])
                            if rule != "R0" and lu == key:
                                continue
                            uses.append((lu, ver[u]))
                        if rule != "R0" and v is not None and v.oracle == orc[k]:
                            # R1: an identical parse keeps the version number,
                            # so its users stay valid; its macro list and uses
                            # become this parse's (p.28 rebuilds uses_fragments
                            # at every parse).
                            nv = v
                        else:
                            nv = Version()
                            ids += 1
                            nv.id = ids
                            store[key] = nv
                        nv.oracle, nv.mlist, nv.tok, nv.uses, nv.ts = orc[k], x["mlist"], x["tok"], uses, clock
                        nv.pragma = x["pragma"]
                        nv.check = [(n, s) for n, s in x["ids"] if n in alsomacro[cfg]] if macro == "V2" else ()
                        processed[key] = nv.id
                        ver[k] = nv.id
                if trace is not None and phase == 1 and b["i"] == trace:
                    print(f"  {x['loc'][1]}:{x['loc'][2]}#{x['occ']}  {reason or 'reused':8} {x['ns'] / 1e3:8.1f} us"
                          f"  uses {[(xs[u]['loc'][1], xs[u]['loc'][2]) for u in x['uses']]}")
            if phase == 1:
                out.append({"i": b["i"], "why": why, "why_ns": why_ns, "saved": saved, "saved_def": saved_def,
                            "unsafe": unsafe, "reused": reused})
    return out


# --- The compile server's rule (S0) ----------------------------------------
SERVER = {"S2": "xs", "S3": "xs3", "S2G": "xg2", "S3G": "xg3", "S3G*": "xg3d"}
SERVER_REASONS = ["reused", "nested", "param", "main", "builtin", "undefined", "macro args", "pending", "repeat",
                  "new", "conditionals", "uses", "macro", "pragma", "negative", "deps", "predecessor", "unsafe",
                  "fallback"]


def oracles_full(b, xs, occurrence=False):
    """oracles(), with the full pragma state. With occurrence keys a reading
    is a key of its own, so only deps on the same reading are left out."""
    same = (lambda d, x: (xs[d]["loc"], xs[d]["occ"]) == (x["loc"], x["occ"])) if occurrence else \
        (lambda d, x: xs[d]["loc"] == x["loc"])
    out = []
    for x in xs:
        deps = sorted({out[d] for d in x["deps"] if not same(d, x)})
        out.append(hash((x["tok"], x["prag"], tuple(deps), (b["cwd"], b["src"]) if x["flags"] & 2 else None)))
    return out


class ServerVersion:
    __slots__ = ("id", "mlist", "prag", "tok", "uses", "deps", "lists", "binds", "bset", "oracle", "oracle2", "shape")


def oracles_tok(b, xs, occurrence=False):
    """oracles_full(), with the dependencies the AST may drop added: the
    earlier units of this compile declaring a name any of the unit's
    identifier tokens spells (a superset of what its parse looked up), and the
    main file where it declared one. For checking the rule only: a name a
    member, a parameter or a local shares with a file-scope declaration is a
    false dependency here."""
    same = (lambda d, x: (xs[d]["loc"], xs[d]["occ"]) == (x["loc"], x["occ"])) if occurrence else \
        (lambda d, x: xs[d]["loc"] == x["loc"])
    declared = collections.defaultdict(list)
    mains = {n for _, n in b["mains"]}
    main_before = collections.defaultdict(lambda: float("inf"))
    for o, n in b["mains"]:
        main_before[n] = min(main_before[n], o)
    out = [None] * len(xs)
    for k in sorted(range(len(xs)), key=lambda k: xs[k]["order"]):
        x = xs[k]
        names = {(ns, n) for n in x["toknames"] for ns in (0, 1)} | set(x["refs"]) | set(x["binds"])
        deps = {d for d in x["deps"] if not same(d, x)}
        deps |= {d for nm in names for d in declared.get(nm, ()) if not same(d, x)}
        main = x["flags"] & 2 or any(nm in mains and main_before[nm] < x["order"] for nm in names)
        out[k] = hash((x["tok"], x["prag"], tuple(sorted(out[d] for d in deps)), (b["cwd"], b["src"]) if main else None))
        for nm in x["binds"]:
            declared[nm].append(k)
    return out


def simulate_server(units, fixed=True, phases=PHASES, trace=None, occurrence=False, used="design", tokrefs=False):
    """The server's decisions for every compile of every pass: R0 with the
    checks of COMPILE_SERVER_DESIGN.md 2.6, in the order the docstring gives.
    A version records its uses and deps with the versions this compile had
    there, and for every name it declares or references the declarations of
    that name this compile had made before it (as (key, version), a main-file
    declaration as unique to its compile run). fixed=False reads nesting from the
    old observer; occurrence=True numbers a location's readings in a compile
    (as R2), so that a repeat is a key of its own. used="fallback" replaces
    2.8's rule: units declaring an internal or inline function without a body
    are reused, and a compile that ends with such a restored function still
    undefined runs again without reuse (the leaked Used bit could hide
    -Wundefined-internal only then). tokrefs=True checks, as references, every
    name the unit's identifier tokens spell (covering what the AST drops).
    Beside the oracle, "unsafe2" counts reuses whose oracle with every such
    name as a dependency differs (oracles_tok)."""
    store, ids, out = {}, 0, []

    def run(b, phase, reuse):
        nonlocal ids
        xs = b[units]
        ok = ("orc_occ_" if occurrence else "orc_") + units
        if ok not in b:
            b[ok] = (oracles_full(b, xs, occurrence), oracles_tok(b, xs, occurrence))
        if "sseq_" + units not in b:
            seq = [(x["order"], 1, k) for k, x in enumerate(xs)] + [(o, 0, n) for o, n in b["mains"]]
            seq.sort(key=lambda s: (s[0], s[1]))
            b["sseq_" + units] = seq
        (orc, orct), cfg = b[ok], b["fconfig"]
        key_of = ((lambda x: (cfg, x["loc"][1], x["loc"][2], x["occ"])) if occurrence else
                  (lambda x: (cfg, x["loc"][1], x["loc"][2])))
        changed = {}                         # key -> what the store held there before this compile
        processed = {}                       # key -> the version this compile has there
        decls = collections.defaultdict(list)  # name -> its declarations so far, as (key, version)
        ver = [None] * len(xs)
        why, why_ns, unsafe = collections.Counter(), collections.Counter(), collections.Counter()
        saved = saved_def = restored = unsafe2 = 0
        tokdeps = collections.Counter()
        reused, undefined_end = [], False
        # Why each unit this compile parsed was parsed, traced through uses
        # and deps to the refusal that started it: (reason, file), or
        # ("stale", file) for a use whose stored version is older than the
        # one this compile restored there, ("absent", file) for one this
        # compile did not process.
        root_of, roots = {}, collections.Counter()
        def root(lu):
            if lu not in processed:
                return ("absent", lu[1])
            return root_of.get(lu) or ("stale", lu[1])
        for order, kind, item in b["sseq_" + units]:
            if kind == 0:
                decls[item].append(("main", phase, b["i"], order))
                continue
            k, x = item, xs[item]
            if x["empty"]:
                why["empty"] += 1
                continue
            key = key_of(x)
            v = store.get(key)
            sv = x["server"]
            names = set(x["binds"]) | x["refs"]
            if tokrefs:
                names |= {(ns, n) for n in x["toknames"] for ns in (0, 1)}
            if not reuse:
                reason = "fallback"
            elif x["snested"] if fixed else x["nested"]:
                reason = "nested"
            elif x["flags"] & 4:
                reason = "param"
            elif x["flags"] & 3:
                reason = "main"
            elif sv & SV_BUILTIN:
                reason = "builtin"
            elif sv & SV_UNDEFINED and used == "design":
                reason = "undefined"
            elif sv & SV_ARGS:
                reason = "macro args"
            elif sv & SV_PENDING:
                reason = "pending"
            elif key in processed:
                reason = "repeat"
            elif v is None:
                reason = "new"
            elif v.shape != x["shape"]:
                reason = "conditionals"
            elif any(processed.get(lu) != vu for lu, vu in v.uses):
                reason = "uses"
            elif v.mlist != x["mlist"]:
                reason = "macro"
            elif v.prag != x["prag"] or sv & (SV_WEAK | SV_ATTRIBUTE):
                reason = "pragma"
            else:
                neg = dep = pred = False
                tokonly = []
                for name, stored in v.lists.items():
                    if tuple(decls.get(name, ())) != stored:
                        if name not in v.bset:
                            dep = True
                            if name not in x["refs"]:
                                tokonly.append(name)
                        elif stored:
                            pred = True
                        else:
                            neg = True
                if dep and not neg and len(tokonly) == sum(1 for nm, st in v.lists.items()
                                                           if nm not in v.bset and tuple(decls.get(nm, ())) != st) \
                        and not any(processed.get(lu) != vu for lu, vu in v.deps):
                    # Refused only for names its identifier tokens spell and its AST does not reference.
                    for name in tokonly:
                        tokdeps[(x["loc"][1], name[1])] += 1
                dep = dep or any(processed.get(lu) != vu for lu, vu in v.deps)
                reason = "negative" if neg else "deps" if dep else "predecessor" if pred else None
                if dep and not neg:
                    bad = [lu for lu, vu in v.deps if processed.get(lu) != vu]
                    dep_root = root(bad[0]) if bad else ("earlier declarations", x["loc"][1])
                if reason is None and v.oracle != orc[k]:
                    reason = "unsafe"
                    unsafe[("tokens" if v.tok != x["tok"] else "earlier declarations", x["loc"][1])] += 1
            if reason is None:               # restored
                processed[key] = ver[k] = v.id
                for name in v.binds:
                    decls[name].append((key, v.id))
                saved += x["ns"]
                saved_def += x.get("defns", 0)
                restored += len(v.binds)
                why["reused"] += 1
                why_ns["reused"] += x["ns"]
                reused.append(k)
                undefined_end = undefined_end or bool(sv & SV_UNDEFINED_END)
                if v.oracle2 != orct[k]:
                    unsafe2 += 1
            else:                            # parsed: a new version
                why[reason] += 1
                why_ns[reason] += x["ns"]
                if reason == "uses":
                    r0 = root(next(lu for lu, vu in v.uses if processed.get(lu) != vu))
                elif reason == "deps":
                    r0 = dep_root
                else:
                    r0 = (reason, x["loc"][1])
                root_of[key] = r0
                if reason in ("uses", "deps"):
                    roots[(reason,) + r0] += x["ns"]
                ids += 1
                nv = ServerVersion()
                nv.id, nv.mlist, nv.prag, nv.oracle, nv.shape = ids, x["mlist"], x["prag"], orc[k], x["shape"]
                nv.tok, nv.oracle2 = x["tok"], orct[k]
                nv.uses = [(key_of(xs[u]), ver[u]) for u in x["uses"]]
                nv.deps = [(key_of(xs[u]), ver[u]) for u in x["deps"]]
                nv.binds, nv.bset = list(x["binds"]), frozenset(x["binds"])
                nv.lists = {n: tuple(decls.get(n, ())) for n in names}
                changed.setdefault(key, store.get(key))
                store[key] = nv
                processed[key] = ver[k] = ids
                for name in x["binds"]:
                    decls[name].append((key, ids))
            if trace is not None and phase == 1 and b["i"] == trace:
                print(f"  {x['loc'][1]}:{x['loc'][2]}#{x['occ']}  {reason or 'reused':12} {x['ns'] / 1e3:8.1f} us")
        res = {"i": b["i"], "why": why, "why_ns": why_ns, "saved": saved, "saved_def": saved_def, "unsafe": unsafe,
               "unsafe2": unsafe2, "reused": reused, "restored": restored, "roots": roots, "tokdeps": tokdeps}
        return res, changed, undefined_end

    for phase in range(phases):
        res = []
        for b in builds:
            r, changed, undefined_end = run(b, phase, True)
            if used == "fallback" and undefined_end:
                # A restored internal or inline function ends the compile
                # undefined: nothing of this run is kept, and it runs again
                # without reuse.
                for key, old in changed.items():
                    if old is None:
                        del store[key]
                    else:
                        store[key] = old
                r, _, _ = run(b, phase, False)
            res.append(r)
        out.append(res)
    return out


# --- Scoring ---------------------------------------------------------------
def plain_of(b):
    if recomp and str(b["i"]) in recomp["compiles"]:
        return recomp["compiles"][str(b["i"])]["plain"], recomp["compiles"][str(b["i"])]["cache"]
    return b["plainM"], None


# What a persistent server skips besides parsing, credited per compile (s).
# "start-up" is the headline: compiler initialization (p.24's "initially"
# level), measured as a one-line file's compile less the driver alone with
# redis's flags; his driver still ran for every request (p.22-23).
CREDITS = {
    "none": lambda b: 0.0,
    "start-up": lambda b: CREDIT,
    "start-up + header files": lambda b: CREDIT + b["files"],  # p.24's cache of header text, at most
    "whole process": lambda b: max(0.0, plain_of(b)[0] - b["span"]) + b["pre"],
}

if HAS_SERVER:  # the model's variants under the fixed observer (2.9)
    VARIANTS.update({"R1 V1 fixed": ("R1", "V1", False, True), "R0 V1 fixed": ("R0", "V1", False, True),
                     "O3 R1 V1 fixed": ("R1", "V1", True, True), "ideal fixed": ("R2", "ideal", False, True)})
if args.trace is not None:
    print(f"compile {args.trace}, {args.variant}, recompilation:")
    if args.variant in SERVER:
        simulate_server(SERVER[args.variant], phases=2, trace=args.trace)
    else:
        simulate(*VARIANTS[args.variant], trace=args.trace)
    sys.exit()

results = {name: simulate(*v) for name, v in VARIANTS.items()}
by_build = {b["i"]: b for b in builds}
inst = lambda opt3: [x for b in builds for x in (b["xs3"] if opt3 else b["xs"])]
for opt3 in (False, True):
    xs = inst(opt3)
    print(f"{len(builds)} compiles; recompilation, option {3 if opt3 else 2}: {len(xs)} fragment instances "
          f"({sum(x['empty'] for x in xs)} empty), {sum(x['ns'] for x in xs) / 1e9:.2f} s in them"
          + (" (with their #define lines)" if opt3 else ""))
REASONS = ["reused", "nested", "param", "main", "repeat", "new", "changed", "macro", "uses", "unsafe"]
for what in ("time", "instances"):
    print(f"\nShare of {what}{' (empty instances apart)' if what == 'instances' else ''}:")
    print(f"{'variant':9} " + " ".join(f"{r:>8}" for r in REASONS))
    for name, res in results.items():
        xs = inst(VARIANTS[name][2])
        why = collections.Counter()
        for r in res:
            why.update(r["why_ns" if what == "time" else "why"])
        total = sum(x["ns"] for x in xs) if what == "time" else sum(not x["empty"] for x in xs)
        print(f"{name:9} " + " ".join(f"{why[r] / total * 100:7.1f}%" if why[r] else f"{'-':>8}" for r in REASONS))
for name in ("R1 V1", "R1 V0", "R2 V1", "O3 R1 V1"):
    u = collections.Counter()
    for r in results[name]:
        u.update(r["unsafe"])
    causes = collections.Counter()
    for (c, _), n in u.items():
        causes[c] += n
    print(f"\n{name}: unsafe decisions by cause: {dict(causes)}; most frequent places:")
    for (c, where), n in u.most_common(6):
        print(f"  {n:6}  {c:22} {where}")


def saved_of(name, i):
    """Seconds a row saves in compile i, before scaling."""
    if name == "every":
        return sum(x["ns"] for x in by_build[i]["xs"]) / 1e9
    if name == "every3":
        return sum(x["ns"] for x in by_build[i]["xs3"]) / 1e9
    base, nodef = (name[:-5], True) if name.endswith(" -def") else (name, False)
    r = per_build[base][i]
    return (r["saved"] - (r["saved_def"] if nodef else 0)) / 1e9


per_build = {name: {r["i"]: r for r in res} for name, res in results.items()}


def row(name, bs, credit):
    """(total seconds, speed-up of the total, geometric mean of the
    per-compile speed-ups) against plain clang; the mean is None where a
    compile's time comes out at or below zero."""
    P = sum(plain_of(b)[0] for b in bs)
    if name == "cache":
        times = [plain_of(b)[1] for b in bs]
    else:
        times = [plain_of(b)[0] - SCALE * saved_of(name, b["i"]) - credit(b) for b in bs]
    T = sum(times)
    g = statistics.geometric_mean([plain_of(b)[0] / t for b, t in zip(bs, times)]) if min(times) > 0 else None
    return T, P / T, g


GROUPS = [g for g in [("all", lambda b: True), ("redis-server (src/)", lambda b: "/redis/src" in b["cwd"])]
          if any(g[1](b) for b in builds)]
LABEL = {"cache": "header cache, recompilation", "every": "every fragment reused",
         "every3": "every option-3 fragment reused"}
HAS_CACHE = all(plain_of(b)[1] is not None for b in builds)
ROWS = (["cache"] if HAS_CACHE else []) + list(results) + ["O3 R1 V1 -def", "every", "every3"]
for label, sel in GROUPS:
    bs = [b for b in builds if sel(b)]
    print(f"\n{label}: {len(bs)} compiles; plain clang {sum(plain_of(b)[0] for b in bs):.2f} s; compiler start-up "
          f"credited {CREDIT * 1e3:.2f} ms per compile in every Bothner row"
          + (f"; time saved by reuse scaled by {SCALE}" if SCALE != 1 else ""))
    for name in ROWS:
        T, x, g = row(name, bs, CREDITS["start-up"])
        print(f"  {LABEL.get(name, 'Bothner ' + name):32} {T:7.2f} s  {x:5.2f}x in total, "
              + (f"{g:5.2f}x geometric mean per compile" if g else "geometric mean undefined"))

if args.markdown:
    PAPER = [("Header cache", "cache"),
             ("Option 2 (as he built it), our repeat rule R1, macro lists", "R1 V1"),
             ("Option 3 (his preferred design), R1, macro lists", "O3 R1 V1"),
             ("the same, its #define lines' time not counted", "O3 R1 V1 -def"),
             ("Option 2, his repeat rule as written (R0), macro lists", "R0 V1"),
             ("Option 2, R1, no macro checks (as his 2003 prototype)", "R1 V0"),
             ("Option 2, R1, p.30's two bits for macros", "R1 V2"),
             ("Option 2, the repeat rule most favourable to him (R2), macro lists", "R2 V1"),
             ("Option 3, R2, macro lists", "O3 R2 V1"),
             ("Ideal fragment cache, option 2: any earlier identical parse", "ideal"),
             ("Ideal fragment cache, option 3", "O3 ideal"),
             ("Ceiling: every fragment reused, option 2", "every"),
             ("Ceiling: every option-3 fragment reused, #define lines included", "every3")]
    fmt = lambda T, x, g: f"{T:.2f} s: **{x:.2f}x** (" + (f"{g:.2f}x" if g else "n/a") + ")"
    L = ["| redis, recompilation, front end | " + " | ".join(
            f"{label}: {sum(1 for b in builds if sel(b))} compiles, plain clang "
            f"{sum(plain_of(b)[0] for b in builds if sel(b)):.2f} s" for label, sel in GROUPS)
         + " | Header-fragment time reused | Unsafe reuses (charged as parses) |",
         "|---|" + "---|" * (len(GROUPS) + 2)]
    for text, name in PAPER:
        if name == "cache" and not HAS_CACHE:
            continue
        cells = [fmt(*row(name, [b for b in builds if sel(b)], CREDITS["start-up"])) for _, sel in GROUPS]
        if name in results:
            why_ns, why = collections.Counter(), collections.Counter()
            for r in results[name]:
                why_ns.update(r["why_ns"])
                why.update(r["why"])
            total = sum(x["ns"] for x in inst(VARIANTS[name][2]))
            cells += [f"{why_ns['reused'] / total * 100:.1f}%", f"{why['unsafe']:,}"]
        else:
            cells += ["", ""]
        L.append(f"| {text} | " + " | ".join(cells) + " |")
    L += ["", "Times are totals over the compiles; bold, the speed-up of the total; in parentheses, the geometric "
          "mean of the per-compile speed-ups. Bothner rows: restoring a reused fragment costs nothing, "
          f"{CREDIT * 1e3:.2f} ms of compiler start-up is credited per compile"
          + (f", and the time each reuse saves is scaled by {SCALE}" if SCALE != 1 else "")
          + ". Option 3 also counts its #define lines' time as saved (replayed for free; not checked by "
          "blank.py). The ceilings reuse what every rule rejects: nested fragments and repeats.", "",
          "Speed-up of the total under each credit for what a persistent server skips (all compiles / "
          "redis-server):", "",
          "| | " + " | ".join(CREDITS) + " |", "|---|" + "---|" * len(CREDITS)]
    for text, name in PAPER:
        if name == "cache":
            if HAS_CACHE:
                L.append("| Header cache | " + " | ".join(
                    " / ".join(f"{row('cache', [b for b in builds if sel(b)], c)[1]:.2f}x" for _, sel in GROUPS)
                    for c in CREDITS.values()) + " |")
            continue
        L.append(f"| {text} | " + " | ".join(
            " / ".join(f"{row(name, [b for b in builds if sel(b)], c)[1]:.2f}x" for _, sel in GROUPS)
            for c in CREDITS.values()) + " |")
    L += ["", "Credits per compile: none; compiler start-up, 1.65 ms; that and a server's cache of header text "
          "(p.24), at most the calibrated cost of each #include and file entry; or the whole process start "
          "outside the compile's own work, which his design did not save (his driver still ran)."]
    open(args.markdown, "w").write("\n".join(L) + "\n")

if args.reused:
    key = "xs3" if VARIANTS[args.variant][2] else "xs"
    json.dump({str(r["i"]): [[by_build[r["i"]][key][k]["loc"][1], by_build[r["i"]][key][k]["loc"][2],
                              by_build[r["i"]][key][k]["occ"]] for k in r["reused"]]
               for r in results[args.variant]}, open(args.reused, "w"))

if args.out:
    json.dump({name: [{"i": r["i"], "src": by_build[r["i"]]["src"], "why": dict(r["why"]), "why_ns": dict(r["why_ns"]),
                       "saved_ns": r["saved"], "saved_def_ns": r["saved_def"]}
                      for r in res] for name, res in results.items()}, open(args.out, "w"))

# --- The compile server's predicted rows (S0) -------------------------------
if HAS_SERVER:
    cfgs = collections.Counter(b["config"] for b in builds)
    fcfgs = collections.Counter(b["fconfig"] for b in builds)
    print(f"\nCompile server (S0): {len(cfgs)} configurations under the model's identity, {len(fcfgs)} under the "
          f"server's key (compiles each: {sorted(fcfgs.values())}); {PHASES} passes, the first a fill")
    LABEL_S = {"S2": "option 2", "S3": "option 3", "S2G": "option 2 + groups", "S3G": "option 3 + groups",
               "S3G*": "option 3 + groups*"}
    # Runs: the rule as designed (2.8's refusal), the same under the old
    # observer, with 2.8's fallback instead, and beyond R0 a version per
    # reading; then every identifier token checked as a reference.
    RUNS = {}
    for name in SERVER:
        RUNS[name] = (name, dict())
        RUNS[name + " old"] = (name, dict(fixed=False))
        RUNS[name + " fb"] = (name, dict(used="fallback"))
    for name in ("S2", "S3G*"):
        RUNS[name + " fb occ"] = (name, dict(used="fallback", occurrence=True))
        RUNS[name + " fb tok"] = (name, dict(used="fallback", tokrefs=True))
    RUNS["S3G* fb occ tok"] = ("S3G*", dict(used="fallback", occurrence=True, tokrefs=True))
    sres = {label: simulate_server(SERVER[name], **kw) for label, (name, kw) in RUNS.items()}

    def describe(label):
        name, kw = RUNS[label]
        return (LABEL_S[name] + (", old observer" if kw.get("fixed") is False else "")
                + (", fallback" if kw.get("used") == "fallback" else "") + (", per reading" if kw.get("occurrence") else "")
                + (", token refs" if kw.get("tokrefs") else ""))

    def header_time(label):
        return sum(x["ns"] for b in builds for x in b[SERVER[RUNS[label][0]]])

    def server_row(res, bs, credit, nodef=False):
        """As row(), from one pass's per-compile results."""
        by_i = {r["i"]: r for r in res}
        P = sum(plain_of(b)[0] for b in bs)
        times = [plain_of(b)[0] - SCALE * (by_i[b["i"]]["saved"] - (by_i[b["i"]]["saved_def"] if nodef else 0)) / 1e9
                 - credit(b) for b in bs]
        T = sum(times)
        g = statistics.geometric_mean([plain_of(b)[0] / t for b, t in zip(bs, times)]) if min(times) > 0 else None
        return T, P / T, g

    print("\nRecompilation (pass 2): share of header time / of instances (empty apart), by reason:")
    print(f"{'run':44} " + " ".join(f"{r[:8]:>8}" for r in SERVER_REASONS))
    for label, (name, kw) in RUNS.items():
        why_ns, why = collections.Counter(), collections.Counter()
        for r in sres[label][1]:
            why_ns.update(r["why_ns"])
            why.update(r["why"])
        T = header_time(label)
        N = sum(not x["empty"] for b in builds for x in b[SERVER[name]])
        print(f"{describe(label):44} " + " ".join(f"{why_ns[r] / T * 100:7.1f}%" if why_ns[r] else f"{'-':>8}"
                                                  for r in SERVER_REASONS))
        print(f"{'':44} " + " ".join(f"{why[r] / N * 100:7.1f}%" if why[r] else f"{'-':>8}" for r in SERVER_REASONS))

    print("\nChecks of the rule, recompilation: reuses the oracle rejects (unsafe), and reuses an oracle that also "
          "depends on every name an identifier token spells rejects (unsafe2; a name a member, parameter or local "
          "shares with a file-scope declaration makes false ones); and passes: header time reused per pass, with "
          "the compiles whose reused instances differ from the pass before")
    for label in RUNS:
        res = sres[label]
        u = collections.Counter()
        for r in res[1]:
            u.update(r["unsafe"])
        T = header_time(label)
        cells = []
        for ph, rs in enumerate(res):
            diff = "" if ph == 0 else f" ({sum(a['reused'] != c['reused'] for a, c in zip(rs, res[ph - 1]))})"
            cells.append(f"{sum(r['saved'] for r in rs) / T * 100:5.1f}%{diff}")
        print(f"  {describe(label):44} unsafe {sum(u.values()):3}, unsafe2 {sum(r['unsafe2'] for r in res[1]):5};  "
              + "  ".join(cells))

    for glabel, sel in GROUPS:
        bs = [b for b in builds if sel(b)]
        print(f"\n{glabel}: {len(bs)} compiles; plain clang {sum(plain_of(b)[0] for b in bs):.2f} s; start-up "
              f"credited {CREDIT * 1e3:.2f} ms per compile; restoring free; columns: recompilation, first build, "
              "recompilation with #define lines not saved (option 3)")
        for name in ("cache",) if HAS_CACHE else ():
            T, x, g = row(name, bs, CREDITS["start-up"])
            print(f"  {'header cache':52} {T:6.2f} s {x:5.2f}x " + (f"({g:4.2f}x)" if g else ""))
        for name in ("R1 V1", "R0 V1", "O3 R1 V1 fixed", "ideal fixed"):
            T, x, g = row(name, bs, CREDITS["start-up"])
            print(f"  {'model ' + name:52} {T:6.2f} s {x:5.2f}x " + (f"({g:4.2f}x)" if g else ""))
        for label, (name, kw) in RUNS.items():
            T, x, g = server_row(sres[label][1], bs, CREDITS["start-up"])
            T0, x0, _ = server_row(sres[label][0], bs, CREDITS["start-up"])
            Tn, xn, _ = server_row(sres[label][1], bs, CREDITS["start-up"], nodef=True)
            print(f"  {'server ' + describe(label):52} {T:6.2f} s {x:5.2f}x " + (f"({g:4.2f}x)" if g else "       ")
                  + f"   first build {x0:5.2f}x" + (f"   no #define {xn:5.2f}x" if name in ("S3", "S3G", "S3G*") else ""))

    for label in ("S2", "S2G", "S3G*", "S3G* fb", "S3G* fb occ"):
        T = header_time(label)
        roots, by_reason = collections.Counter(), collections.Counter()
        for r in sres[label][1]:
            roots.update(r["roots"])
            for (what, why0, _), ns in r["roots"].items():
                by_reason[(what, why0)] += ns
        print(f"\n{describe(label)}: uses and deps refusals by the refusal that started them (share of header "
              "time): " + "; ".join(f"{w} <- {r0} {ns / T * 100:.1f}%" for (w, r0), ns in by_reason.most_common(6)))
        for (w, r0, where), ns in roots.most_common(6):
            print(f"    {ns / T * 100:5.1f}%  {w:5} <- {r0:12} {where}")
    for label in ("S2 fb tok", "S3G* fb tok"):
        td = collections.Counter()
        for r in sres[label][1]:
            td.update(r["tokdeps"])
        byname = collections.Counter()
        for (f, n), c in td.items():
            byname[n] += c
        print(f"\n{describe(label)}: refusals caused only by names the AST does not reference: {sum(td.values())}; "
              "names: " + ", ".join(f"{n} {c}" for n, c in byname.most_common(25)))
        for (f, n), c in td.most_common(12):
            print(f"    {c:5}  {n:24} {f}")
    for label in ("S2", "S3G* fb", "S3G* fb occ"):
        rb = [r["restored"] for r in sres[label][1]]
        print(f"\nDeclarations restored per compile, {describe(label)}: median {statistics.median(rb):.0f}, "
              f"max {max(rb)}")

    if args.server_out:
        json.dump({label: [[{"i": r["i"], "why": dict(r["why"]), "why_ns": dict(r["why_ns"]), "saved_ns": r["saved"],
                             "saved_def_ns": r["saved_def"], "restored": r["restored"], "unsafe2": r["unsafe2"]}
                            for r in res] for res in v] for label, v in sres.items()}, open(args.server_out, "w"))
