"""The rule profile_graph.py and draw_graph.py use to call a header
contingent: some read of it used a macro or a declaration from outside, and
the header's #includes do not explain that use.

- An #include the compiler skipped because the file was already in counts
  as an include. The trace shows it as the includer's test of the file's
  guard (the file's first test of a macro that nothing defined).
- A use is explained when its provider is reachable through #includes, or
  when a reachable header gives the same name somewhere in the build: glibc's
  stdio.h and <stdarg.h> both give va_list, and the first one in provides it.
- A name the main file defined before the #include is outside too. Its
  provider is reported as "main file"."""
import collections


class Rule:
    def __init__(self):
        self.inc = collections.Counter()               # (includer, header) -> reads
        self.uses = collections.Counter()              # (header, name, provider) -> reads
        self.providers = collections.defaultdict(set)  # name -> headers that gave it
        self.guard = {}                                # header -> its guard macro
        self.tests = collections.Counter()             # (header, macro, provider) -> reads

    def read(self, path, parent, uses):
        """One read of a header: its includer and its E records, each
        (kind, name, provider, in|out, value)."""
        if not parent.endswith(".c") and parent != "-":
            self.inc[(parent, path)] += 1
        for kind, name, prov, io, val in uses:
            if prov != "-":
                self.providers[name].add(prov)
            if kind == "C" and prov == "-" and val == "0":
                self.guard.setdefault(path, name)
            if kind == "C" and prov not in ("-", path):
                self.tests[(path, name, prov)] += 1
            if io == "out" and prov not in ("-", path):
                self.uses[(path, name, prov)] += 1

    def finish(self, headers):
        """Adds the skipped #includes to inc. Returns the reads per
        (header, provider) of uses no #include explains, and the names behind
        them per header."""
        for (h, name, prov), c in self.tests.items():
            if self.guard.get(prov) == name and prov in headers:
                self.inc[(h, prov)] += c
        kids = collections.defaultdict(set)
        for s, t in self.inc:
            kids[s].add(t)
        reach = {}
        def reachable(h):
            if h not in reach:
                seen, todo = set(), [h]
                while todo:
                    for y in kids.get(todo.pop(), ()):
                        if y not in seen:
                            seen.add(y); todo.append(y)
                reach[h] = seen
            return reach[h]
        hidden = collections.Counter(); names = collections.defaultdict(collections.Counter)
        for (h, name, prov), c in self.uses.items():
            if h not in headers or not (prov in headers or prov.endswith(".c")):
                continue
            r = reachable(h)
            if prov in r or not self.providers[name].isdisjoint(r):
                continue
            hidden[(h, "main file" if prov.endswith(".c") else prov)] += c
            names[h][name] += c
        return hidden, names
