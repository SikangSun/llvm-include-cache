"""The recorded compiles of a build, as pph/pph_run.py selects them: each call
that compiles one .c file, with its working directory and its flags less the
source, output and dependency-file arguments."""
import os

LOGS = os.environ.get("LOGS", "/home/sikang/local_IncludeCache/bench/logs/log-redis")
BIN = os.environ.get("BIN", os.path.normpath(os.path.join(os.path.dirname(__file__), "../../../../build_rel/bin")))
CLANG = os.path.join(BIN, "clang")
MOD = ["-fmodules", "-fno-implicit-module-maps"]
# The cache's own switches must not reach a compile measured as plain clang.
ENV = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_PPH", "BOTHNER_"))}


def commands():
    """(working directory, recorded argv, source) for each one-.c-file compile."""
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


def front_end(cwd, args, src, extra=()):
    """The front end alone, as the cache's runs measure it."""
    return [CLANG, "-fsyntax-only"] + MOD + config(args, src) + [src] + list(extra)
