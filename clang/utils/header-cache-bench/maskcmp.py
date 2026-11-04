"""Compares two ELF objects with the numbers of counter-made names masked
(the user's rule, 2026-10-27: a copy whose __COUNTER__ values only name
things may be served at a lower counter, and the counter then jumps past
its values). Every section but the symbol names must match byte for byte,
and the symbol tables entry by entry, with every number in a name
(_<digits>, as in Linux's __UNIQUE_ID_x_844 and __initcall__kmod_x__1186_2325_f)
masked.
usage: maskcmp.py A.o B.o"""
import re, subprocess, sys


def sections(path):
    out = subprocess.run(['readelf', '-S', '-W', path], capture_output=True, text=True).stdout
    data = open(path, 'rb').read()
    r = {}
    for l in out.splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+([0-9a-f]+)', l)
        if m and m[3] != 'NOBITS':
            off, size = int(m[4], 16), int(m[5], 16)
            r[(int(m[1]), m[2])] = data[off:off + size]
    return r


def symbols(path):
    out = subprocess.run(['readelf', '-s', '-W', path], capture_output=True, text=True).stdout
    rows = []
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 7 and p[0].endswith(':') and p[0][:-1].isdigit():
            name = p[7] if len(p) > 7 else ''
            rows.append((p[1], p[2], p[3], p[4], p[5], p[6], re.sub(r'_\d+(?=_|\b)', '_#', name)))
    return rows


a, b = sys.argv[1], sys.argv[2]
sa, sb = sections(a), sections(b)
bad = [k[1] for k in sa if k[1] not in ('.strtab', '.symtab') and sa[k] != sb.get(k)]
syms = symbols(a) == symbols(b)
print('same with counter names masked' if not bad and syms else
      f'DIFFER: sections {bad}, symbol tables {"same" if syms else "differ"}')
