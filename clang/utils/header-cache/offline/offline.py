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

usage: offline.py RECORD_DIR DECISIONS_FILE

Prints one JSON line: the decisions, their simulated saving, the merges,
and the time each step took, in ms."""
import json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import costs, folds, merges, records


def main():
    record_dir, out = sys.argv[1:3]
    ms, last = {}, [time.monotonic()]

    def lap(step):
        now = time.monotonic()
        ms[step] = round((now - last[0]) * 1e3)
        last[0] = now

    lines = records.compiles(record_dir)
    lap("read")
    build = records.load(lines)
    c = costs.Costs(lines)
    lap("fit")
    graph = folds.Graph(build, c)
    fold, top = graph.decide()
    lap("folds")
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
        compiles=len(lines), pairs=len(graph.edges), folds=len(fold), outer=len(graph.outer),
        tops=len(top), saving_s=dict(folds=round(sum(fold.values()) / 1e6, 3),
                                     tops=round(sum(top.values()) / 1e6, 3)),
        trees=len(build.trees), trees_to_merge=len(trees), trees_merged=len(merged),
        merge_compiles=replays, c_link_us=round(c.c_link, 1), ms=ms,
        total_ms=sum(ms.values()))), flush=True)


if __name__ == "__main__":
    main()
