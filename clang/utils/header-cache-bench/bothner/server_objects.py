"""The header cache's correctness check (artifact/check.sh), for the compile
server: every recorded compile builds its object with the build's own flags,
as plain clang and through the server with reuse on, and the objects are
compared byte for byte. The diagnostics are compared as text (diagtext.py);
the two known differences of section 6 are counted apart. The server serves PASSES passes of the compiles in log
order (default 2): a first build, then the same build again, each pass its own
build ID. A project too large for one server's 2 GiB of source locations is
split into chunks of CHUNK compiles, each chunk its own server for all
passes. JOBS plain compiles and chunks run at once.

With VIA=driver each chunk's server listens on a socket, and every compile
runs as the build ran it, through the clang driver with CLANG_COMPILE_SERVER
set (driver.cpp, step S6), one compile after another. A compile the driver
ran locally, because it could not reach the server, counts as failed.

usage: server_objects.py <workdir> <project>...
env: BIN (default build_assert/bin of this checkout), SO, LOGS_DIR (holding
     log-<project>), BENCH_ROOT, SRC_ROOT, PASSES, CHUNK, JOBS, KEEP (keep
     the objects), VIA (requests or driver), OPTION (2, the default, or 3:
     #define and #undef lines join fragments, design 2.7), FRAGMENT_GROUPS
     (1: conditionals join fragments while the parser is nested), KEYS
     (reading: one key for each reading of a location, decision 8)
"""
import concurrent.futures as cf, json, os, shlex, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "../../../.."))
os.environ.setdefault("BIN", os.path.join(ROOT, "build_assert/bin"))
LOGS_DIR = os.environ.get("LOGS_DIR", "/home/sikang/c/code/scratch/e2e/multi")
SO = os.environ.get("SO", os.path.join(HERE, "BothnerServer.so"))
PASSES = int(os.environ.get("PASSES", "2"))
CHUNK = int(os.environ.get("CHUNK", "0"))
JOBS = int(os.environ.get("JOBS", "16"))
VIA = os.environ.get("VIA", "requests")
OPTION_ARGS = (["-plugin-arg-bothner-server", "option=3"] if os.environ.get("OPTION") == "3" else []) + \
    (["-plugin-arg-bothner-server", "groups=1"] if os.environ.get("FRAGMENT_GROUPS") == "1" else []) + \
    (["-plugin-arg-bothner-server", "keys=reading"] if os.environ.get("KEYS") == "reading" else [])
sys.path.insert(0, HERE)
import diagtext  # noqa: E402


def cc1_args(cwd, argv, env):
    r = subprocess.run(argv + ["-###"], cwd=cwd, env=env, capture_output=True, text=True)
    for line in r.stderr.splitlines():
        parts = shlex.split(line)
        if len(parts) > 1 and parts[1] == "-cc1":
            return parts[2:]
    raise RuntimeError("no -cc1 job: " + r.stderr[-1000:])


def run_project(work, proj):
    os.environ["LOGS"] = os.path.join(LOGS_DIR, "log-" + proj)
    import importlib, compiles
    importlib.reload(compiles)
    CLANG, env = compiles.CLANG, {k: v for k, v in compiles.ENV.items() if k != "BOTHNER_DUMP_OUT"}
    # __DATE__ and __TIME__ (cpython's getbuildinfo.c) from a fixed time, as
    # the driver passes it to cc1 (-source-date-epoch): plain and server
    # compiles run minutes apart.
    env["SOURCE_DATE_EPOCH"] = "1700000000"
    cmds = compiles.commands()
    d = os.path.join(work, proj)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    s0 = time.time()

    def plain(i):
        cwd, args, src = cmds[i]
        argv = [CLANG] + compiles.config(args, src) + ["-c", src, "-o", os.path.join(d, f"{i:04d}.plain.o")]
        r = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True)
        return i, r.returncode, r.stderr, cc1_args(cwd, argv, env)

    cc1, plain_failed, plain_err = {}, {}, {}
    with cf.ThreadPoolExecutor(JOBS) as ex:
        for i, rc, err, a in ex.map(plain, range(len(cmds))):
            cc1[i] = a
            plain_err[i] = err
            if rc:
                plain_failed[i] = err[-300:]

    chunks = [list(range(len(cmds)))] if not CHUNK else \
        [list(range(k, min(len(cmds), k + CHUNK))) for k in range(0, len(cmds), CHUNK)]

    def serve_driver(c, ids):
        res = os.path.join(d, f"res{c}.jsonl")
        sockdir = tempfile.mkdtemp(prefix="bs-")  # short: a socket path has a length limit
        sock = os.path.join(sockdir, "s")
        senv = {k: v for k, v in env.items() if not k.startswith("CLANG_COMPILE_SERVER")}
        srv = subprocess.Popen([CLANG, "-cc1", "-load", SO, "-plugin", "bothner-server",
                                "-plugin-arg-bothner-server", "socket=" + sock,
                                "-plugin-arg-bothner-server", "results=" + res,
                                "-plugin-arg-bothner-server", "reuse=1"] + OPTION_ARGS + ["-x", "c", "/dev/null"],
                               env=senv, stderr=subprocess.PIPE, text=True)
        while not os.path.exists(sock) and srv.poll() is None:
            time.sleep(0.01)
        local, derr = {}, {}
        for p in range(PASSES):
            denv = dict(env, CLANG_COMPILE_SERVER=sock, CLANG_COMPILE_SERVER_BUILD=f"pass{p}")
            for i in ids:
                cwd, args, src = cmds[i]
                argv = [CLANG] + compiles.config(args, src) + ["-c", src, "-o", os.path.join(d, f"{i:04d}.p{p}.o")]
                r = subprocess.run(argv, cwd=cwd, env=denv, capture_output=True, text=True)
                derr[(p, i)] = r.stderr
                if "compiling locally" in r.stderr:
                    local[(p, i)] = r.stderr[-300:]
        quit_server(sock)
        err = srv.communicate()[1]
        shutil.rmtree(sockdir, ignore_errors=True)
        rows = [json.loads(l) for l in open(res)] if os.path.exists(res) else []
        # The answers in order; the last is the quit request's. A compile run
        # locally failed.
        rows = [r for r in rows if "config" in r or r.get("status")]
        for (p, i), e in local.items():
            rows.insert(p * len(ids) + ids.index(i), {"status": 3, "error": "compiled locally: " + e})
        return c, srv.returncode, err[-1500:], rows, derr

    def serve(c, ids):
        if VIA == "driver":
            return serve_driver(c, ids)
        req, res = os.path.join(d, f"req{c}.jsonl"), os.path.join(d, f"res{c}.jsonl")
        with open(req, "w") as f:
            for p in range(PASSES):
                for i in ids:
                    a = list(cc1[i])
                    a[a.index("-o") + 1] = os.path.join(d, f"{i:04d}.p{p}.o")
                    f.write(json.dumps({"cwd": cmds[i][0], "args": a, "build": f"pass{p}"}) + "\n")
        argv = [CLANG, "-cc1", "-load", SO, "-plugin", "bothner-server",
                "-plugin-arg-bothner-server", "requests=" + req, "-plugin-arg-bothner-server", "results=" + res,
                "-plugin-arg-bothner-server", "reuse=1"] + OPTION_ARGS + ["-x", "c", "/dev/null"]
        r = subprocess.run(argv, env=env, capture_output=True, text=True)
        rows = [json.loads(l) for l in open(res)] if os.path.exists(res) else []
        return c, r.returncode, r.stderr[-1500:], rows, {}

    results = {}
    with cf.ThreadPoolExecutor(min(JOBS, len(chunks))) as ex:
        for c, rc, err, rows, derr in ex.map(lambda x: serve(*x), enumerate(chunks)):
            results[c] = (rc, err, rows, derr)

    report = {"project": proj, "compiles": len(cmds), "plain_failed": len(plain_failed), "passes": []}
    for p in range(PASSES):
        differ, failed, reused, parsed, restored = [], [], 0, 0, 0
        diags = {"same": 0, "include lines": 0, "order": 0, "differ": []}
        moved = set()
        for c, ids in enumerate(chunks):
            rc, err, rows, derr = results[c]
            for k, i in enumerate(ids):
                if i in plain_failed:
                    continue
                n = p * len(ids) + k
                row = rows[n] if n < len(rows) else {"status": 3, "error": f"no answer (server exit {rc}) {err[-300:]}"}
                if row.get("status"):
                    failed.append((i, row.get("error", ""), row.get("diags", "")[-300:]))
                    continue
                reused += row.get("reused", 0)
                parsed += row.get("parsed", 0)
                restored += row.get("restored_decls", 0)
                a = open(os.path.join(d, f"{i:04d}.plain.o"), "rb").read()
                b_path = os.path.join(d, f"{i:04d}.p{p}.o")
                b = open(b_path, "rb").read() if os.path.exists(b_path) else None
                if a != b:
                    differ.append(i)
                # Diagnostics: through the driver, its whole standard error;
                # a request, the server's text against cc1's part of plain
                # clang's.
                if VIA == "driver":
                    pt, st = plain_err[i], derr.get((p, i), "")
                else:
                    pt, st = diagtext.cc1_text(plain_err[i]), diagtext.server_text(row)
                how = diagtext.compare(pt, st)
                if how == "differ":
                    diags["differ"].append(i)
                    open(os.path.join(d, f"{i:04d}.plain.diags"), "w").write(pt)
                    open(os.path.join(d, f"{i:04d}.p{p}.diags"), "w").write(st)
                else:
                    diags[how] += 1
                    if how == "order":
                        moved.update(diagtext.moved(pt, st))
        report["passes"].append({"differ": differ, "failed": failed, "reused": reused, "parsed": parsed,
                                 "restored": restored, "diags_differ": diags["differ"],
                                 "diags_include_lines": diags["include lines"],
                                 "diags_order": diags["order"]})
        print(f"{proj} pass {p}: {len(cmds) - len(plain_failed)} objects, {len(differ)} differ, {len(failed)} failed;"
              f" fragments reused {reused}, parsed {parsed} ({100 * reused / max(reused + parsed, 1):.1f}% of"
              f" those with a decision), declarations restored {restored}", flush=True)
        print(f"{proj} pass {p}: diagnostics same {diags['same']}, in include lines only {diags['include lines']},"
              f" in order only {diags['order']}{' (' + ' '.join(sorted(moved)) + ')' if moved else ''},"
              f" differ {len(diags['differ'])}", flush=True)
        for i in differ[:5]:
            print(f"    differs: {cmds[i][2]} ({d}/{i:04d}.plain.o vs .p{p}.o)")
        for i in diags["differ"][:5]:
            print(f"    diagnostics differ: {cmds[i][2]} (diff {d}/{i:04d}.plain.diags {d}/{i:04d}.p{p}.diags)")
        for i, e, dg in failed[:5]:
            print(f"    failed: {cmds[i][2]}: {e} {dg}")
    if plain_failed:
        print(f"{proj}: {len(plain_failed)} compiles fail under plain clang too (left out)")
    for c, (rc, err, rows, derr) in results.items():
        if rc:
            print(f"{proj} chunk {c}: server exit {rc}: {err[-600:]}")
    print(f"{proj}: {time.time() - s0:.0f} s", flush=True)
    json.dump(report, open(os.path.join(work, proj + ".json"), "w"))
    if not os.environ.get("KEEP"):
        for f in os.listdir(d):
            if f.endswith(".o") and not any(f.startswith(f"{i:04d}.") for p in report["passes"] for i in p["differ"]):
                os.remove(os.path.join(d, f))
    return report


def quit_server(sock):
    """Sends {"quit": true} to a socket server (the framing of CacheProtocol.h,
    MessageKind CompileRequest = 11, CompileReply = 12)."""
    import socket, struct
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock)
        body = bytes([17]) + b'{"quit": true}'
        s.sendall(struct.pack("<I", len(body)) + body)
        s.recv(1 << 16)
    except OSError:
        pass
    finally:
        s.close()


def main():
    work = os.path.abspath(sys.argv[1])
    os.makedirs(work, exist_ok=True)
    bad = 0
    for proj in sys.argv[2:]:
        r = run_project(work, proj)
        bad += sum(len(p["differ"]) + len(p["failed"]) + len(p["diags_differ"]) for p in r["passes"])
    print("all objects and diagnostics the same" if not bad else f"{bad} differences or failures")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
