"""Folding threshold sweep (u31cut, u31cut2): per project and cutoff,
instructions against plain clang, lookups, hits, links per hit, copies
written, and instructions split by compiles' role.
usage: cutoff.py <dir> [<dir> ...]"""
import json, re, sys

HIT = re.compile(r"header cache: (\d+) hits / (\d+) lookups, (\d+) candidates rejected, \d+ not at a declaration boundary, (\d+) not on the ask-list")
EXP = re.compile(r"header cache: exported (\d+) parts")
LNK = re.compile(r"header cache hit: (\S+) links=(\d+) combo=([0-9A-F]+) instr=(\d+)")
TXT = re.compile(r"header cache text time: ([\d.]+) ms reading (\d+) outermost")
CHK = re.compile(r"header cache time: ([\d.]+) ms in lookups \(([\d.]+) waiting for the daemon, ([\d.]+) checking candidates, ([\d.]+) loading\)")
for d in sys.argv[1:]:
    for p in ("git", "redis"):
        try:
            j = json.load(open(f"{d}/cut-{p}.json"))
        except FileNotFoundError:
            continue
        plain = sum(j["I"]["plain"])
        print(f"## {d.split('/')[-1]} {p} ({len(j['I']['plain'])} compiles, plain {plain/1e9:.1f} G)")
        print("| Cutoff (us) | Instr (G) | Speedup | Hits | Lookups | Rejected | Not asked | Links per hit | Hit instr (G) | Copies written | Check ms | Load ms |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for m in j["modes"][1:]:
            tot = sum(j["I"][m])
            h = l = r = na = e = 0; links = []; hi = 0; chk = ld = 0.0
            for s in j["S"][m]:
                if x := HIT.search(s):
                    h += int(x[1]); l += int(x[2]); r += int(x[3]); na += int(x[4])
                if x := EXP.search(s):
                    e += int(x[1])
                if x := CHK.search(s):
                    chk += float(x[3]); ld += float(x[4])
                for g in LNK.finditer(s):
                    links.append(int(g[2])); hi += int(g[4])
            lp = sum(links) / len(links) if links else 0
            print(f"| {m[7:]} | {tot/1e9:.1f} | {plain/tot:.2f}x | {h} | {l} | {r} | {na} | {lp:.1f} | {hi/1e9:.1f} | {e} | {chk:.0f} | {ld:.0f} |")
        print()
