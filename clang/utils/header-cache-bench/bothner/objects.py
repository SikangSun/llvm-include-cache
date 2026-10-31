"""BothnerTrace is passive (COMPILE_SERVER_PLAN.md 3.4, step D6): every
recorded compile, built to an object with its own flags and the cache's
modules flags, three ways, serially:
  plain    plain clang;
  timing   with the plugin, recording times (no output written);
  record   with the plugin in record mode, writing its output: the most it
           does, an AST walk before code generation runs.
The objects must be identical byte for byte. This shows the plugin changes
nothing; it shows nothing about Bothner's technique, which the model never
runs (the oracle is that argument).
usage: objects.py <workdir>
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from compiles import CLANG, ENV, MOD, commands, config

TRACE = os.path.join(HERE, "BothnerTrace.so")
work = os.path.abspath(sys.argv[1])
os.makedirs(work, exist_ok=True)
modes = {"plain": ([], {}), "timing": (["-fplugin=" + TRACE], {}),
         "record": (["-fplugin=" + TRACE], {"BOTHNER_TRACE_RECORD": "1"})}
same = differ = failed = 0
for i, (cwd, args, src) in enumerate(commands()):
    objs, rcs = {}, {}
    for m, (extra, env) in modes.items():
        o = os.path.join(work, f"{i:03d}.{m}.o")
        e = dict(ENV, **env)
        if m == "record":
            e["BOTHNER_TRACE_OUT"] = os.path.join(work, f"{i:03d}.record.json")
        r = subprocess.run([CLANG] + MOD + config(args, src) + ["-c", src, "-o", o] + extra,
                           cwd=cwd, env=e, capture_output=True)
        rcs[m] = r.returncode
        objs[m] = open(o, "rb").read() if r.returncode == 0 and os.path.exists(o) else None
    if any(rcs.values()):
        failed += 1
        print(f"{i:3d} {src}: failed {rcs}", flush=True)
    elif objs["plain"] == objs["timing"] == objs["record"]:
        same += 1
        for m in modes:
            os.remove(os.path.join(work, f"{i:03d}.{m}.o"))
    else:
        differ += 1
        print(f"{i:3d} {src}: objects differ "
              f"(timing {'same' if objs['timing'] == objs['plain'] else 'differs'}, "
              f"record {'same' if objs['record'] == objs['plain'] else 'differs'})", flush=True)
print(f"{same} compiles identical three ways, {differ} differ, {failed} failed")
