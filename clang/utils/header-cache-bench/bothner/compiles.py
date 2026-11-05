"""The recorded compiles of a build, as pph/pph_run.py selects them: each call
that compiles one .c file, with its working directory and its flags less the
source, output and dependency-file arguments.

As in modes_bench.py, SRC_ROOT reads the sources from a copy of BENCH_ROOT
there: dram_sources.sh puts one in /dev/shm/bench-src, where the cache's own
runs read them, so that no compile reads a file from disk."""
import os

LOGS = os.environ.get("LOGS", "/home/sikang/local_IncludeCache/bench/logs/log-redis")
BIN = os.environ.get("BIN", os.path.normpath(os.path.join(os.path.dirname(__file__), "../../../../build_rel/bin")))
CLANG = os.path.join(BIN, "clang")
MOD = ["-fmodules", "-fno-implicit-module-maps"]
# The model's compiles run as plain clang, as Bothner's server would compile;
# BOTHNER_MODULES=1 gives them the cache's modules flags instead. (Under
# -fmodules, Clang's __stddef_*.h headers skip their guards, so they are read
# again in every compile, which plain clang does not do.)
MODEL_MOD = MOD if os.environ.get("BOTHNER_MODULES") == "1" else []
BENCH_ROOT = os.environ.get("BENCH_ROOT", "/home/sikang/local_IncludeCache/bench").rstrip("/") + "/"
SRC_ROOT = os.environ.get("SRC_ROOT", "").rstrip("/")
# The cache's own switches must not reach a compile measured as plain clang.
ENV = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_PPH", "BOTHNER_"))}


def commands():
    """(working directory, recorded argv, source) for each one-.c-file compile."""
    cmds = []
    for name in sorted(os.listdir(LOGS), key=lambda n: int(n.split(".")[0])):
        parts = open(os.path.join(LOGS, name), "rb").read().decode(errors="replace").split("\0")[:-1]
        cwd, args = parts[0], parts[1:]
        if SRC_ROOT:
            cwd = cwd.replace(BENCH_ROOT, SRC_ROOT + "/")
            args = [a.replace(BENCH_ROOT, SRC_ROOT + "/") for a in args]
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


def front_end(cwd, args, src, extra=(), mod=None):
    """The front end alone, as the cache's runs measure it, with the model's
    flags unless mod says otherwise."""
    return [CLANG, "-fsyntax-only"] + (MODEL_MOD if mod is None else mod) + config(args, src) + [src] + list(extra)
