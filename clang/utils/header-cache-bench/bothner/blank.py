"""Checks the time model (COMPILE_SERVER_PLAN.md 3.4, step D6): what the model
charges for fragments against what removing them saves when measured.

For each chosen compile, the locations it can lose without changing anything
else: every instance there is reused under the headline variant (simulate.py
--reused), and no fragment of the compile and not its main file uses or
redeclares anything declared there (BothnerTrace's uses, deps, and flag 8).
Their text is blanked in copies of their headers, from the first token to the
end of the last, newlines kept, so every line keeps its number. The compile
then runs through a VFS overlay that maps each header to its blanked copy,
and through one that maps it to an unchanged copy, so that the overlay costs
the same on both sides; REPS times each, interleaved, the best of each,
pinned to CORE. The time removing them saved is set against what the model
charged them (the timing run's per-fragment times).

The pooled ratio of charged to saved time comes with a 95% bootstrap interval
over the compiles. Where the interval's low end is below 1, the model may
undercharge by that much: simulate.py's SAVED_SCALE = 1 / low end makes the
bound strict. (One compile's ratio is mostly wall-time noise: scaling by the
worst one drives some compiles' times below zero.)

usage: blank.py <record dir> <timing dir> <reused json> <workdir> [source names...]
env: REPS (default 11), CORE (default 3)
"""
import json, os, random, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from compiles import ENV, commands, front_end

rec_dir, time_dir, reused_json, work = sys.argv[1:5]
work = os.path.abspath(work)
PICK = sys.argv[5:] or ["server.c", "networking.c", "module.c", "cluster_legacy.c", "redis-cli.c",
                        "t_string.c", "db.c", "object.c", "util.c", "config.c",
                        "sds.c", "adlist.c", "lapi.c", "hiredis.c", "linenoise.c"]
REPS = int(os.environ.get("REPS", "11"))
CORE = os.environ.get("CORE", "3")
reused_all = json.load(open(reused_json))

PUNCT = sorted(["%:%:", "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "<:", ":>", "<%", "%>", "%:"],
               key=len, reverse=True)
LITERAL = re.compile(r"(u8|u|U|L)?([\"'])")
IDENT = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*")
NUMBER = re.compile(r"\.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.'])*")


def token_end(t, p):
    """The end of the C token starting at offset p of text t."""
    m = LITERAL.match(t, p)
    if m:
        q = m.end()
        while q < len(t) and t[q] != m.group(2) and t[q] != "\n":
            q += 2 if t[q] == "\\" else 1
        return min(q + 1, len(t))
    for rx in (IDENT, NUMBER):
        m = rx.match(t, p)
        if m:
            return m.end()
    for op in PUNCT:
        if t.startswith(op, p):
            return p + len(op)
    return p + 1


def compile_record(i):
    r = json.load(open(os.path.join(rec_dir, f"{i:03d}.json")))
    t = json.load(open(os.path.join(time_dir, f"{i:03d}.json")))
    return r["data"], t["data"]


def wall(argv, cwd):
    s = time.perf_counter()
    r = subprocess.run(["taskset", "-c", CORE] + argv, cwd=cwd, env=ENV, capture_output=True, text=True)
    return time.perf_counter() - s, r


cmds = commands()
chosen = []
for name in PICK:  # the first compile of that name, one of redis-server's if there is one
    hits = [i for i, (cwd, _, src) in enumerate(cmds) if os.path.basename(src) == name]
    srv = [i for i in hits if cmds[i][0].endswith("/redis/src")]
    if hits:
        chosen.append((srv or hits)[0])

os.makedirs(work, exist_ok=True)
rows = []
print(f"{'source':18} {'blanked':>8} {'of':>6} {'charged ms':>11} {'saved ms':>9} {'ratio':>6}   {'control ms':>10} {'blanked ms':>10}")
for i in chosen:
    cwd, args, src = cmds[i]
    rd, td = compile_record(i)
    entries = rd["entries"]
    frags = rd["frags"]
    real = lambda f: entries[f[0]][1] or os.path.normpath(os.path.join(cwd, entries[f[0]][0]))
    # A location here is a file and an offset: names a header was opened by
    # all lead to the same text.
    loc = [(real(f), f[1]) for f in frags]
    reused_keys = {(n, s, o) for n, s, o in reused_all.get(str(i), [])}
    occ, is_reused = {}, []
    for f in sorted(range(len(frags)), key=lambda k: frags[k][10]):
        key = (entries[frags[f][0]][0], frags[f][1])
        occ[key] = occ.get(key, 0) + 1
        is_reused.append((f, (key[0], key[1], occ[key]) in reused_keys))
    reused = dict(is_reused)
    referenced = set()
    for f in frags:
        for u in f[14] + f[16]:
            referenced.add(loc[u])
    for k, f in enumerate(frags):
        if f[13] & 8:
            referenced.add(loc[k])
    by_loc = {}
    for k in range(len(frags)):
        by_loc.setdefault(loc[k], []).append(k)
    eligible = {l for l, ks in by_loc.items() if l not in referenced and all(reused[k] for k in ks)}
    charged = sum(td["frags"][k][5] for k in range(len(frags)) if loc[k] in eligible)

    # Copies of the headers concerned: blanked, and unchanged.
    d = os.path.join(work, f"{i:03d}")
    files = {}
    for (path, start) in eligible:
        files.setdefault(path, []).append(start)
    names = {}  # every name the compile may look a header up by -> its real path
    for e in entries:
        p = e[1] or os.path.normpath(os.path.join(cwd, e[0]))
        names[os.path.normpath(os.path.join(cwd, e[0]))] = p
        names[p] = p
    overlays = {}
    for kind in ("control", "blanked"):
        roots = []
        for n, p in sorted(names.items()):
            if p not in files:
                continue
            text = open(p, encoding="latin-1").read()
            if kind == "blanked":
                chars = list(text)
                ends = {k: frags[k][2] for k in range(len(frags)) if loc[k][0] == p}
                for start in files[p]:
                    end = token_end(text, next(e for k, e in ends.items() if frags[k][1] == start))
                    for q in range(start, end):
                        if chars[q] != "\n":
                            chars[q] = " "
                text = "".join(chars)
            copy = os.path.join(d, kind, p.lstrip("/"))
            os.makedirs(os.path.dirname(copy), exist_ok=True)
            open(copy, "w", encoding="latin-1").write(text)
            roots.append({"type": "file", "name": n, "external-contents": copy})
        overlays[kind] = os.path.join(d, kind + ".json")
        json.dump({"version": 0, "use-external-names": False, "roots": roots}, open(overlays[kind], "w"))

    argv = {k: front_end(cwd, args, src, ["-ivfsoverlay", overlays[k]]) for k in overlays}
    bad = [k for k in argv if wall(argv[k], cwd)[1].returncode]
    if bad:
        print(f"{src:18} the {', '.join(bad)} compile fails; skipped")
        continue
    best = {k: float("inf") for k in argv}
    for rep in range(REPS):
        for k in (("control", "blanked") if rep % 2 == 0 else ("blanked", "control")):
            best[k] = min(best[k], wall(argv[k], cwd)[0])
    saved = (best["control"] - best["blanked"]) * 1e3
    row = {"i": i, "src": src, "locations": len(eligible), "of": len(by_loc), "charged_ms": charged / 1e6,
           "saved_ms": saved, "control_ms": best["control"] * 1e3, "blanked_ms": best["blanked"] * 1e3}
    rows.append(row)
    print(f"{src:18} {len(eligible):8} {len(by_loc):6} {charged / 1e6:11.2f} {saved:9.2f} {charged / 1e6 / saved:6.3f}"
          f"   {row['control_ms']:10.2f} {row['blanked_ms']:10.2f}", flush=True)

C = sum(r["charged_ms"] for r in rows)
S = sum(r["saved_ms"] for r in rows)
ratios = sorted(r["charged_ms"] / r["saved_ms"] for r in rows)
rng = random.Random(1)
boots = sorted(sum(r["charged_ms"] for r in x) / sum(r["saved_ms"] for r in x)
               for x in ([rows[rng.randrange(len(rows))] for _ in rows] for _ in range(10000)))
low, high = boots[249], boots[9749]
scale = max(1.0, 1 / low)
print(f"\n{len(rows)} compiles: charged {C:.1f} ms, saved {S:.1f} ms when measured: the model charges "
      f"{(C / S - 1) * 100:+.1f}% of the measured saving, 95% interval {(low - 1) * 100:+.1f}% to "
      f"{(high - 1) * 100:+.1f}% (per compile: median {(ratios[len(ratios) // 2] - 1) * 100:+.1f}%, "
      f"from {(ratios[0] - 1) * 100:+.1f}% to {(ratios[-1] - 1) * 100:+.1f}%); strict bound: SAVED_SCALE={scale:.3f}")
json.dump({"rows": rows, "charged_ms": C, "saved_ms": S, "interval": [low, high], "saved_scale": scale},
          open(os.path.join(work, "blank.json"), "w"))
