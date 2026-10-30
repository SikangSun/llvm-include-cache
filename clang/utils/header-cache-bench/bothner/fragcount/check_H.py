"""Cross-checks the plugin's header-entry count against clang -H, serially."""
import json, os, re, subprocess, sys
out = sys.argv[1]
env = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_PPH"))}
mism = n = 0
for f in sorted(os.listdir(out)):
    d = json.load(open(os.path.join(out, f)))
    m, x = d["meta"], d["data"]
    if not x: continue
    argv = [a for a in m["argv"] if not a.startswith("-fplugin=")] + ["-H"]
    r = subprocess.run(argv, cwd=m["cwd"], env=env, capture_output=True, text=True)
    h = sum(1 for l in r.stderr.splitlines() if re.match(r"^\.+ ", l))
    e = x["sys"]["entries"] + x["proj"]["entries"]
    n += 1
    if h != e:
        mism += 1; print("mismatch", f, m["src"], h, e)
print(f"checked {n}, mismatches {mism}")
