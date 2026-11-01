"""Draws which headers the cache stores as units of their own and which it
folds into the unit of the header that includes them, as a disc: a header's
distance from the centre is its first parse time (log scale, longest in the
middle), the time the daemon lists it by. The daemon stores a header as its
own unit when that time is at least -ask-cutoff-us, so the cutoff is a
circle: inside it, stored; outside it, folded. A header sits at the angle of
the header it was first read inside (a radial tree), so a folded header is
drawn near the unit that holds it. Dot area: how many times the build reads
the header. Colors as in draw_graph.py (contingency.py).

usage: draw_folding.py <cut-run json> <mode> <trace dir> <project> <out>
       [--cutoff US] [--best US]
  cut-run json: modes_bench.py output of a linkcut<c> run with LEVEL=2
  (its "text" lines); trace dir: ddg_run.py traces of the same project.
Writes <out>.pdf, .svg and .png. Needs numpy and matplotlib."""
import argparse, collections, json, math, os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patheffects as pe
import matplotlib.text
import matplotlib.colors
from matplotlib.collections import LineCollection
from matplotlib.lines import Line2D
from matplotlib.patches import Circle
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import contingency

ap = argparse.ArgumentParser()
ap.add_argument("cutjson"); ap.add_argument("mode"); ap.add_argument("tdir")
ap.add_argument("name"); ap.add_argument("out")
ap.add_argument("--cutoff", type=int, default=226, help="the run's -ask-cutoff-us")
ap.add_argument("--best", type=int, default=0, help="a second cutoff to draw, dashed")
ap.add_argument("--labels", type=int, default=6, help="headers to name on each side of the cutoff")
a = ap.parse_args()

def norm(p):
    """One name for a header in both runs (their source and clang paths differ)."""
    i = p.find("/lib/clang/")
    if i >= 0:
        return "clang/" + p[i:].split("/include/", 1)[1]
    m = re.search(r"/(bench-src|benchmarks)/[^/]+/", p)
    return p[m.end():] if m else p

def is_system(p):
    return p.startswith("/usr/") or p.startswith("clang/")

def short(p):
    for pre in ("/usr/include/x86_64-linux-gnu/", "/usr/include/", "clang/"):
        if p.startswith(pre):
            return p[len(pre):]
    return os.path.basename(p)

# Each header's first read as text. Log lines come as headers are left
# (children first): the tree is rebuilt from their depths and walked in
# entry order, since the daemon keeps the time of the first entry.
LINE = re.compile(r"^  text (\S+) (\d+) depth (\d+) own (\d+) why (\S+)", re.M)
first = {}
for s in json.load(open(a.cutjson))["S"][a.mode]:
    stack = []
    for g in LINE.finditer(s):
        d = int(g[3]); kids = []
        while stack and stack[-1][0] == d + 1:
            kids.append(stack.pop()[1])
        stack.append((d, (norm(g[1]), int(g[2]), kids[::-1])))
    def walk(node, parent):
        name, us, kids = node
        if name not in first:
            first[name] = (us, parent)
        for k in kids:
            walk(k, name)
    for d, node in stack:
        walk(node, None)

# Reads and contingency from the traces.
reads = collections.Counter(); rule = contingency.Rule(); known = set(); ncomp = 0
for fn in sorted(f for f in os.listdir(a.tdir) if f.endswith(".ddg")):
    rs = []
    with open(os.path.join(a.tdir, fn), errors="replace") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if p[0] == "R":
                rs.append([p[1], p[2], int(p[4]), []])
            elif p[0] == "E" and rs and len(p) >= 7:
                rs[-1][3].append((p[2], p[3], p[4], p[5], p[6]))
    ncomp += bool(rs); seen = set()
    for path, parent, size, us in rs:
        if size >= (1 << 63) or path.endswith(".c"):
            continue
        if norm(path) not in seen:
            seen.add(norm(path)); reads[norm(path)] += 1     # compiles that read it
        known.add(path)
        rule.read(path, parent, us)
hidden, _ = rule.finish(known)
contingent = {norm(s) for s, t in hidden}

nodes = [p for p in first if p in reads]          # entry order
inset = set(nodes)
us = {p: max(first[p][0], 1) for p in nodes}
stored = {p for p in nodes if us[p] >= a.cutoff}
print(f"{a.name}: {len(nodes)} headers, {len(stored)} at or above {a.cutoff} us (stored), "
      f"{len(nodes) - len(stored)} below (folded); compiles reading a header: median "
      f"{int(np.median(list(reads[p] for p in nodes)))}, max {max(reads[p] for p in nodes)}")

# Radius: decades of first parse time, the longest in the middle.
top = math.log10(max(us.values())) + 0.1; bot = math.floor(math.log10(min(us.values())))
def radius(t):
    return (top - math.log10(t)) / (top - bot)

# Angle: a radial tree over "first read inside", each subtree given an arc
# as wide as the square roots of its compile reads, the header at the
# middle of its arc.
kids = collections.defaultdict(list); roots = []
for p in nodes:
    par = first[p][1]
    (kids[par] if par in inset and par != p else roots).append(p)
weight = {}
def size_of(p):
    weight[p] = math.sqrt(reads[p]) + sum(size_of(k) for k in kids[p])
    return weight[p]
total = sum(size_of(r) for r in roots)
theta = {}
def place(p, start, span):
    theta[p] = start + span / 2
    unit = span / weight[p]; at = start
    for k in kids[p]:
        place(k, at, weight[k] * unit)
        at += weight[k] * unit
at = math.pi / 2
for r in roots:
    place(r, at, 2 * math.pi * weight[r] / total)
    at += 2 * math.pi * weight[r] / total
idx = {p: i for i, p in enumerate(nodes)}
pos = np.array([[radius(us[p]) * math.cos(theta[p]), radius(us[p]) * math.sin(theta[p])] for p in nodes])

# Drawing.
BLUE, VERM, GRAY, EDGE, INK, MUTED, CORE = "#0072B2", "#D55E00", "#BCC3CB", "#A3ACB6", "#1F2328", "#57606A", "#EEF2F6"
sysm = np.array([is_system(p) for p in nodes]); con = np.array([p in contingent for p in nodes])
rd = np.array([reads[p] for p in nodes], float)
size = 3 + 75 * rd / rd.max()                     # dot area, points^2, as compiles reading it
rad = np.sqrt(size) / 2
fig = plt.figure(figsize=(12, 7.5))
ax = fig.add_axes([0.0, 0.0, 0.62, 1.0])
ax.set_axis_off(); ax.set_aspect("equal")
ax.set_xlim(-1.08, 1.08); ax.set_ylim(-1.08, 1.08)
rc = radius(a.cutoff)
ax.add_patch(Circle((0, 0), rc, facecolor=CORE, edgecolor="none", zorder=0))
# Ring labels along the emptiest direction.
ang = np.arctan2(pos[:, 1], pos[:, 0])
quiet = min(np.radians(np.arange(0, 360, 5)),
            key=lambda t: np.sum(np.abs(np.angle(np.exp(1j * (ang - t)))) < np.radians(6)))
for k in range(bot + 1, math.ceil(top)):
    if radius(10 ** k) <= 0.05:
        continue
    ax.add_patch(Circle((0, 0), radius(10 ** k), facecolor="none", edgecolor="#D0D5DB", lw=0.6, zorder=0.5))
    lab = f"{10 ** k} µs" if k < 3 else f"{10 ** (k - 3)} ms"
    ax.text(radius(10 ** k) * math.cos(quiet), radius(10 ** k) * math.sin(quiet), lab, ha="center", va="center",
            fontsize=7, color="#8C959F", zorder=0.6, bbox=dict(boxstyle="square,pad=0.1", fc="white", ec="none"))
# Lines fade with the compiles that read the header, so the ~200 intrinsics
# headers a few compiles read do not hide the rest.
tree = [p for p in nodes if first[p][1] in inset and first[p][1] != p]
rgb = matplotlib.colors.to_rgb(EDGE)
ax.add_collection(LineCollection([(pos[idx[p]], pos[idx[first[p][1]]]) for p in tree],
                                 colors=[(*rgb, 0.08 + 0.5 * reads[p] / max(reads.values())) for p in tree],
                                 linewidths=0.5, zorder=1))
ax.add_patch(Circle((0, 0), rc, facecolor="none", edgecolor=INK, lw=1.5, zorder=2))
if a.best:
    ax.add_patch(Circle((0, 0), radius(a.best), facecolor="none", edgecolor=INK, lw=1.1, ls=(0, (4, 3)), zorder=2))
ax.scatter(pos[sysm, 0], pos[sysm, 1], s=size[sysm], c=GRAY, edgecolors="white", linewidths=0.4, zorder=3)
for mask, col in ((~sysm & ~con, BLUE), (~sysm & con, VERM)):
    ax.scatter(pos[mask, 0], pos[mask, 1], s=size[mask], c=col, edgecolors="white", linewidths=0.6, zorder=4)

# Labels: the stored headers whose units save the most (time x reads) and
# the folded headers read most, each at the first free place around its dot.
fig.canvas.draw(); rend = fig.canvas.get_renderer()
def pick(side, n, key):
    app = sorted((i for i in side if not sysm[i]), key=key)[:n - n // 2]
    sy = sorted((i for i in side if sysm[i]), key=key)[:n // 2]
    return app + sy
# Inside, the units that save the most (time x compiles); outside, the
# folded headers the most compiles read.
ins = pick([i for i, p in enumerate(nodes) if p in stored], a.labels, lambda i: -us[nodes[i]] * rd[i])
outs = pick([i for i, p in enumerate(nodes) if p not in stored], a.labels, lambda i: (-rd[i], -us[nodes[i]]))
order = ins + outs
def dot_box(i, pad=1.5):
    x, y = ax.transData.transform(pos[i]); r = (rad[i] + pad) * fig.dpi / 72
    return matplotlib.transforms.Bbox([[x - r, y - r], [x + r, y + r]])
big = sorted(range(len(nodes)), key=lambda i: -size[i])[:60]
placed = [(i, dot_box(i)) for i in set(big) | set(order)]
def spots(r):
    d = r / math.sqrt(2)
    return [(0, r, "center", "bottom"), (0, -r, "center", "top"), (r, 0, "left", "center"),
            (-r, 0, "right", "center"), (d, d, "left", "bottom"), (-d, d, "right", "bottom"),
            (d, -d, "left", "top"), (-d, -d, "right", "top")]
for i in order:
    tries = [(dx, dy, ha, va, False) for dx, dy, ha, va in spots(rad[i] + 2.5)] + \
            [(dx, dy, ha, va, True) for dx, dy, ha, va in spots(rad[i] + 16)]
    best = None
    for k in range(len(tries) + 1):
        dx, dy, ha, va, lead = tries[k] if k < len(tries) else best[1]
        t = ax.annotate(short(nodes[i]), pos[i], xytext=(dx, dy), textcoords="offset points", ha=ha, va=va,
                        fontsize=9 if not sysm[i] else 8.5, zorder=6, color=INK if not sysm[i] else MUTED,
                        path_effects=[pe.withStroke(linewidth=3, foreground="white")],
                        arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.6, shrinkA=1, shrinkB=rad[i] + 1) if lead else None)
        t.update_positions(rend)
        bb = matplotlib.text.Text.get_window_extent(t, rend).expanded(1.06, 1.1)
        hits = sum(bb.overlaps(o) for j, o in placed if j != i)
        if k == len(tries) or hits == 0:
            placed.append((i, bb)); break
        if best is None or hits < best[0]:
            best = (hits, (dx, dy, ha, va, lead))
        t.remove()

# Key and note.
app = f"{a.name} header"
def ms_or_us(t):
    return f"{t:,} µs" if t < 1000 else f"{t / 1000:g} ms"
hl = [Line2D([], [], marker="o", ls="", ms=9, mfc=VERM, mec="white", label=f"{app}, contingent"),
      Line2D([], [], marker="o", ls="", ms=9, mfc=BLUE, mec="white", label=f"{app}, modular"),
      Line2D([], [], marker="o", ls="", ms=9, mfc=GRAY, mec="white", label="System header (libc, compiler)"),
      Line2D([], [], color=INK, lw=1.5, label=f"Cutoff {ms_or_us(a.cutoff)} (default)")]
if a.best:
    hl.append(Line2D([], [], color=INK, lw=1.1, ls=(0, (4, 3)), label=f"Cutoff {ms_or_us(a.best)} (best for {a.name})"))
hl.append(Line2D([], [], color=EDGE, lw=1.4, label="Line to the header that\nfirst included it"))
leg = fig.legend(handles=hl, loc="upper left", bbox_to_anchor=(0.635, 0.95), frameon=False, fontsize=10,
                 labelspacing=0.8, handletextpad=0.6, title=a.name, title_fontsize=13, alignment="left")
fig.canvas.draw()
y = fig.transFigure.inverted().transform(leg.get_window_extent(rend))[0][1] - 0.03
nin = len(stored); nout = len(nodes) - nin
def held(p):
    """Whether some stored header holds p: p or a header it was first read inside."""
    while p is not None and p in inset:
        if p in stored:
            return True
        p = first[p][1]
    return False
loose = sum(1 for p in nodes if not held(p))
note = (f"{len(nodes)} headers in {a.name}'s first build ({ncomp} compiles)\n\n"
        f"Distance from the centre: the header's first parse time\n"
        f"(log scale). The cache stores a header as a unit of its\n"
        f"own when that time is at least the cutoff.\n\n"
        f"Inside the {ms_or_us(a.cutoff)} circle: {nin} headers. The cache looks\neach one up, and stores it, as a unit of its own.\n"
        f"Outside: {nout} headers, folded into the unit of\nthe header that includes them. {loose} of them\n"
        f"have no stored header around them and are\nread as text.\n")
if a.best:
    nb = sum(1 for p in nodes if us[p] >= a.best)
    note += f"At {ms_or_us(a.best)}: {nb} stored, {len(nodes) - nb} folded.\n"
note += f"\nDot area: how many of the {ncomp} compiles read the header."
fig.text(0.645, y, note, fontsize=9, color=MUTED, va="top", linespacing=1.45)
for ext in ("pdf", "svg", "png"):
    fig.savefig(f"{a.out}.{ext}", dpi=300 if ext == "png" else None, facecolor="white")
print("wrote", a.out + ".{pdf,svg,png}")
