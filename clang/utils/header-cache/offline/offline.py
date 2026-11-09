#!/usr/bin/env python3
"""The offline step between two builds (user 2026-11-02): from one build's
record (CLANG_HEADER_CACHE_RECORD), the decisions for the next build, then
the merges, in the daemon's region:

  top h             h is not asked for where a .c file includes it
  fold P h          h is not asked for inside a copy of P being written

and every tree hit two or more times merged into one copy (merges.py).
clang-header-cache-daemon -offline runs it once a build's link is done; it
also runs by hand, with the daemon running. The decisions file
(CLANG_HEADER_CACHE_DECISIONS) is written last and replaced whole: a step
stopped part way leaves none, and a compile reads the old decisions or the
new ones, never a part.

usage: offline.py [--no-folds] [--no-merges] RECORD_DIR DECISIONS_FILE

  --no-folds    folding off: no top and no fold lines (the top lines come
                from the same rule, after the folds), and no cost fit
  --no-merges   merging off: no tree is merged

With both, the decisions file is still written, empty: the next build
takes no decisions and records nothing, as with the step on. The daemon
appends the two paths to its -offline command, so the switches go in that
command: -offline "python3 -I offline.py --no-merges".

Prints one JSON line: the decisions, their simulated saving, the merges,
and the time each step took, in ms."""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import costs, folds, merges, records


def main():
    p = argparse.ArgumentParser(description="The offline step between two builds.")
    p.add_argument("--no-folds", action="store_true", help="folding off: write no top and no fold lines")
    p.add_argument("--no-merges", action="store_true", help="merging off: merge no tree")
    p.add_argument("record_dir")
    p.add_argument("decisions_file")
    a = p.parse_args()
    record_dir, out = a.record_dir, a.decisions_file
    ms, last = {}, [time.monotonic()]

    def lap(step):
        now = time.monotonic()
        ms[step] = round((now - last[0]) * 1e3)
        last[0] = now

    lines = records.compiles(record_dir)
    lap("read")
    build = records.load(lines)
    fold, top, pairs, outer, c_link = {}, {}, 0, 0, None
    if not a.no_folds:
        c = costs.Costs(lines)
        lap("fit")
        graph = folds.Graph(build, c)
        fold, top = graph.decide()
        pairs, outer, c_link = len(graph.edges), len(graph.outer), round(c.c_link, 1)
        lap("folds")
    trees, merged, replays = set(), [], 0
    if not a.no_merges:
        trees = merges.decide(build)
        merged, replays = merges.run(build, trees)
        lap("merges")
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        for h in sorted(top):
            f.write(f"top\t{h}\n")
        for P, h in sorted(fold):
            f.write(f"fold\t{P}\t{h}\n")
    os.replace(tmp, out)
    lap("write")
    print(json.dumps(dict(
        folding=not a.no_folds, merging=not a.no_merges,
        compiles=len(lines), pairs=pairs, folds=len(fold), outer=outer,
        tops=len(top), saving_s=dict(folds=round(sum(fold.values()) / 1e6, 3),
                                     tops=round(sum(top.values()) / 1e6, 3)),
        trees=len(build.trees), trees_to_merge=len(trees), trees_merged=len(merged),
        merge_compiles=replays, c_link_us=c_link, ms=ms,
        total_ms=sum(ms.values()))), flush=True)


if __name__ == "__main__":
    main()
