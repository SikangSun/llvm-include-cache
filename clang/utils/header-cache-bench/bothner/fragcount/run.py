"""Runs the fragcount plugin on every selected redis compile, serially.
usage: run.py <outdir> [nomod] [limit]"""
import json, os, subprocess, sys, time
LOGS = "/home/sikang/local_IncludeCache/bench/logs/log-redis"
CLANG = "/home/sikang/local_IncludeCache/include-cache/code/llvm-project/build_rel/bin/clang"
PLUGIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "FragCount.so")
MOD = ["-fmodules", "-fno-implicit-module-maps"]

# commands() and config() copied verbatim from pph_run.py
def commands():
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

out = os.path.abspath(sys.argv[1])
mod = [] if "nomod" in sys.argv[2:] else MOD
limit = int(sys.argv[3]) if len(sys.argv) > 3 else None
os.makedirs(out, exist_ok=True)
env = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_PPH"))}
cmds = commands()
print(len(cmds), "compiles", flush=True)
for i, (cwd, args, src) in enumerate(cmds[:limit]):
    o = os.path.join(out, f"{i:03d}.json")
    if os.path.exists(o):
        continue
    argv = [CLANG, "-fsyntax-only"] + mod + config(args, src) + [src, "-fplugin=" + PLUGIN, "-fno-crash-diagnostics"]
    t = time.time()
    r = subprocess.run(argv, cwd=cwd, env=dict(env, FRAGCOUNT_OUT=o + ".tmp"), capture_output=True, text=True)
    dt = time.time() - t
    meta = {"i": i, "cwd": cwd, "src": src, "argv": argv, "rc": r.returncode, "secs": dt,
            "stderr_tail": r.stderr[-2000:]}
    d = json.load(open(o + ".tmp")) if os.path.exists(o + ".tmp") else None
    if d is not None:
        os.remove(o + ".tmp")
    json.dump({"meta": meta, "data": d}, open(o, "w"))
    print(i, r.returncode, f"{dt:.2f}s", src, flush=True)
