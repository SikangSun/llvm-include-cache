"""Step S1 of COMPILE_SERVER_DESIGN.md: the server with no reuse, checked
against plain clang.

Every recorded compile runs first as plain clang, emitting LLVM IR
(-S -emit-llvm -O0 -disable-llvm-passes) with BothnerServer.so's bothner-dump
action, which writes the declaration summary of design section 5. Then one
server process serves PASSES passes of all compiles in log order, each compile
its own request, and writes the same two outputs. For every compile and pass
the IR and the summary must be byte-identical to plain clang's. The report
also gives, per pass, the server's time per compile beside plain clang's, its
memory, and the source-location address space used (2 GiB at most).

With TIMING=1 the compiles run -fsyntax-only instead, plain and through the
server, pinned to CORE, and nothing is compared. With REUSE=1 the server
keeps and reuses header fragments (steps S2-S4), and the report adds its
decisions per pass.

usage: server_s1.py <workdir>
env: BIN (default build_assert/bin of this checkout), SO (the plugin built
     against BIN; default BothnerServer.so here), LOGS, BENCH_ROOT,
     SRC_ROOT (as compiles.py), PASSES (default 2), FIRST, COUNT, CORE,
     TIMING, REUSE, OPTION (3: #define and #undef lines join fragments),
     WARN (flags added to every compile, such as -Weverything: the server's
     diagnostics must then equal plain clang's, section 5),
     FRAGMENT_GROUPS (1: conditionals join fragments while the parser is
     nested)
"""
import json, os, resource, shlex, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "../../../.."))
os.environ.setdefault("BIN", os.path.join(ROOT, "build_assert/bin"))
sys.path.insert(0, HERE)
from compiles import CLANG, ENV, commands, config  # noqa: E402
import diagtext  # noqa: E402

SO = os.environ.get("SO", os.path.join(HERE, "BothnerServer.so"))
PASSES = int(os.environ.get("PASSES", "2"))
TIMING = os.environ.get("TIMING") == "1"
REUSE = os.environ.get("REUSE") == "1"
CORE = os.environ.get("CORE")
WARN = shlex.split(os.environ.get("WARN", ""))
PIN = ["taskset", "-c", CORE] if CORE else []
ENV_PLAIN = {k: v for k, v in ENV.items() if k != "BOTHNER_DUMP_OUT"}


def driver_args(args, src, out):
    """The driver command of one compile: plain clang's flags (no modules
    flags), IR at -O0 with no LLVM passes, or -fsyntax-only when timing."""
    if TIMING:
        return [CLANG, "-fsyntax-only"] + config(args, src) + WARN + [src]
    return ([CLANG, "-S", "-emit-llvm"] + config(args, src) + WARN +
            ["-O0", "-Xclang", "-disable-llvm-passes", src, "-o", out])


def cc1_args(cwd, argv):
    """The arguments after -cc1 that the driver would run in process."""
    r = subprocess.run(argv + ["-###"], cwd=cwd, env=ENV_PLAIN, capture_output=True, text=True)
    for line in r.stderr.splitlines():
        parts = shlex.split(line)
        if len(parts) > 1 and parts[1] == "-cc1":
            return parts[2:]
    raise RuntimeError("no -cc1 job: " + " ".join(argv) + "\n" + r.stderr[-2000:])


def with_output(args, out):
    a = list(args)
    a[a.index("-o") + 1] = out
    return a


def main():
    work = os.path.abspath(sys.argv[1])
    os.makedirs(work, exist_ok=True)
    cmds = commands()
    first = int(os.environ.get("FIRST", "0"))
    count = int(os.environ.get("COUNT", str(len(cmds))))
    todo = list(range(first, min(len(cmds), first + count)))

    # Plain clang: the reference outputs and times.
    plain_ms, plain_cpu, cc1, plain_diags = {}, {}, {}, {}
    for i in todo:
        cwd, args, src = cmds[i]
        ref = os.path.join(work, f"ref-{i:03d}")
        argv = driver_args(args, src, ref + ".ll")
        cc1[i] = cc1_args(cwd, argv)
        if not TIMING:
            argv = argv + ["-fplugin=" + SO]
        env = dict(ENV_PLAIN, BOTHNER_DUMP_OUT=ref + ".decls") if not TIMING else ENV_PLAIN
        u = resource.getrusage(resource.RUSAGE_CHILDREN)
        s = time.perf_counter()
        r = subprocess.run(PIN + argv, cwd=cwd, env=env, capture_output=True, text=True)
        plain_ms[i] = (time.perf_counter() - s) * 1e3
        v = resource.getrusage(resource.RUSAGE_CHILDREN)
        plain_cpu[i] = (v.ru_utime + v.ru_stime - u.ru_utime - u.ru_stime) * 1e3
        plain_diags[i] = diagtext.cc1_text(r.stderr)
        if r.returncode:
            print(f"plain {i} failed: {r.stderr[-500:]}")

    # The server: PASSES passes of all compiles, in one process.
    req = os.path.join(work, "requests.jsonl")
    res = os.path.join(work, "results.jsonl")
    with open(req, "w") as f:
        for p in range(PASSES):
            for i in todo:
                out = os.path.join(work, f"p{p}-{i:03d}")
                r = {"cwd": cmds[i][0], "args": cc1[i] if TIMING else with_output(cc1[i], out + ".ll")}
                if not TIMING:
                    r["dump"] = out + ".decls"
                f.write(json.dumps(r) + "\n")
    argv = PIN + [CLANG, "-cc1", "-load", SO, "-plugin", "bothner-server",
                  "-plugin-arg-bothner-server", "requests=" + req,
                  "-plugin-arg-bothner-server", "results=" + res] + \
        (["-plugin-arg-bothner-server", "reuse=1"] if REUSE else []) + \
        (["-plugin-arg-bothner-server", "option=3"] if os.environ.get("OPTION") == "3" else []) + \
        (["-plugin-arg-bothner-server", "groups=1"] if os.environ.get("FRAGMENT_GROUPS") == "1" else []) + ["-x", "c", "/dev/null"]
    s = time.perf_counter()
    r = subprocess.run(argv, env=ENV_PLAIN, capture_output=True, text=True)
    server_s = time.perf_counter() - s
    results = [json.loads(l) for l in open(res)] if os.path.exists(res) else []
    print(f"server exit {r.returncode}, {len(results)} of {PASSES * len(todo)} requests answered, {server_s:.1f} s")
    if r.returncode:
        print(r.stderr[-3000:])

    # Per pass: outputs equal to plain clang's, times, memory, address space.
    for p in range(PASSES):
        rows = results[p * len(todo):(p + 1) * len(todo)]
        same_ir = same_decls = same_diags = known_diags = failed = 0
        diffs = []
        for i, row in zip(todo, rows):
            if row.get("status"):
                failed += 1
                diffs.append(f"{i}: status {row.get('status')} {row.get('error', '')} {row.get('diags', '')[:300]}")
                continue
            if TIMING:
                continue
            how = diagtext.compare(plain_diags[i], diagtext.server_text(row))
            d = os.path.join(work, f"p{p}-{i:03d}.diags")
            if how == "same":
                same_diags += 1
                if os.path.exists(d):
                    os.remove(d)
            else:
                known_diags += how != "differ"
                open(d, "w").write(diagtext.server_text(row))
                open(os.path.join(work, f"ref-{i:03d}.diags"), "w").write(plain_diags[i])
                if how == "differ":
                    diffs.append(f"{i}.diags: differs (diff {os.path.join(work, f'ref-{i:03d}.diags')} {d})")
            for ext in (".ll", ".decls"):
                a = os.path.join(work, f"ref-{i:03d}{ext}")
                b = os.path.join(work, f"p{p}-{i:03d}{ext}")
                ta = open(a).read() if os.path.exists(a) else None
                tb = open(b).read() if os.path.exists(b) else None
                if ta is not None and ta == tb:
                    if ext == ".ll":
                        same_ir += 1
                    else:
                        same_decls += 1
                else:
                    diffs.append(f"{i}{ext}: differs ({a} {b})")
        n = len(rows)
        wall = sum(r.get("wall_ms", 0) for r in rows)
        plain = sum(plain_ms[i] for i in todo)
        cpu = sum(r.get("cpu_ms", 0) for r in rows)
        plain_c = sum(plain_cpu[i] for i in todo)
        last = rows[-1] if rows else {}
        print(f"pass {p}: {n} compiles, failed {failed}" +
              ("" if TIMING else f", IR same {same_ir}, declarations same {same_decls},"
                                f" diagnostics same {same_diags} (+{known_diags} with the known differences only)"))
        print(f"  time per compile: server {wall / max(n, 1):.1f} ms (reset {sum(r.get('reset_ms', 0) for r in rows) / max(n, 1):.2f},"
              f" setup {sum(r.get('setup_ms', 0) for r in rows) / max(n, 1):.2f}, parse {sum(r.get('parse_ms', 0) for r in rows) / max(n, 1):.1f}),"
              f" plain process {plain / max(len(todo), 1):.1f} ms")
        print(f"  CPU per compile: server {cpu / max(n, 1):.1f} ms, plain process {plain_c / max(len(todo), 1):.1f} ms"
              f" (server / plain {cpu / max(plain_c, 1e-9):.3f})")
        print(f"  after the pass: RSS {last.get('rss_kib', 0) / 1024:.0f} MiB, address space {last.get('sloc_bytes', 0) / 2**20:.0f} MiB,"
              f" identifiers {last.get('idents', 0)}, configurations {1 + max([r.get('config', 0) for r in rows] or [0])}")
        if REUSE:
            why = {}
            for r in rows:
                for k, v in r.get("decisions", {}).items():
                    why[k] = why.get(k, 0) + v
            tot = sum(why.values())
            print(f"  fragments: {tot}, reused {why.get('reused', 0)} ({100 * why.get('reused', 0) / max(tot, 1):.1f}%),"
                  f" empty {why.get('empty', 0)}, restored declarations {sum(r.get('restored_decls', 0) for r in rows)}")
            print("  parsed because: " + ", ".join(f"{k} {v}" for k, v in sorted(why.items(), key=lambda x: -x[1])
                                                   if k not in ("reused", "empty")))
        for d in diffs[:10]:
            print("  " + d)
    json.dump({"plain_ms": plain_ms, "plain_cpu_ms": plain_cpu, "server_s": server_s},
              open(os.path.join(work, "summary.json"), "w"))


if __name__ == "__main__":
    main()
