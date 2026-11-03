"""Offline merges (user 2026-11-02): "offline merge doesn't have write time,
so anything two hits up should be merged". Between two builds, each tree the
record saw hit two or more times becomes one copy in the daemon's region,
and the next build's hits on that tree load one copy in place of its links.

A merge is the compile's own (HeaderCacheServer's mergeHere), in a compile
of the record run again: one that hit the tree where its .c file includes
the root, with CLANG_HEADER_CACHE_MERGE=decided and decisions that name the
trees ("merge <tree>"). At that hit the compile reads the root as text, the
copies inside it served, and writes the one copy, which the daemon then
serves for the tree. A compile merges one tree at a time, so a tree it
could not merge goes to the next compile that hit it, ROUNDS times at most.
These compiles run with CLANG_HEADER_CACHE_OFFLINE=1: their lookups do not
stop the offline step, and they record nothing. Each runs as -fsyntax-only,
its output files left out."""
import collections, os, re, subprocess, tempfile

ROUNDS = 3
MERGED = re.compile(r"header cache merge: \S+ sent \S+ combo=([0-9A-F]+)")
# -cc1 options that name an output file, with the file as the next argument,
# and those that only change what is written; and the actions.
OUTPUT_FILE = {"-o", "-dependency-file", "-MT", "-MQ", "-serialize-diagnostic-file",
               "-header-include-file", "-split-dwarf-output"}
OUTPUT_FLAG = {"-sys-header-deps", "-MP", "-module-file-deps"}
ACTIONS = {"-emit-obj", "-emit-llvm", "-emit-llvm-bc", "-emit-llvm-only", "-emit-codegen-only",
           "-S", "-E", "-fsyntax-only", "-emit-pch", "-emit-module"}


def decide(build):
    """The trees to merge: two or more hits, and a compile to merge it in."""
    return {name for name, t in build.trees.items()
            if t.hits >= 2 and any(c in build.commands for c in t.top_compiles)}


def syntax_only(args):
    """\\p args ("-cc1" first) as -fsyntax-only, with no output file."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a in OUTPUT_FILE:
            skip = True
        elif a not in OUTPUT_FLAG and a not in ACTIONS and not a.startswith(("-ftime-trace", "-stats-file=")):
            out.append(a)
    return out[:1] + ["-fsyntax-only"] + out[1:]


def replay(build, compile_index, trees):
    """Runs \\p compile_index again to merge \\p trees; the trees it merged."""
    cwd, program, args = build.commands[compile_index]
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLANG_HEADER_CACHE")}
    env.update(build.envs.get(compile_index, {}))
    env.pop("CLANG_HEADER_CACHE_RECORD", None)
    with tempfile.NamedTemporaryFile("w", suffix=".decisions", delete=False) as f:
        for t in sorted(trees):
            f.write(f"merge\t{t}\n")
    env.update(CLANG_HEADER_CACHE_DECISIONS=f.name, CLANG_HEADER_CACHE_MERGE="decided",
               CLANG_HEADER_CACHE_OFFLINE="1")
    try:
        r = subprocess.run([program] + syntax_only(args), cwd=cwd, env=env,
                           capture_output=True, text=True, timeout=600)
        return set(MERGED.findall(r.stderr)) & set(trees)
    except (OSError, subprocess.TimeoutExpired):
        return set()
    finally:
        os.unlink(f.name)


def run(build, trees):
    """Merges \\p trees; returns the trees merged and the compiles run."""
    todo, merged, runs = set(trees), set(), 0
    tried = collections.Counter()   # tree -> compiles tried for it
    for _ in range(ROUNDS):
        plan = collections.defaultdict(list)
        for name in sorted(todo):
            cs = [c for c in build.trees[name].top_compiles if c in build.commands]
            if tried[name] < len(cs):
                plan[cs[tried[name]]].append(name)
                tried[name] += 1
        if not plan:
            break
        for c in sorted(plan):
            got = replay(build, c, plan[c])
            runs += 1
            merged |= got
            todo -= got
    return merged, runs
