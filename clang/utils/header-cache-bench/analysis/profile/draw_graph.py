"""Draws one project's header dependency graph from its ddg_run.py traces:
a dot per header, sized by the work spent on its own lines over the whole
build, and two kinds of edges: an #include, and a use the includes do not
explain -- a header used a macro or a declaration from a header it does not
include, directly or through others, so it relies on its includer having
included that header first. A header with such a use is contingent; one
without is modular. Only the largest connected part is drawn. Writes
<out>.pdf, .svg and .png.

contingency.py has the rule: skipped #includes count as includes, and a
use is explained when the header's #includes give that name somewhere.

usage: draw_graph.py <trace dir> <project name> <out> [--labels N] [--seed S]
Needs numpy, matplotlib and networkx."""
import argparse, collections, json, math, os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection, PathCollection
from matplotlib.path import Path
import matplotlib.patheffects as pe
from matplotlib.lines import Line2D
from matplotlib.patches import FancyArrowPatch
import matplotlib.text
import networkx as nx
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import contingency

ap = argparse.ArgumentParser()
ap.add_argument("tdir"); ap.add_argument("name"); ap.add_argument("out")
ap.add_argument("--labels", type=int, default=9, help="application headers to name")
ap.add_argument("--system-labels", type=int, default=2, help="system headers to name, of each kind")
ap.add_argument("--seed", type=int, default=3)
ap.add_argument("--iters", type=int, default=600)
ap.add_argument("--scale", type=float, default=2.0)
ap.add_argument("--gravity", type=float, default=1.0)
a = ap.parse_args()

def is_system(p):
    return p.startswith("/usr/") or "/lib/clang/" in p

# Parsed once per trace directory, kept beside the output.
cache = a.out + ".graph4.json"
if os.path.exists(cache):
    G = json.load(open(cache))
else:
    work = collections.Counter(); reads = collections.Counter(); rule = contingency.Rule()
    compiles = 0
    for fn in sorted(f for f in os.listdir(a.tdir) if f.endswith(".ddg")):
        rs = []
        with open(os.path.join(a.tdir, fn), errors="replace") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if p[0] == "R":
                    rs.append([p[1], p[2], int(p[4]), []])
                elif p[0] == "E" and rs and len(p) >= 7:
                    rs[-1][3].append((p[2], p[3], p[4], p[5], p[6]))
        if not rs:
            continue
        compiles += 1
        own = [r[2] for r in rs]; stack = []
        for i, r in enumerate(rs):
            if r[2] >= (1 << 63):
                own[i] = 0; stack = [i]; continue
            while stack and rs[stack[-1]][0] != r[1]:
                stack.pop()
            if stack and rs[stack[-1]][2] < (1 << 63):
                own[stack[-1]] -= r[2]
            stack.append(i)
        for i, (path, parent, size, us) in enumerate(rs):
            if size >= (1 << 63) or path.endswith(".c"):
                continue
            reads[path] += 1
            work[path] += max(own[i], 0)
            rule.read(path, parent, us)
    hidden, names = rule.finish(set(reads))
    inc = rule.inc
    G = dict(compiles=compiles, work=work, reads=reads,
             inc=[[s, t, c] for (s, t), c in inc.items()],
             hidden=[[s, t, c] for (s, t), c in hidden.items()],
             names={h: dict(c.most_common(8)) for h, c in names.items()})
    json.dump(G, open(cache, "w"))

work, reads = G["work"], G["reads"]
incl = [(s, t) for s, t, c in G["inc"] if s in reads and t in reads and s != t]
contingent = {s for s, t, c in G["hidden"]}
hidden = [(s, t) for s, t, c in G["hidden"] if t != "main file"]
allnodes = sorted(reads)
# The largest connected part, both kinds of edges taken undirected.
adj = collections.defaultdict(set)
for s, t in incl + hidden:
    adj[s].add(t); adj[t].add(s)
seen, best = set(), []
for v in allnodes:
    if v in seen:
        continue
    comp, todo = [], [v]; seen.add(v)
    while todo:
        x = todo.pop(); comp.append(x)
        for y in adj[x]:
            if y not in seen:
                seen.add(y); todo.append(y)
    if len(comp) > len(best):
        best = comp
nodes = sorted(best)
idx = {p: i for i, p in enumerate(nodes)}
n = len(nodes)
incl = [(s, t) for s, t in incl if s in idx and t in idx]
hidden = [(s, t) for s, t in hidden if s in idx and t in idx]
sysm = np.array([is_system(p) for p in nodes]); con = np.array([p in contingent for p in nodes])
print(f"{a.name}: {G['compiles']} compiles, {len(allnodes)} headers, {n} in the largest connected part "
      f"({int((~sysm).sum())} application, {int((~sysm & con).sum())} of them contingent), "
      f"{len(incl)} include edges, {len(hidden)} uses no include explains")

# Layout: ForceAtlas2 (LinLog), include edges pulling harder than uses.
g = nx.Graph()
g.add_nodes_from(range(n))
for s, t in hidden:
    g.add_edge(idx[s], idx[t], weight=0.3)
for s, t in incl:
    g.add_edge(idx[s], idx[t], weight=1.0)
p = nx.forceatlas2_layout(g, max_iter=a.iters, scaling_ratio=a.scale, gravity=a.gravity, linlog=True,
                          distributed_action=True, weight="weight", seed=a.seed)
pos = np.array([p[i] for i in range(n)])
pos -= np.median(pos, 0)
pos /= np.percentile(np.sqrt((pos ** 2).sum(1)), 99)

# Drawing.
BLUE, VERM, ORANGE, GRAY, EDGE, INK, MUTED = "#0072B2", "#D55E00", "#E69F00", "#BCC3CB", "#A3ACB6", "#1F2328", "#57606A"
def short(p):
    for pre in ("/usr/include/x86_64-linux-gnu/", "/usr/include/"):
        if p.startswith(pre):
            return p[len(pre):]
    return os.path.basename(p)
w = np.array([work[p] for p in nodes], float)
size = 5 + 620 * (w / w.max()) ** 0.55          # dot area, points^2
rad = np.sqrt(size) / 2                          # dot radius, points
fig = plt.figure(figsize=(12, 7.5))
ax = fig.add_axes([0.0, 0.0, 0.74, 1.0])
ax.set_axis_off(); ax.set_aspect("equal")
lo, hi = pos.min(0), pos.max(0); pad = 0.04 * (hi - lo).max()
ax.set_xlim(lo[0] - pad, hi[0] + pad); ax.set_ylim(lo[1] - pad, hi[1] + pad)
ax.add_collection(LineCollection([(pos[idx[s]], pos[idx[t]]) for s, t in incl],
                                 colors=EDGE, linewidths=0.45, alpha=0.45, zorder=1))
# A use no #include explains: an arrow from the user to the header that gave
# the name. Between two system headers, a faint curve only.
def curve(s, t):
    p0, p2 = pos[idx[s]], pos[idx[t]]
    c = (p0 + p2) / 2 + 0.15 * np.array([-(p2 - p0)[1], (p2 - p0)[0]])
    return Path([p0, c, p2], [Path.MOVETO, Path.CURVE3, Path.CURVE3])
sys_only = [(s, t) for s, t in hidden if is_system(s) and is_system(t)]
app_side = [(s, t) for s, t in hidden if not (is_system(s) and is_system(t))]
ax.add_collection(PathCollection([curve(s, t) for s, t in sys_only], facecolors="none",
                                 edgecolors=ORANGE, linewidths=0.5, alpha=0.3, zorder=2))
for s, t in app_side:
    i, j = idx[s], idx[t]
    ax.add_patch(FancyArrowPatch(pos[i], pos[j], connectionstyle="arc3,rad=0.15",
                                 arrowstyle="-|>,head_length=0.45,head_width=0.22", mutation_scale=8,
                                 shrinkA=rad[i], shrinkB=rad[j] + 0.5, lw=0.8, color=ORANGE, alpha=0.75, zorder=2))
ax.scatter(pos[sysm, 0], pos[sysm, 1], s=size[sysm], c=GRAY, edgecolors="white", linewidths=0.4, zorder=3)
for mask, col in ((~sysm & ~con, BLUE), (~sysm & con, VERM)):
    ax.scatter(pos[mask, 0], pos[mask, 1], s=size[mask], c=col, edgecolors="white", linewidths=0.7, zorder=4)

# Labels: the application headers with the most work, and some system
# headers. Each goes at the first of eight places next
# to its dot, then eight places farther out with a leader line, that covers
# no label and no colored or large dot.
fig.canvas.draw()
rend = fig.canvas.get_renderer()
deg = collections.Counter()
for s, t in incl:
    deg[idx[s]] += 1; deg[idx[t]] += 1
# System headers: the hubs of #include edges, and the headers most often
# used without an #include.
inn = collections.Counter(idx[t] for s, t in app_side if is_system(t))
hubs = sorted((i for i in range(n) if sysm[i]), key=lambda i: -deg[i])[:a.system_labels]
used = [i for i, c in inn.most_common() if i not in hubs][:a.system_labels]
order = sorted((i for i in range(n) if not sysm[i]), key=lambda i: -w[i])[:a.labels] + hubs + used
def dot_box(i, pad=1.5):
    x, y = ax.transData.transform(pos[i]); r = (rad[i] + pad) * fig.dpi / 72
    return matplotlib.transforms.Bbox([[x - r, y - r], [x + r, y + r]])
big_sys = sorted((i for i in range(n) if sysm[i]), key=lambda i: -size[i])[:40]
placed = [(i, dot_box(i)) for i in set(np.flatnonzero(~sysm)) | set(big_sys) | set(order)]
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
        t = ax.annotate(short(nodes[i]), pos[i], xytext=(dx, dy), textcoords="offset points",
                        ha=ha, va=va, fontsize=9.5 if not sysm[i] else 8.5, zorder=6,
                        color=INK if not sysm[i] else MUTED,
                        path_effects=[pe.withStroke(linewidth=3, foreground="white")],
                        arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.6, shrinkA=1, shrinkB=rad[i] + 1)
                        if lead else None)
        t.update_positions(rend)
        bb = matplotlib.text.Text.get_window_extent(t, rend).expanded(1.06, 1.1)
        hits = sum(bb.overlaps(o) for j, o in placed if j != i)
        if k == len(tries) or hits == 0:
            placed.append((i, bb))
            break
        if best is None or hits < best[0]:
            best = (hits, (dx, dy, ha, va, lead))
        t.remove()

# Key and note, to the right of the drawing.
app = f"{a.name} header"
hl = [Line2D([], [], marker="o", ls="", ms=9, mfc=VERM, mec="white", label=f"{app}, contingent"),
      Line2D([], [], marker="o", ls="", ms=9, mfc=BLUE, mec="white", label=f"{app}, modular"),
      Line2D([], [], marker="o", ls="", ms=9, mfc=GRAY, mec="white", label="System header (libc, compiler)"),
      Line2D([], [], color=EDGE, lw=1.4, label="#include"),
      Line2D([], [], color=ORANGE, lw=1.4, label="Uses a name from a header\nit does not include")]
leg = fig.legend(handles=hl, loc="upper left", bbox_to_anchor=(0.745, 0.93), frameon=False, fontsize=10,
                 labelspacing=0.9, handletextpad=0.6, title=a.name, title_fontsize=13, alignment="left")
fig.canvas.draw()
top = fig.transFigure.inverted().transform(leg.get_window_extent(rend))[0][1] - 0.04
napp = int((~sysm).sum()); ncon = int((~sysm & con).sum())
note = (f"{G['compiles']} compiles of a first build\n"
        f"{n} headers: {napp} {a.name}, {n - napp} system\n"
        f"{ncon} of the {napp} {a.name} headers are contingent\n"
        f"{len(incl):,} #include edges, {len(hidden):,} orange edges\n\n"
        "Contingent: uses a macro or a declaration\nfrom a header that it does not include,\n"
        "directly or through other headers.\n"
        "An orange arrow points to that header.\n\n"
        "Dot area: work on the header's own lines,\nsummed over all compiles.")
fig.text(0.755, top, note, fontsize=9, color=MUTED, va="top", linespacing=1.45)
for ext in ("pdf", "svg", "png"):
    fig.savefig(f"{a.out}.{ext}", dpi=300 if ext == "png" else None, facecolor="white")
print("wrote", a.out + ".{pdf,svg,png}")
