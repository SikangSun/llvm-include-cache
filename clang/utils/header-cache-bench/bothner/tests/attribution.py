"""Checks BothnerTrace's attribution on a hand-written header.

h.h has a small fragment, a large one (N structs and prototypes), one under
#if, and a last declaration that ends right before #endif, so that its
consumer call comes after the main file's first token (the lookahead case).
Checks:
  1. the fragments are the ones FragCount counts;
  2. fragments plus the other owners add up to the in-process span;
  3. the last declaration's consumer call is charged to its own fragment;
  4. the large fragment's charged time matches what removing it saves: the
     same compile with that fragment blanked (newlines kept), plain clang,
     best of REPS wall times each, pinned to CORE.
usage: attribution.py <workdir>    env: REPS (default 21), CORE (default 3), N
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from compiles import CLANG, ENV, MOD

TRACE = os.path.join(os.path.dirname(HERE), "BothnerTrace.so")
COUNT = os.path.join(os.path.dirname(HERE), "fragcount", "FragCount.so")
REPS = int(os.environ.get("REPS", "21"))
CORE = os.environ.get("CORE", "3")
N = int(os.environ.get("N", "1500"))

work = os.path.abspath(sys.argv[1])
os.makedirs(work, exist_ok=True)
big = "".join(f"struct s_{i} {{ int a; long b; char c[8]; }}; int fn_{i}(struct s_{i} *p, int x);\n" for i in range(N))
head = "#ifndef H_H\n#define H_H\ntypedef int small_t;\nextern small_t small_var;\n#define BIGN 1\n"
tail = "#if BIGN\nint in_if;\n#endif\nint last_decl;\n#endif\n"
open(os.path.join(work, "h.h"), "w").write(head + big + tail)
open(os.path.join(work, "blank.h"), "w").write(head + "\n" * N + tail)
for name, hdr in (("main.c", "h.h"), ("main_blank.c", "blank.h")):
    open(os.path.join(work, name), "w").write(
        f'#include "{hdr}"\nint main_var = 1;\nint f(void) {{ return small_var + last_decl + in_if; }}\n')

def run(argv, env=ENV):
    r = subprocess.run(["taskset", "-c", CORE] + argv, cwd=work, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(argv)}\n{r.stderr}")
    return r

base = [CLANG, "-fsyntax-only"] + MOD

# 1-3: one traced run with the event dump, and FragCount's count.
out, evs, cnt = (os.path.join(work, f) for f in ("trace.json", "events.txt", "count.json"))
run(base + ["main.c", "-fplugin=" + TRACE], dict(ENV, BOTHNER_TRACE_OUT=out, BOTHNER_TRACE_EVENTS=evs))
run(base + ["main.c", "-fplugin=" + COUNT], dict(ENV, FRAGCOUNT_OUT=cnt))
t, c = json.load(open(out)), json.load(open(cnt))
frags = t["frags"]
ok = True
def check(cond, what):
    global ok
    ok &= cond
    print(("ok   " if cond else "FAIL ") + what)

counted = c["sys"]["frag2"] + c["proj"]["frag2"]
check(len(frags) == counted, f"fragments: {len(frags)} traced, {counted} counted")
total = sum(f[5] for f in frags) + sum(t["own_ns"].values())
check(abs(total - t["span_ns"]) <= max(1000, 1e-4 * t["span_ns"]),
      f"parts {total} ns = span {t['span_ns']} ns")
check(t["loose_tokens"] == 0, f"tokens outside every fragment: {t['loose_tokens']}")

lines = open(evs).read().splitlines()
last = max(i for i, f in enumerate(frags))  # fragments are in text order
charged = None
for i, l in enumerate(lines):
    if l.startswith("decl h.h:") and i + 1 < len(lines) and int(l.split(":")[1]) == N + 9:
        charged = lines[i + 1].split()[2] if lines[i + 1].startswith("  -> frag") else lines[i + 1]
check(charged == str(last), f"last_decl's consumer call charged to fragment {charged}, its own is {last}")

# 4: the large fragment against blanking it.
bigf = max(range(len(frags)), key=lambda i: frags[i][5])
check(frags[bigf][3] > N, f"the largest fragment is the generated one ({frags[bigf][3]} tokens)")
traced = []
for _ in range(5):
    run(base + ["main.c", "-fplugin=" + TRACE], dict(ENV, BOTHNER_TRACE_OUT=out))
    traced.append(json.load(open(out)))
best = min(traced, key=lambda x: x["span_ns"])
attributed = best["frags"][bigf][5] / 1e6

def wall(src):
    ts = []
    for _ in range(REPS):
        s = time.perf_counter()
        run(base + [src])
        ts.append(time.perf_counter() - s)
    return min(ts) * 1e3

full, blank = [], []
for _ in range(3):  # interleaved, so drift hits both
    full.append(wall("main.c"))
    blank.append(wall("main_blank.c"))
saved = min(full) - min(blank)
print(f"large fragment: charged {attributed:.2f} ms; blanking it saves {saved:.2f} ms "
      f"(full {min(full):.2f} ms, blanked {min(blank):.2f} ms); ratio {attributed / saved:.3f}")
check(abs(attributed - saved) <= 0.1 * saved, "charged time within 10% of the measured saving")
print("all ok" if ok else "FAILURES")
sys.exit(0 if ok else 1)
