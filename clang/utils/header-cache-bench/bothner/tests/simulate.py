"""Checks simulate.py's decisions on a build worked out by hand: the three
compiles of tests/record.py (m1, m2, m3, in that order), recorded, then
replayed by simulate.py (the same records serve as the timing).

Fragments: m1 and m3 read buf.h (BUFSIZ 10), a.h, b.h, en.h (three fragments,
all inside the enum) and f.h; m2 reads buf.h with BUFSIZ 20, b.h after
`#define int size_t`, and f.h as ./sub/../f.h. One version is kept per
location, the latest parse, and the enum's fragments are never reused.
After the first build (m1, m2, m3), buf.h and b.h hold m3's versions. In the
recompilation:
  m1: buf, a, b, f reused; en 3 nested                 4 reused, 3 nested
  m2: buf, b changed (and now hold m2's); a, f reused  2 reused, 2 changed, 3 nested
  m3: buf, b changed back; a, f reused                 2 reused, 2 changed, 3 nested
usage: simulate.py <workdir>   (runs tests/record.py into <workdir>/record first)
"""
import json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
work = os.path.abspath(sys.argv[1])
rec = os.path.join(work, "record")
subprocess.run([sys.executable, "-I", os.path.join(HERE, "record.py"), rec], check=True, capture_output=True)
build = os.path.join(work, "build")
shutil.rmtree(build, ignore_errors=True)
os.makedirs(build)
for i, src in enumerate(["m1.c", "m2.c", "m3.c"]):
    d = json.load(open(os.path.join(rec, src + ".json")))
    json.dump({"meta": {"i": i, "src": src, "cwd": rec}, "data": d}, open(os.path.join(build, f"{i:03d}.json"), "w"))
r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(HERE), "simulate.py"), build, build],
                   capture_output=True, text=True, check=True)
line = r.stdout.splitlines()[0]
got = {k: int(v) for k, v in re.findall(r"(\w+) (\d+) \(", line)}
want = {"reused": 8, "changed": 4, "nested": 9}
print(line)
print("ok" if got == want else f"FAIL: expected {want}, got {got}")
sys.exit(0 if got == want else 1)
