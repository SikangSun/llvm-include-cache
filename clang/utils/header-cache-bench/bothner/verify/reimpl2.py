"""Independent reimplementation of the compile server's rule (S0), round 2:
round 1's reimpl.py brought to the CURRENT specification, from simulate.py's
docstrings (the file's and simulate_server's), COMPILE_SERVER_DESIGN.md
2.4-2.9 and BothnerTrace's record-format comment only. Not read before the
results were in: simulate_server(), oracles_tok(), groups_of().

Spec deltas over round 1:
  main         record flags & 3 (1: uses a main-file declaration; 2: an earlier
               declaration of what it references is one);
  main ids     main-file declarations in the per-name sequences are unique per
               (pass, compile[, run]);
  undefined    SV_UNDEFINED (32), right after builtin, in the design runs;
  " fb"        skips "undefined"; a compile that reuses any unit with
               SV_UNDEFINED_END (64) has every store change undone and runs
               again with every unit parsed ("fallback");
  " tok"       the names whose earlier-declaration sequences a version stores
               and checks also get (0, n) and (1, n) for every identifier-token
               name n of the unit (frag field 25; spans/groups: union);
  unsafe2      reuses whose stored token oracle differs from this compile's;
  " old"       nesting read from the old observer (frag fields 8/9).

usage: reimpl2.py <record dir> <timing dir> <out.json> [--variants a,b,...]
"""
import argparse, collections, json, os, sys

SV_BUILTIN, SV_ARGS, SV_PENDING, SV_WEAK, SV_ATTRIBUTE, SV_UNDEFINED, SV_UNDEFINED_END = 1, 2, 4, 8, 16, 32, 64
SEP_COND, SEP_DEFINE, SEP_INCLUDE, SEP_PRAGMA, SEP_OTHER, SEP_FIRST = 1, 2, 4, 8, 16, 32

ALL = ("S2,S2 old,S2 fb,S3,S3 old,S3 fb,S2G,S2G old,S2G fb,S3G,S3G old,S3G fb,S3G*,S3G* old,S3G* fb,"
       "S2 fb occ,S2 fb tok,S3G* fb occ,S3G* fb tok,S3G* fb occ tok")

ap = argparse.ArgumentParser()
ap.add_argument("records")
ap.add_argument("timing")
ap.add_argument("out")
ap.add_argument("--variants", default=ALL)
ap.add_argument("--phases", type=int, default=5)
# Readings where the specification leaves a choice (defaults: this reading).
ap.add_argument("--group-sv", default="union", choices=["union", "first"],
                help="a group's SV_* flags: any member's, or the first member's (plus SV_BUILTIN of any)")
ap.add_argument("--group-observer", default="fixed", choices=["fixed", "run"],
                help="groups are formed under the fixed observer always, or under the run's observer")
ap.add_argument("--mark-members", default="no", choices=["no", "yes"],
                help="a processed group also marks its other members' keys as processed (repeat check)")
ap.add_argument("--empty-sep", default="acc", choices=["acc", "break"],
                help="an empty unit between two units of a group: transparent (acc) or breaks the run")
ap.add_argument("--deps-missing", default="yes", choices=["yes", "no"],
                help="a record dep not processed at all in this compile refuses (yes) or not (no)")
ap.add_argument("--tok-declared", default="declared", choices=["declared", "ref"],
                help="a token name the unit also declares: classified as declared (negative/predecessor), "
                     "or also checked as a reference (deps)")
ap.add_argument("--tok-oracle-rec", default="tok", choices=["tok", "ast"],
                help="the token oracle's deps contribute their token oracles (tok) or their AST oracles (ast)")
ap.add_argument("--tok-binders", default="all", choices=["all", "latest"],
                help="token-name deps: every earlier unit declaring (0,n)/(1,n), or only the latest per name")
ap.add_argument("--tok-main", default="no", choices=["no", "yes"],
                help="the token oracle also depends on the main file when an earlier main-file declaration "
                     "binds a token name")
ap.add_argument("--tok-sameloc", default="skip", choices=["skip", "keep"],
                help="token-name deps at the unit's own location are left out (as oracles()) or kept")
ap.add_argument("--fallback-why", default="all", choices=["all", "reasons"],
                help="in a fallback rerun every non-empty unit counts as 'fallback' (all), or units keep their "
                     "refusal reasons and would-be reuses count as 'fallback' (reasons)")
ap.add_argument("--main-id", default="run", choices=["run", "index"],
                help="identity of a main-file declaration: per run (pass, compile, attempt) or per compile index")
ap.add_argument("--key", default="key", choices=["key", "start"],
                help="offset in the key: post-directive offset (Frag::Key, Span::Key) or first-token offset")
ap.add_argument("--config", default="full", choices=["full", "config"],
                help="configuration in the key: full_config (server key) or config (model key)")
ap.add_argument("--trace", help="variant:pass:log index -> print every decision of that compile")
args = ap.parse_args()


class U:
    """A unit the server decides on: an option-2 fragment instance, an
    option-3 span, or a fragment group of either."""
    __slots__ = ("key", "base", "empty", "sb", "sa", "ob", "oa", "flags", "server", "tok", "prag",
                 "ids", "uses", "deps", "binds", "refs", "toknames", "sep", "ns", "defns", "defines",
                 "defnames", "order", "entry", "shape", "members", "oracle", "oracle2", "idx")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def copy(self):
        return U(**{s: getattr(self, s) for s in U.__slots__})


def load(records, timing):
    builds = []
    for f in sorted(os.listdir(records)):
        if not f.endswith(".json"):
            continue
        r = json.load(open(os.path.join(records, f)))
        t = json.load(open(os.path.join(timing, f)))
        if not r["data"] or not t["data"]:
            continue
        rd, td = r["data"], t["data"]
        assert len(rd["frags"]) == len(td["frags"]), f
        names = rd["names"]
        fc = rd["full_config"] if args.config == "full" else rd["config"]
        xs = []
        for rec, (a, b) in enumerate(zip(rd["frags"], td["frags"])):
            nm = rd["entries"][a[0]][0]
            assert (nm, a[1]) == (td["entries"][b[0]][0], b[1]), f
            assert len(a) == 26, (f, len(a))
            ids = tuple(sorted((names[n], s) for n, s in a[15]))
            off = a[23] if args.key == "key" else a[1]
            x = U(key=(fc, nm, off), base=(fc, nm, off), empty=a[8] == -1, sb=a[17], sa=a[18], ob=a[8], oa=a[9],
                  flags=a[13], server=a[20], tok=a[7], prag=a[19], ids=ids, uses=a[14], deps=a[16],
                  binds=[(ns, names[n]) for ns, n in a[21]], refs=frozenset((ns, names[n]) for ns, n in a[22]),
                  toknames=frozenset(names[n] for n in a[25]),
                  sep=a[24], ns=b[5], defns=0, defines=0, defnames=frozenset(), order=a[10], entry=a[0])
            xs.append(x)
        byrec = list(xs)
        order = sorted(range(len(xs)), key=lambda k: xs[k].order)
        pos = {k: p for p, k in enumerate(order)}
        xs = [xs[k] for k in order]
        for p, x in enumerate(xs):
            x.uses = sorted({pos[u] for u in x.uses})
            x.deps = sorted({pos[u] for u in x.deps})
            x.idx = p
        # Option 3 spans, built as simulate.py's loader builds them.
        assert len(rd["spans"]) == len(td["spans"]), f
        ys = []
        span_of = {}
        for rec, (sr, st) in enumerate(zip(rd["spans"], td["spans"])):
            nm = rd["entries"][sr[0]][0]
            assert (nm, sr[1]) == (td["entries"][st[0]][0], st[1]), f
            allms = [byrec[m] for m in sr[3]]
            ms = sorted((m for m in allms if not m.empty), key=lambda m: m.order)
            defs = sorted((e, names[n]) for e, n in sr[10])
            defs_first = sr[6] >= 0 and (not ms or sr[6] < ms[0].order)
            first = {}
            for m in ms:
                set_before = {n for e, n in defs if e < m.order}
                for n, v in m.ids:
                    if n not in set_before:
                        first.setdefault(n, v)
            ids = tuple(sorted(first.items()))
            flags = 0
            for m in ms:
                flags |= m.flags
            sv = sr[14] if defs_first else (ms[0].server if ms else 0)
            for m in ms:
                sv |= m.server
            off = sr[15] if args.key == "key" else sr[1]
            y = U(key=(fc, nm, off), base=(fc, nm, off), empty=(not ms and sr[6] < 0),
                  sb=sr[11] if defs_first else (ms[0].sb if ms else -1),
                  sa=ms[-1].sa if ms else sr[12],
                  ob=sr[7] if defs_first else (ms[0].ob if ms else -1),
                  oa=ms[-1].oa if ms else sr[8],
                  flags=flags, server=sv, tok=hash((tuple(m.tok for m in ms), sr[4])),
                  prag=sr[13] if defs_first else (ms[0].prag if ms else ""),
                  ids=ids, binds=[bd for m in ms for bd in m.binds],
                  refs=frozenset().union(*(m.refs for m in ms)),
                  toknames=frozenset().union(*(m.toknames for m in ms)), sep=sr[16],
                  ns=sum(m.ns for m in ms) + st[5], defns=st[5], defines=sr[4],
                  defnames=frozenset(n for _, n in defs),
                  order=min(([sr[6]] if sr[6] >= 0 else []) + [m.order for m in ms] or [allms[0].order]),
                  entry=sr[0])
            y.members = ms
            for m in ms:
                span_of[id(m)] = y
            ys.append(y)
        ys.sort(key=lambda y: y.order)
        pos3 = {id(y): p for p, y in enumerate(ys)}
        for p, y in enumerate(ys):
            y.idx = p
            for fld in ("uses", "deps"):
                out = set()
                for m in y.members:
                    for u in getattr(m, fld):
                        q = pos3[id(span_of[id(xs[u])])]
                        if q != p:
                            out.add(q)
                setattr(y, fld, sorted(out))
        for y in ys:
            y.members = None
        mains = sorted((o, (ns, names[n])) for o, ns, n in rd["main_decls"])
        builds.append({"i": r["meta"]["i"], "cwd": r["meta"]["cwd"], "src": r["meta"]["src"],
                       "fconfig": fc, "xs": xs, "xs3": ys, "mains": mains})
    return builds


def make_units(groups):
    """Units from member lists (each a list of base units, in processing order
    of their first member); uses and deps mapped to units, less the unit's own."""
    gof = {}
    for gi, ms in enumerate(groups):
        for m in ms:
            gof[m.idx] = gi
    units = []
    for gi, ms in enumerate(groups):
        if len(ms) == 1:
            g = ms[0].copy()
        else:
            g = U()
            g.key = g.base = ms[0].base
            g.empty = False
            g.sb, g.sa, g.ob, g.oa = ms[0].sb, ms[-1].sa, ms[0].ob, ms[-1].oa
            g.flags = 0
            for m in ms:
                g.flags |= m.flags
            if args.group_sv == "union":
                g.server = 0
                for m in ms:
                    g.server |= m.server
            else:
                g.server = ms[0].server
                for m in ms:
                    g.server |= m.server & SV_BUILTIN
            g.tok = hash(tuple(m.tok for m in ms))
            g.prag = ms[0].prag
            first = {}
            defined = set()
            for m in ms:
                for n, v in m.ids:
                    if n not in defined:
                        first.setdefault(n, v)
                defined |= m.defnames
            g.ids = tuple(sorted(first.items()))
            g.binds = [bd for m in ms for bd in m.binds]
            g.refs = frozenset().union(*(m.refs for m in ms))
            g.toknames = frozenset().union(*(m.toknames for m in ms))
            g.sep = ms[0].sep
            g.ns = sum(m.ns for m in ms)
            g.defns = sum(m.defns for m in ms)
            g.defines = sum(m.defines for m in ms)
            g.defnames = frozenset().union(*(m.defnames for m in ms))
            g.order = ms[0].order
            g.entry = ms[0].entry
        g.members = ms
        g.shape = tuple(m.base for m in ms)
        g.idx = gi
        for fld in ("uses", "deps"):
            s = set()
            for m in ms:
                for u in getattr(m, fld):
                    q = gof[u]
                    if q != gi:
                        s.add(q)
            setattr(g, fld, sorted(s))
        units.append(g)
    return units


def groups_mine(base, defines_ok, observer):
    """Fragment groups (p.33, 2.7) over base units in processing order: a
    maximal run of one header entry's units, joined where the parser is
    nested at the boundary and only conditionals separate them; without
    defines_ok, no unit holding #define lines joins; empty units are
    transparent (their separators count)."""
    groups = []
    last = {}  # entry -> index of the group holding its last non-empty unit
    acc = {}   # entry -> separator kinds since then
    for x in base:
        if x.empty:
            if args.empty_sep == "acc":
                acc[x.entry] = acc.get(x.entry, 0) | x.sep
            else:
                acc[x.entry] = acc.get(x.entry, 0) | x.sep | SEP_OTHER
            groups.append([x])
            continue
        sep = acc.pop(x.entry, 0) | x.sep
        gi = last.get(x.entry)
        join = False
        if gi is not None and sep == SEP_COND:
            pm = groups[gi]
            if observer == "fixed":
                nested = pm[-1].sa != 1 or x.sb != 1
            else:
                nested = pm[-1].oa != 1 or x.ob != 1
            ok = defines_ok or (x.defines == 0 and all(m.defines == 0 for m in pm))
            join = nested and ok
        if join:
            groups[gi].append(x)
        else:
            last[x.entry] = len(groups)
            groups.append([x])
    return make_units(groups)


def tokset(g):
    return frozenset((ns, n) for n in g.toknames for ns in (0, 1))


def oracles(b, units):
    """The AST oracle (oracles_full: the full pragma state), and the token
    oracle: as the AST oracle, with every unit holding an earlier
    declaration of a name its identifier tokens spell (in either namespace)
    as a dependency besides."""
    binders = collections.defaultdict(list)  # (ns, name) -> unit indices binding it, in order
    mains = b["mains"]
    mi = 0
    main_names = set()
    for gi, g in enumerate(units):
        while mi < len(mains) and mains[mi][0] < g.order:
            main_names.add(mains[mi][1])
            mi += 1
        if g.empty:
            g.oracle = g.oracle2 = None
        else:
            ds = sorted({units[d].oracle for d in g.deps if units[d].base != g.base and units[d].oracle is not None})
            mainx = (b["cwd"], b["src"]) if g.flags & 2 else None
            g.oracle = hash((g.tok, g.prag, tuple(ds), mainx))
            dset = {d for d in g.deps if units[d].base != g.base}
            tmain = False
            for nm in tokset(g):
                lst = binders.get(nm, ())
                if args.tok_binders == "latest":
                    lst = lst[-1:]
                for d in lst:
                    if args.tok_sameloc == "keep" or units[d].base != g.base:
                        dset.add(d)
                if nm in main_names:
                    tmain = True
            fld = "oracle2" if args.tok_oracle_rec == "tok" else "oracle"
            ds2 = sorted({getattr(units[d], fld) for d in dset if getattr(units[d], fld) is not None})
            mainx2 = mainx
            if args.tok_main == "yes" and tmain:
                mainx2 = (b["cwd"], b["src"])
            g.oracle2 = hash((g.tok, g.prag, tuple(ds2), mainx2))
        for bd in g.binds or ():
            binders[bd].append(gi)


def number_occ(units):
    seen = collections.Counter()
    for g in units:
        seen[g.base] += 1
        g.key = (g.base, seen[g.base])


class Ver:
    __slots__ = ("id", "shape", "uses", "deps", "mlist", "prag", "names", "declared", "binds", "oracle", "oracle2")


def simulate(builds, units_of, occ, observer, fb, tokrefs, phases, trace=None):
    store = {}
    nver = [0]
    passes = []

    def run(p, bi, b, reuse, attempt):
        units = units_of[bi]
        changes = {}  # key -> store value before this run's first change
        processed = {}
        decls = collections.defaultdict(list)
        mains = b["mains"]
        mi = 0
        why = collections.Counter()
        why_ns = collections.Counter()
        saved = saved_def = restored = unsafe2 = 0
        need_fallback = False
        for g in units:
            while mi < len(mains) and mains[mi][0] < g.order:
                if args.main_id == "run":
                    decls[mains[mi][1]].append(("main", p, bi, attempt, mains[mi][0]))
                else:
                    decls[mains[mi][1]].append(("main", bi, mains[mi][0]))
                mi += 1
            if g.empty:
                why["empty"] += 1
                if trace and trace == (p, b["i"]):
                    print(p, b["i"], g.key, "empty", g.ns)
                continue
            key = g.key
            s = store.get(key)
            if observer == "fixed":
                nested = not (g.sb == 1 and g.sa == 1)
            else:
                nested = not (g.ob == 1 and g.oa == 1)
            tnames = tokset(g) if tokrefs else frozenset()
            reason = None
            detail = None
            if nested:
                reason = "nested"
            elif g.flags & 4:
                reason = "param"
            elif g.flags & 3:
                reason = "main"
            elif g.server & SV_BUILTIN:
                reason = "builtin"
            elif not fb and g.server & SV_UNDEFINED:
                reason = "undefined"
            elif g.server & SV_ARGS:
                reason = "macro args"
            elif g.server & SV_PENDING:
                reason = "pending"
            elif key in processed:
                reason = "repeat"
            elif s is None:
                reason = "new"
            elif s.shape != g.shape:
                reason = "conditionals"
            elif any(processed.get(k) != v for k, v in s.uses):
                reason = "uses"
                detail = [(k, v, processed.get(k)) for k, v in s.uses if processed.get(k) != v][:3]
            elif s.mlist != g.ids:
                reason = "macro"
            elif s.prag != g.prag or g.server & (SV_WEAK | SV_ATTRIBUTE):
                reason = "pragma"
            else:
                neg = dep = pred = False
                for nm, seq in s.names.items():
                    cur = tuple(decls.get(nm, ()))
                    if cur != seq:
                        if nm in s.declared:
                            if not seq:
                                neg = True
                            else:
                                pred = True
                            if args.tok_declared == "ref" and nm in tnames:
                                dep = True
                        else:
                            dep = True
                        if detail is None:
                            detail = (nm, seq, cur)
                if not dep:
                    for k, v in s.deps:
                        pv = processed.get(k)
                        if pv is None and args.deps_missing == "no":
                            continue
                        if pv != v:
                            dep = True
                            detail = ("dep", k, v, pv)
                            break
                if neg:
                    reason = "negative"
                elif dep:
                    reason = "deps"
                elif pred:
                    reason = "predecessor"
                elif s.oracle != g.oracle:
                    reason = "unsafe"
            if not reuse:
                if args.fallback_why == "all" or reason is None:
                    reason = "fallback"
            if reason is None:
                v = s
                why["reused"] += 1
                why_ns["reused"] += g.ns
                saved += g.ns
                saved_def += g.defns
                restored += len(v.binds)
                if s.oracle2 != g.oracle2:
                    unsafe2 += 1
                if g.server & SV_UNDEFINED_END:
                    need_fallback = True
            else:
                why[reason] += 1
                why_ns[reason] += g.ns
                nver[0] += 1
                v = Ver()
                v.id = nver[0]
                v.shape = g.shape
                v.uses = tuple((units[u].key, processed.get(units[u].key)) for u in g.uses)
                v.deps = tuple((units[u].key, processed.get(units[u].key)) for u in g.deps)
                v.mlist = g.ids
                v.prag = g.prag
                declared = frozenset(g.binds)
                v.declared = declared
                v.names = {nm: tuple(decls.get(nm, ())) for nm in declared | g.refs | tnames}
                v.binds = tuple(g.binds)
                v.oracle = g.oracle
                v.oracle2 = g.oracle2
                if key not in changes:
                    changes[key] = s
                store[key] = v
            if trace and trace == (p, b["i"]):
                print(p, b["i"], attempt, g.key, len(g.shape), reason or "reused", g.ns, detail if detail else "")
            processed[key] = v.id
            if args.mark_members == "yes" and not occ and len(g.shape) > 1:
                for mk in g.shape[1:]:
                    processed.setdefault(mk, v.id)
            for nm in v.binds:
                decls[nm].append((key, v.id))
        res = {"i": b["i"], "why": dict(why), "why_ns": {k: x for k, x in why_ns.items() if x},
               "saved_ns": saved, "saved_def_ns": saved_def, "restored": restored, "unsafe2": unsafe2}
        return res, need_fallback, changes

    for p in range(phases):
        res = []
        for bi, b in enumerate(builds):
            r, need, changes = run(p, bi, b, True, 0)
            if fb and need:
                for k, old in changes.items():
                    if old is None:
                        store.pop(k, None)
                    else:
                        store[k] = old
                r, _, _ = run(p, bi, b, False, 1)
            res.append(r)
        passes.append(res)
    return passes


def main():
    builds = load(args.records, args.timing)
    print(len(builds), "compiles", file=sys.stderr)
    trace = None
    if args.trace:
        tv, tp, ti = args.trace.rsplit(":", 2)
        trace = (tv, int(tp), int(ti))
    out = {}
    cache = {}
    for name in args.variants.split(","):
        words = name.split()
        vname = words[0]
        mods = set(words[1:])
        assert mods <= {"old", "fb", "occ", "tok"}, name
        observer = "old" if "old" in mods else "fixed"
        gobs = observer if args.group_observer == "run" else "fixed"
        occ = "occ" in mods
        ck = (vname, gobs, occ)
        if ck not in cache:
            units_of = []
            for b in builds:
                if vname == "S2":
                    units = make_units([[x] for x in b["xs"]])
                elif vname == "S3":
                    units = make_units([[y] for y in b["xs3"]])
                elif vname == "S2G":
                    units = groups_mine(b["xs"], False, gobs)
                elif vname == "S3G":
                    units = groups_mine(b["xs3"], False, gobs)
                elif vname == "S3G*":
                    units = groups_mine(b["xs3"], True, gobs)
                else:
                    raise SystemExit("unknown variant " + name)
                if occ:
                    number_occ(units)
                oracles(b, units)
                units_of.append(units)
            cache.clear()
            cache[ck] = units_of
        units_of = cache[ck]
        t = (trace[1], trace[2]) if trace and trace[0] == name else None
        out[name] = simulate(builds, units_of, occ, observer, "fb" in mods, "tok" in mods, args.phases, t)
        tot = collections.Counter()
        for r in out[name][1]:
            tot.update(r["why"])
        print(name, dict(tot), "unsafe2", sum(r["unsafe2"] for r in out[name][1]),
              sum(r["saved_ns"] for r in out[name][1]) / 1e9, file=sys.stderr, flush=True)
    json.dump(out, open(args.out, "w"))


main()
