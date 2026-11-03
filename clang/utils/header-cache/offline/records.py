"""Reads a build's record (CLANG_HEADER_CACHE_RECORD: one .rec file per
compile, tab-separated lines; see HeaderCacheServer's Recording) into what
the fold and merge rules read: per header, per copy, per (parent copy,
child copy) pair, and per tree. Every cost is a mean of this build's own
measurements of that header or copy; nothing is fixed.

Lines:
  R header parent hit|miss request-us load-us copy|- bytes
  K copy child child-copy loaded|skipped      (before the R line of its hit)
  C copy tree links bytes                     (before the R line of its hit)
  T header parent own-us folded-us asked first
  W header copy bytes writer-us [child child-copy]...
  A directory program arg...                  (first: the compile's command)
  E NAME=VALUE...                             (its CLANG_HEADER_CACHE variables)
"""
import collections, glob, os, statistics


class Header:
    def __init__(self):
        self.requests = []          # request us, one per request
        self.held = []              # own + folded us, one per first read
        self.hits = 0
        self.misses = 0
        self.out_requests = 0       # requests where the .c file includes it
        self.out_hits = collections.Counter()   # copy -> hits there
        self.out_misses = 0

    def ask(self):
        return statistics.mean(self.requests) if self.requests else None

    def parse(self):
        return statistics.mean(self.held) if self.held else None


class Copy:
    def __init__(self, header):
        self.header = header
        self.bytes = None
        self.write = []             # writer us, one per write
        self.loads = []             # load us shares, one per load
        self.hits = 0               # requests it served
        self.links = []             # (child header, child copy), from W
        self.out_created = 0        # written after a miss at the .c file

    def load(self):
        return statistics.mean(self.loads) if self.loads else None

    def write_us(self):
        return statistics.mean(self.write) if self.write else None


class Pair:
    """A parent copy linking a child copy."""
    def __init__(self):
        self.loaded = 0             # P_hit#: hits of the parent that loaded it
        self.skipped = 0            # out_of_order#: hits that skipped it
        self.created_hit = 0        # h_hit#: the child's request inside the
        self.created_miss = 0       # parent, when the parent copy was written


class Tree:
    def __init__(self, root):
        self.root = root
        self.hits = 0
        self.links = 0
        self.bytes = 0
        self.units = None           # the copies loaded: root, then links
        self.parents = collections.Counter()    # hits by the includer, - for the .c file
        self.top_compiles = []      # compiles that hit it where the .c file includes the root
        # links loaded, over its hits: (includer, parent copy, child) -> loads
        self.link_loads = collections.Counter()


def compiles(record_dir):
    """Each compile's record lines, split into fields, in build order: a
    file is named pid-nanoseconds.rec, by the time the compile began."""
    files = glob.glob(os.path.join(record_dir, "*.rec"))
    files.sort(key=lambda f: int(os.path.basename(f).split("-")[1].split(".")[0]))
    return [[l.rstrip("\n").split("\t") for l in open(f, errors="replace")] for f in files]


class Build:
    def __init__(self, lines):
        self.headers = collections.defaultdict(Header)
        self.copies = {}
        self.pairs = collections.defaultdict(Pair)
        self.trees = {}
        self.compiles = 0
        self.commands = {}          # compile -> (directory, program, args), from A lines
        self.envs = {}              # compile -> its CLANG_HEADER_CACHE variables, from E lines
        self._unsized = []          # loads waiting for their copies' sizes
        for L in lines:
            self._read(L)

    def copy(self, header, name):
        c = self.copies.get(name)
        if c is None:
            c = self.copies[name] = Copy(header)
        return c

    def _read(self, lines):
        index = self.compiles
        self.compiles += 1
        pending_k, pending_c = [], None
        requests_in = {}            # (parent, child) -> (hit|miss, copy)
        served = set()              # (header, copy) of each hit
        writes = []
        for f in lines:
            kind = f[0]
            if kind == "A" and len(f) > 3:
                self.commands[index] = (f[1], f[2], f[3:])
            elif kind == "E":
                self.envs[index] = dict(v.split("=", 1) for v in f[1:] if "=" in v)
            elif kind == "K":
                pending_k.append(f)
            elif kind == "C":
                pending_c = f
            elif kind == "R":
                header, parent, outcome, req, load, copy, nbytes = f[1], f[2], f[3], int(f[4]), int(f[5]), f[6], int(f[7])
                h = self.headers[header]
                h.requests.append(req)
                if outcome == "hit":
                    h.hits += 1
                    served.add((header, copy))
                    c = self.copy(header, copy)
                    c.hits += 1
                    # The load's share for each copy loaded, by bytes: the
                    # hit's own copy, then each link loaded.
                    units = [(header, copy)]
                    if pending_c is not None and pending_c[1] == copy:
                        units += [(k[2], k[3]) for k in pending_k if k[4] == "loaded"]
                        for k in pending_k:
                            p = self.pairs[(k[1], k[3])]
                            if k[4] == "loaded":
                                p.loaded += 1
                            else:
                                p.skipped += 1
                        t = self.trees.get(pending_c[2])
                        if t is None:
                            t = self.trees[pending_c[2]] = Tree(copy)
                        t.hits += 1
                        t.parents[parent] += 1
                        if parent == "-" and (not t.top_compiles or t.top_compiles[-1] != index):
                            t.top_compiles.append(index)
                        t.links = int(pending_c[3])
                        t.bytes = int(pending_c[4])
                        if t.units is None:
                            t.units = [u for _, u in units]
                        for k in pending_k:
                            if k[4] == "loaded":
                                t.link_loads[(parent, k[1], k[2])] += 1
                    sizes = [self.copy(hh, u).bytes for hh, u in units]
                    if all(s is not None for s in sizes) and sum(sizes):
                        total = sum(sizes)
                        for (hh, u), s in zip(units, sizes):
                            self.copy(hh, u).loads.append(load * s / total)
                    else:
                        # Sizes come from W lines; a copy not seen written
                        # yet gets its share once they are read (second pass).
                        self._unsized.append((units, load))
                    if parent == "-":
                        h.out_hits[copy] += 1
                else:
                    h.misses += 1
                    if parent == "-":
                        h.out_misses += 1
                if parent == "-":
                    h.out_requests += 1
                requests_in.setdefault((parent, header), (outcome, copy))
                pending_k, pending_c = [], None
            elif kind == "T":
                header, parent, own, folded, asked, first = f[1], f[2], int(f[3]), int(f[4]), f[5], f[6]
                if first == "1":
                    self.headers[header].held.append(own + folded)
            elif kind == "W":
                writes.append(f)
        # A copy links the copies requested while it was written: inside it,
        # or inside a header read as text inside it, so a request's parent
        # (the header that includes it) need not be the copy's header. The
        # link's request is the hit on that copy, or the miss that wrote it.
        written = {(f[1], f[2]) for f in writes}
        for f in writes:
            header, name, nbytes, us = f[1], f[2], int(f[3]), int(f[4])
            c = self.copy(header, name)
            c.bytes = nbytes
            c.write.append(us)
            links = list(zip(f[5::2], f[6::2]))
            if not c.links:
                c.links = links
            outcome = requests_in.get(("-", header))
            if outcome and outcome[0] == "miss":
                c.out_created += 1
            for child, child_copy in links:
                p = self.pairs[(name, child_copy)]
                if (child, child_copy) in served:
                    p.created_hit += 1
                elif (child, child_copy) in written:
                    p.created_miss += 1

    def finish(self):
        """Shares the loads whose copies' sizes came later in the reading."""
        for units, load in self._unsized:
            sizes = [self.copies[u].bytes if u in self.copies else None for _, u in units]
            if all(s is not None for s in sizes) and sum(sizes):
                total = sum(sizes)
                for (_, u), s in zip(units, sizes):
                    self.copies[u].loads.append(load * s / total)
        self._unsized = []
        return self


def load(lines):
    return Build(lines).finish()


if __name__ == "__main__":
    import sys
    b = load(compiles(sys.argv[1]))
    print(f"{b.compiles} compiles, {len(b.headers)} headers, {len(b.copies)} copies, "
          f"{len(b.pairs)} copy pairs, {len(b.trees)} trees")
    sized = sum(1 for c in b.copies.values() if c.bytes is not None)
    loaded = sum(1 for c in b.copies.values() if c.loads)
    print(f"copies with a size {sized}, with a load measured {loaded}")
    hs = [h for h in b.headers.values() if h.requests]
    print(f"headers requested {len(hs)}; mean request us {statistics.mean(h.ask() for h in hs):.0f}")
    made = sum(1 for p in b.pairs.values() if p.created_hit or p.created_miss)
    print(f"pairs with their creation found {made} of {len(b.pairs)}")
