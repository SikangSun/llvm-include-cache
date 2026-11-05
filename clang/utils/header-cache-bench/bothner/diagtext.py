"""The diagnostics of a compile, plain clang's against the server's
(COMPILE_SERVER_DESIGN.md section 5)."""
import re

INCLUDE = "In file included from "
HEAD = re.compile(r"^\S.*?:\d+:\d+: (warning|error|remark): |^(warning|error|remark): |^\d+ (warnings?|errors?)")


def server_text(row):
    """What the driver prints for a reply: the diagnostics, then cc1's count
    line ("2 warnings generated.")."""
    return row.get("diags", "") + row.get("stats", "")


def cc1_text(stderr):
    """Plain clang's standard error without the driver's own lines
    ("clang: warning: argument unused ..."), which a request never makes."""
    return "".join(l for l in stderr.splitlines(True) if not l.startswith("clang: "))


def blocks(text):
    """Each diagnostic with its source lines and notes, without the
    "In file included from" lines."""
    out, cur = [], []
    for line in text.splitlines():
        if line.startswith(INCLUDE):
            continue
        if HEAD.match(line) and cur:
            out.append("\n".join(cur))
            cur = []
        cur.append(line)
    if cur:
        out.append("\n".join(cur))
    return out


def compare(plain, server):
    """"same"; "include lines" when only the "In file included from" lines
    differ (a diagnostic that points into a restored declaration names the
    include chain of the compile that parsed it); "order" when besides that
    the same diagnostics come in another order (clang reports
    -Wunused-macros from a hash set of locations, whose values differ in the
    server; a reused fragment ends the parser's read-ahead past a
    declaration's end early); or "differ"."""
    if plain == server:
        return "same"
    a, b = blocks(plain), blocks(server)
    if a == b:
        return "include lines"
    if sorted(a) == sorted(b):
        return "order"
    return "differ"


def moved(plain, server):
    """The warning options of the diagnostics that changed places."""
    a, b = blocks(plain), blocks(server)
    flags = set()
    for x, y in zip(a, b):
        if x != y:
            for t in (x, y):
                m = re.search(r"\[(-W[^\],]+)", t.splitlines()[0])
                flags.add(m.group(1) if m else "?")
    return sorted(flags)
