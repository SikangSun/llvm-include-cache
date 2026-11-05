"""Source-location space and memory of the object check's servers, per
project: what one pass of the whole build would take in one server.

usage: server_space.py <server_objects.py workdir> [projects...]
Each chunk's server serves its compiles twice (res<chunk>.jsonl, two passes in
order). sloc_bytes is SourceManager::getNextLocalOffset() after a compile.
"""
import glob, json, os, sys

base = sys.argv[1]
projects = sys.argv[2:] or ["redis", "nginx", "curl", "git", "cpython", "postgres", "ffmpeg"]
LIMIT = 2 ** 31
print("| Project | Compiles | Space per compile (MiB) | One pass in one server (GiB) | Fill + same build again (GiB) | Fits 2 GiB | Largest RSS, 200 compiles, 2 passes (GiB) |")
print("|---|---|---|---|---|---|---|")
for proj in projects:
    files = sorted(glob.glob(os.path.join(base, proj, "res*.jsonl")))
    if not files or not os.path.exists(os.path.join(base, proj + ".json")):
        continue
    compiles = space = 0
    rss = 0.0
    for f in files:
        rows = [json.loads(l) for l in open(f)]
        n = len(rows) // 2
        assert n * 2 == len(rows), f
        compiles += n
        space += rows[n - 1]["sloc_bytes"]
        rss = max(rss, rows[-1]["rss_kib"] / 2 ** 20)
    one = space / 2 ** 30
    print(f"| {proj} | {compiles:,} | {space / compiles / 2 ** 20:.2f} | {one:.2f} | {2 * one:.2f} | "
          f"{'yes' if 2 * space < LIMIT else 'no'} | {rss:.2f} |")
