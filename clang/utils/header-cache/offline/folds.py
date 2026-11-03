"""Fold decisions for the next build (user 2026-11-02): each parent-child
pair of the first build's record is checked once, bottom to top, by the
user's inequality 2, and each header the .c files include by inequality 1.

  2. keep the link of child h in parent P while
       h_hit# x (C_parse - C_load) + out_of_order# x C_order + extra_parents# x C_write
     > P_hit# x C_link + P_miss# x C_ask + h_miss# x C_write
     otherwise "fold P h"
  1. request h where the .c file includes it while
       h_hit# x (C_parse - C_load) > Req# x C_ask + h_miss# x C_write
     otherwise "top h"

Counts come from the record (records.py), costs from costs.py. C_order of
P: P's copy no longer fits, so P is parsed instead of loaded and written
again. Bottom to top: a parent is checked after all of its children; a
child folded into P adds its parse time and bytes to P's copy, and P's hits
load one link fewer, before P's own edges are checked. The gain of a
decision is its right side minus its left side."""
import collections, statistics


class Graph:
    def __init__(self, build, costs):
        self.b, self.c = build, costs
        hdr_of = {name: c.header for name, c in build.copies.items()}
        creator = {}
        for (pc, cc), p in build.pairs.items():
            if p.created_miss:
                creator[cc] = hdr_of.get(pc)
        E = collections.defaultdict(collections.Counter)
        for (pc, cc), p in build.pairs.items():
            ph, ch = hdr_of.get(pc), hdr_of.get(cc)
            if ph is None or ch is None or ph == ch:
                continue
            e = E[(ph, ch)]
            e["P_hit"] += p.loaded
            e["out_of_order"] += p.skipped
            e["h_hit"] += p.created_hit
            e["h_miss"] += p.created_miss
            if p.created_hit and creator.get(cc, "-") != ph:
                e["extra_parents"] += p.created_hit
        for e in E.values():
            e["P_miss"] = e["h_hit"] + e["h_miss"]
        self.edges = E
        self.children = collections.defaultdict(list)
        for (ph, ch) in E:
            self.children[ph].append(ch)
        own = collections.defaultdict(list)
        for c in build.copies.values():
            if c.bytes is not None:
                own[c.header].append(c.bytes)
        self.own_bytes = {h: statistics.mean(v) for h, v in own.items()}
        self.hit_links = {h: statistics.mean(n for n, _ in v) for h, v in costs.hit_trees.items()}
        self.hit_bytes = {h: statistics.mean(x for _, x in v) for h, v in costs.hit_trees.items()}
        self.hits = {h: H.hits for h, H in build.headers.items()}
        self.parse = {h: statistics.mean(v) for h, v in costs.parse.items()}
        self.ask = {h: statistics.mean(v) for h, v in costs.ask.items()}
        self.outer = {h: (H.out_requests, sum(H.out_hits.values()), H.out_misses)
                      for h, H in build.headers.items() if H.out_requests}

    def order(self):
        """Headers with every child before its parents; an edge back into
        the path being walked (an include cycle) is left out."""
        done, on, out = set(), set(), []
        for root in sorted(set(self.children) | {h for _, h in self.edges}):
            if root in done:
                continue
            stack = [(root, iter(sorted(self.children.get(root, ()))))]
            on.add(root)
            while stack:
                node, it = stack[-1]
                nxt = next(it, None)
                if nxt is None:
                    stack.pop(); on.discard(node); done.add(node); out.append(node)
                elif nxt not in on and nxt not in done:
                    on.add(nxt)
                    stack.append((nxt, iter(sorted(self.children.get(nxt, ())))))
        return out

    def decide(self):
        c = self.c
        parse = dict(self.parse)
        own = dict(self.own_bytes)
        links = dict(self.hit_links)
        hbytes = dict(self.hit_bytes)

        def c_load(h):
            return c.load_us(links.get(h, 0), hbytes.get(h, own.get(h, 0)))

        def c_write(h):
            return c.write_us(h, nbytes=own.get(h), parse=parse.get(h))

        folds, tops = {}, {}
        for P in self.order():
            for h in sorted(self.children.get(P, ())):
                e = self.edges[(P, h)]
                if h not in parse or h not in self.ask or P not in parse:
                    continue
                c_order = parse[P] - c_load(P) + c_write(P)
                left = (e["h_hit"] * (parse[h] - c_load(h)) + e["out_of_order"] * c_order
                        + e["extra_parents"] * c_write(h))
                right = e["P_hit"] * c.c_link + e["P_miss"] * self.ask[h] + e["h_miss"] * c_write(h)
                if right > left:
                    folds[(P, h)] = right - left
                    parse[P] += parse[h]
                    own[P] = own.get(P, 0) + own.get(h, 0)
                    if self.hits.get(P):
                        links[P] = max(0.0, links.get(P, 0) - e["P_hit"] / self.hits[P])
        for h, (req, hit, miss) in sorted(self.outer.items()):
            if h not in parse or h not in self.ask:
                continue
            left = hit * (parse[h] - c_load(h))
            right = req * self.ask[h] + miss * c_write(h)
            if right >= left:
                tops[h] = right - left
        self.after = dict(parse=parse, own=own)
        return folds, tops
