"""Pre-parsed headers (clang/lib/ASTCache/PPH_PLAN.md) on one recorded build.

  discover  Every compile once with -H: the headers it enters, by the name it
            opened each by, nested by who included whom.
  generate  Per configuration -- a compile's working directory and flags, its
            output and source files left out -- an image for each project
            header (CLANG_PPH_GEN), the headers it includes first, so that its
            image links theirs, as GCC's PPH images referred to their
            children's. A header is generated under each name a compile opened
            it by, from a wrapper in the working directory that includes it by
            that name, so that its __FILE__ and debug names are the compile's.
  replay    Every compile three ways -- plain clang, plain clang with the
            modules flags the images need, and with the images -- comparing the
            objects.

Each step writes its results to WORK and is skipped when they exist, so a run
the machine's crash cut short starts again where it was.

usage: pph_run.py <log-dir> <project-root> <work-dir> [discover] [generate] [replay]
env: BIN (clang's bin directory), JOBS (compiles at once, default 4),
     POLICY (CLANG_PPH_POLICY for the replay: unset, or "safe"; the replay's
     results are kept apart per policy).
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait

BIN = os.environ["BIN"]
CLANG = f"{BIN}/clang"
JOBS = int(os.environ.get("JOBS", "4"))
MOD = ["-fmodules", "-fno-implicit-module-maps"]
# Absolute: every compile runs in its own working directory.
LOGS, ROOT, WORK = (os.path.normpath(os.path.abspath(a)) for a in sys.argv[1:4])
STEPS = sys.argv[4:] or ["discover", "generate", "replay"]
# Names the replay's outputs: "pph" for PPH's own rule, "safe" for the safe
# policy (PPH_PLAN.md D3).
TAG = os.environ.get("POLICY") or "pph"
os.makedirs(WORK, exist_ok=True)
BASE = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_PPH"))}


def commands():
    """The recorded compiles of one .c file each, as modes_bench.py takes them."""
    cmds = []
    for name in sorted(os.listdir(LOGS), key=lambda n: int(n.split(".")[0])):
        parts = open(os.path.join(LOGS, name), "rb").read().decode(errors="replace").split("\0")[:-1]
        cwd, args = parts[0], parts[1:]
        if "-c" not in args or "-E" in args:
            continue
        srcs = [a for i, a in enumerate(args) if a.endswith(".c") and args[i - 1] not in ("-o", "-MF", "-MT", "-MQ")]
        if len(srcs) != 1 or "conftest" in srcs[0]:
            continue
        cmds.append((cwd, args, srcs[0]))
    return cmds


def config(args, src):
    """A compile's flags without its source, output and dependency-file flags."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif a in ("-c", "-S", "-MD", "-MMD", "-MP") or a == src or a.startswith("-Wp,-M"):
            pass
        else:
            out.append(a)
    return out


def key(cwd, cfg):
    return hashlib.sha1("\0".join([cwd] + cfg).encode()).hexdigest()[:16]


def absolute(cwd, name):
    """How the header cache names a header: absolute, dots removed, links kept."""
    return os.path.normpath(os.path.join(cwd, name))


def run(argv, cwd, env=None):
    return subprocess.run(argv, cwd=cwd, env=env or BASE, capture_output=True, text=True)


def save(name, value):
    tmp = os.path.join(WORK, name + ".tmp")
    with open(tmp, "w") as f:
        json.dump(value, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, os.path.join(WORK, name))


def load(name):
    p = os.path.join(WORK, name)
    return json.load(open(p)) if os.path.exists(p) else None


HLINE = re.compile(r"^(\.+) (.*)$")


def discover(cmds):
    def one(i):
        cwd, args, src = cmds[i]
        cfg = config(args, src)
        r = run([CLANG, "-fsyntax-only", "-H"] + MOD + cfg + [src], cwd)
        entries, unguarded, in_list = [], [], False
        for line in r.stderr.splitlines():
            if line.startswith("Multiple include guards may be useful for:"):
                in_list = True
            elif in_list:
                unguarded.append(absolute(cwd, line.strip()))
            elif m := HLINE.match(line):
                entries.append((len(m.group(1)), m.group(2)))
        return {"i": i, "cwd": cwd, "cfg": cfg, "key": key(cwd, cfg), "rc": r.returncode,
                "entries": entries, "unguarded": unguarded}
    with ThreadPoolExecutor(JOBS) as ex:
        found = list(ex.map(one, range(len(cmds))))
    bad = [d for d in found if d["rc"]]
    print(f"discover: {len(found)} compiles, {len({d['key'] for d in found})} configurations, "
          f"{len(bad)} failed to compile", flush=True)
    return found


def plan(found):
    """Per configuration: the project headers to generate, each under every
    name a compile opened it by, and, for each, the project headers entered
    below it, which are generated first."""
    tasks = {}
    for d in found:
        stack = []
        for depth, name in d["entries"]:
            path = absolute(d["cwd"], name)
            del stack[depth - 1:]
            node = (d["key"], path, name) if path.startswith(ROOT + os.sep) else None
            if node:
                t = tasks.setdefault(node, {"key": d["key"], "cwd": d["cwd"], "cfg": d["cfg"],
                                            "path": path, "name": name, "deps": set()})
                for above in stack:
                    if above:
                        tasks[above]["deps"].add(node)
            stack.append(node)
    return tasks


def generate(found):
    tasks = plan(found)
    img = os.path.join(WORK, "img")
    os.makedirs(img, exist_ok=True)
    results, done = {}, set()

    def one(node):
        t = tasks[node]
        # The name a compile opens a header by is its includer's directory,
        # as the compile names it, joined to the spelling: ./fmacros.h,
        # ../deps/hiredis/hiredis.h. A wrapper named in that directory the
        # same way, including the header's base name, opens it by that name.
        d, base = os.path.split(t["name"])
        wrapper = os.path.join(d or ".", f".pph_gen_{hashlib.sha1(repr(node).encode()).hexdigest()[:12]}.c")
        with open(os.path.join(t["cwd"], wrapper), "w") as f:
            f.write(f'#include "{base}"\n')
        env = dict(BASE, CLANG_PPH_DIR=os.path.join(img, t["key"]), CLANG_PPH_GEN=t["path"])
        r = run([CLANG, "-fsyntax-only"] + MOD + t["cfg"] + [wrapper], t["cwd"], env)
        os.remove(os.path.join(t["cwd"], wrapper))
        refused = [l.split(": no image: ", 1)[1] for l in r.stderr.splitlines() if ": no image: " in l]
        why = refused[0] if refused else ("error" if r.returncode else None)
        return node, {"path": t["path"], "name": t["name"], "key": t["key"], "rc": r.returncode,
                      "refused": why, "stderr": r.stderr[-2000:] if r.returncode else ""}

    pending = set(tasks)
    with ThreadPoolExecutor(JOBS) as ex:
        running = {}
        while pending or running:
            # A header whose children are done (generated or not) is ready.
            ready = [n for n in pending if tasks[n]["deps"] <= done | {n}]
            for n in ready[:max(0, JOBS * 2 - len(running))]:
                pending.discard(n)
                running[ex.submit(one, n)] = n
            if not running:
                # What is left includes itself, through other headers: as GCC
                # did with circular includes, those are read as text.
                for n in pending:
                    results[n] = {"path": tasks[n]["path"], "name": tasks[n]["name"], "key": tasks[n]["key"],
                                  "rc": None, "refused": "an include cycle", "stderr": ""}
                break
            fin, _ = wait(running, return_when=FIRST_COMPLETED)
            for f in fin:
                node, res = f.result()
                del running[f]
                results[node] = res
                done.add(node)
    out = list(results.values())
    images = sum(len([x for x in os.listdir(os.path.join(img, k, h)) if x.endswith(".pph")])
                 for k in os.listdir(img) for h in os.listdir(os.path.join(img, k)))
    reasons = {}
    for r in out:
        if r["refused"]:
            reasons[r["refused"]] = reasons.get(r["refused"], 0) + 1
    print(f"generate: {len(out)} header names in {len({r['key'] for r in out})} configurations; "
          f"{images} images written; refused: {reasons}", flush=True)
    return out


def replay(cmds, found):
    policy = os.environ.get("POLICY", "")
    objs = os.path.join(WORK, "obj")
    logs = os.path.join(WORK, f"log-{TAG}")
    os.makedirs(logs, exist_ok=True)

    def one(i):
        cwd, args, src = cmds[i]
        k = found[i]["key"]
        res = {"i": i, "src": src}
        for mode in ("plain0", "plainM", "pph"):
            o = os.path.join(objs, TAG if mode == "pph" else mode, f"{i}.o")
            os.makedirs(os.path.dirname(o), exist_ok=True)
            a = list(args)
            if "-o" in a:
                a[a.index("-o") + 1] = o
            else:
                a += ["-o", o]
            env = BASE
            if mode != "plain0":
                a = MOD + a
            if mode == "pph":
                env = dict(BASE, CLANG_PPH_DIR=os.path.join(WORK, "img", k), CLANG_HEADER_CACHE_STATS="2",
                           **({"CLANG_PPH_POLICY": policy} if policy else {}))
            r = run([CLANG] + a, cwd, env)
            res[mode + "_rc"] = r.returncode
            if mode == "pph":
                open(os.path.join(logs, f"{i}.txt"), "w").write(r.stderr)
                m = re.search(r"header cache: (\d+) hits / (\d+) lookups", r.stderr)
                res["hits"], res["lookups"] = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        same = lambda x, y: (os.path.exists(x) and os.path.exists(y) and open(x, "rb").read() == open(y, "rb").read())
        o = lambda m: os.path.join(objs, m, f"{i}.o")
        res["pph_vs_plainM"] = same(o(TAG), o("plainM"))
        res["pph_vs_plain0"] = same(o(TAG), o("plain0"))
        res["plainM_vs_plain0"] = same(o("plainM"), o("plain0"))
        return res

    with ThreadPoolExecutor(JOBS) as ex:
        out = list(ex.map(one, range(len(cmds))))
    failed = [r["src"] for r in out if r["pph_rc"]]
    differ = [r["src"] for r in out if not r["pph_rc"] and not r["pph_vs_plainM"]]
    print(f"replay ({TAG}): {len(out)} compiles; {sum(r['hits'] for r in out)} hits / {sum(r['lookups'] for r in out)} lookups; "
          f"objects identical to plainM: {sum(r['pph_vs_plainM'] for r in out)}, to plain0: "
          f"{sum(r['pph_vs_plain0'] for r in out)} (plainM = plain0 for {sum(r['plainM_vs_plain0'] for r in out)}); "
          f"failed: {len(failed)}; differ: {len(differ)}", flush=True)
    for s in failed[:10]:
        print("  failed:", s)
    for s in differ[:10]:
        print("  differs:", s)
    return out


cmds = commands()
print(f"{len(cmds)} recorded compiles", flush=True)
found = load("discover.json")
if "discover" in STEPS and found is None:
    found = discover(cmds)
    save("discover.json", found)
if "generate" in STEPS and load("generate.json") is None:
    if os.path.exists(os.path.join(WORK, "img")):
        shutil.rmtree(os.path.join(WORK, "img"))
    save("generate.json", generate(found))
if "replay" in STEPS:
    save(f"replay-{TAG}.json", replay(cmds, found))
