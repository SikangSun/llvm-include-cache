"""What one build's record says about the cache's costs, from the build's
own timings, in one pass over the record files (build order):

  load us  = l0 + C_link * links + l1 * bytes   (R hit lines; links from C)
  write us = the additive model of gam.py over seven values of the write

C_parse (T lines: own + folded us) and C_ask (R lines: request us) stay
each header's own means: the first build measures them at every read and
every request."""
import collections
import numpy as np

import gam

# The values a write's time is modelled on: the copy's bytes and links, the
# header's parse time in that compile, and what the compile did in all.
WRITE_VALUES = ["bytes", "links", "parse", "loaded", "lbytes", "text", "writes"]


class Costs:
    def __init__(self, lines):
        """\\p lines: records.compiles of the build."""
        self.loads = []             # (links, bytes, us), build order
        self.writes = []            # one dict of WRITE_VALUES and us per write
        self.parse = collections.defaultdict(list)   # header -> own + folded us
        self.ask = collections.defaultdict(list)     # header -> request us
        self.hit_trees = collections.defaultdict(list)  # header -> (links, bytes) per hit
        for L in lines:
            loaded = sum(1 for x in L if (x[0] == "K" and x[4] == "loaded") or
                         (x[0] == "R" and x[3] == "hit"))
            lbytes = sum(int(x[7]) for x in L if x[0] == "R" and x[3] == "hit")
            text = sum(int(x[3]) for x in L if x[0] == "T")
            parse_here = {}
            pending_c = None
            for x in L:
                if x[0] == "C":
                    pending_c = x
                elif x[0] == "R":
                    self.ask[x[1]].append(int(x[4]))
                    if x[3] == "hit":
                        links = int(pending_c[3]) if pending_c is not None and pending_c[1] == x[6] else 0
                        self.loads.append((links, int(x[7]), int(x[5])))
                        self.hit_trees[x[1]].append((links, int(x[7])))
                    pending_c = None
                elif x[0] == "T":
                    parse_here.setdefault(x[1], int(x[3]) + int(x[4]))
                    if x[6] == "1":
                        self.parse[x[1]].append(int(x[3]) + int(x[4]))
            W = [x for x in L if x[0] == "W"]
            for x in W:
                self.writes.append(dict(header=x[1], us=int(x[4]), bytes=int(x[3]),
                                        links=(len(x) - 5) // 2, parse=parse_here.get(x[1], 0),
                                        loaded=loaded, lbytes=lbytes, text=text, writes=len(W)))
        self.fit()

    def fit(self):
        X = np.array([(1, n, b) for n, b, _ in self.loads], float)
        y = np.array([u for *_, u in self.loads], float)
        (self.l0, self.c_link, self.l1), *_ = np.linalg.lstsq(X, y, rcond=None)
        F = np.log1p(np.array([[w[k] for k in WRITE_VALUES] for w in self.writes], float))
        self.write_model = gam.GAM().fit(F, np.log(np.maximum(1, [w["us"] for w in self.writes])))
        per = collections.defaultdict(list)
        for w in self.writes:
            per[w["header"]].append([w[k] for k in WRITE_VALUES])
        self.write_values = {h: np.mean(v, axis=0) for h, v in per.items()}
        self.median_values = np.median(np.array([[w[k] for k in WRITE_VALUES] for w in self.writes], float), axis=0)

    def load_us(self, links, nbytes):
        return self.l0 + self.c_link * links + self.l1 * nbytes

    def write_us(self, header, nbytes=None, parse=None, links=None):
        """A write of a copy of \\p header, its own values from its writes in
        the record unless given."""
        v = np.array(self.write_values.get(header, self.median_values), float)
        if nbytes is not None:
            v[0] = nbytes
        if links is not None:
            v[1] = links
        if parse is not None:
            v[2] = parse
        g = self.write_model
        return float(np.exp(g.predict_log(np.log1p(v)[None, :]))[0] * g.smear)
