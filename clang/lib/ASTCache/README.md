# Header compilation cache

An experimental per-header compilation cache for Clang. Two independent
optimizations sit on one shared observation layer:

- **Token-stream cache** — saves a header's lexed-but-unexpanded tokens, so a
  hit skips lexing that header's text.
- **AST cache** — saves a header's *parsed* declarations, so a hit skips
  lexing, macro expansion, and parsing.

This directory is the AST tier. The token tier is in `clang/lib/CacheObserver`.

Design decisions, and the reasoning behind them, are in
[DECISIONS.md](DECISIONS.md). Read that before changing anything here.

## The idea

When a compile reaches `#include "foo.h"`, load a previously parsed copy of
foo.h instead of parsing it again.

That is only sound when nothing outside foo.h could have changed how it
parses. Four things can:

| | Example | What a hit must check |
|---|---|---|
| Conditional | `#ifdef FEATURE` | the macro is defined, or not, the same way |
| Value | `char buf[BUFSZ];` | the macro expands to the same tokens |
| Symbol | `T *p;` — a declaration, or a multiplication? | the name still resolves to the same type |
| Inclusion | an `#include` skipped because the file was already read | that file has likewise been read |

Plus the pragma state active at the `#include`, which no macro records:
`#pragma pack(1)` before an `#include` changes the layout of every struct in
the header.

A header's dependency **record** holds all of that. Recording is done by
observing a real compile, which means an unobserved event is an *omission* —
and an omitted dependency makes an entry match **more** compiles than it
should. Everything here is built to fail closed: anything that cannot be
described soundly costs a cache miss, never a wrong answer.

## Components

```
      producer                    daemon                    client
  ┌──────────────┐          ┌───────────────┐         ┌──────────────┐
  │ compile one  │  PUSH    │ filename ->   │ LOOKUP  │ check each   │
  │ header alone │ ───────> │ entries       │ <────── │ candidate    │
  │ + record its │          │               │ ──────> │ against this │
  │ dependencies │          │ shared memory │ entries │ compile      │
  └──────────────┘          └───────────────┘         └──────────────┘
```

| File | Role |
|---|---|
| `DependencyRecord.h` | what a record holds |
| `ASTCacheDependencyTracker.*` | watches a compile, fills in one record per header |
| `PragmaStateCheck.*` | captures pragma state, replays what a header left behind |
| `MacroTokens.*` | one spelling of a macro's replacement list, shared by both sides |
| `DependencySerialization.*` | record to bytes and back |
| `ByteStream.h` | little-endian primitives, shared |
| `CacheProtocol.*` | LOOKUP / PUSH over a socket |
| `SharedRegion.*` | append-only mmap region holding saved results |
| `CacheDaemon.*` | the server: filename to entries, one lock, thread per connection |
| `ASTCacheProducer.*` | compiles one header alone and records what it needed |
| `CacheClient.*` | asks the daemon, picks a candidate, loads it |
| `HeaderCacheServer.*` | the compiler's side at an `#include`: asks, loads, replays the header's effects, times headers, records jobs |
| `PartExporter.*` | at a miss, writes the header's own part of the real compile and its metadata, and hands it to the server to push |
| `DeclFingerprint.*` | what must match for an outside declaration a part refers to: kind, type, layout, value, definition |

The daemon never interprets a record. It matches on filename and returns every
entry it holds; choosing between them needs the compile's live macro table and
its Sema, which the daemon does not have.

## Trying it

Every compile involved, cached or not, runs with
`-fmodules -fno-implicit-module-maps`: a saved copy is a module, and only a
compile with modules on can load one. See DECISIONS.md §2.8.

Build the two tools:

```
ninja clang-header-cache-daemon clang-header-cache-populate
```

Ask what a project's headers look like, without needing a daemon:

```
clang-header-cache-populate -dry-run -list-declines \
    -Xcc -Isrc -Xcc -DHAVE_CONFIG_H \
    src/*.h
```

This reports how many headers could be saved, the mean size of a saved copy
and of a record, and — for the ones that could not — why. Pass the project's
own compile flags with `-Xcc`; a missing `-I` shows up as a header that
"does not compile in this context."

A header that relies on its includer needs the context the real build gave
it. redis's `anet.h` uses `uint32_t` and never includes `<stdint.h>`, because
everything that includes `anet.h` has included `<stdint.h>` already. Pass the
includes to put in front of a header, in order, with `-context`:

```
clang-header-cache-populate -dry-run -Xcc -Isrc \
    -context '<stdint.h>' src/anet.h
```

Write a header found through the include path the way the source writes it
(`'<stdint.h>'`), not as its resolved path: Clang's `stdint.h` reaches glibc's
with `#include_next`, which only works when the compiler found the file by
searching (DECISIONS.md §2.9).

A compile that misses on a header writes the same three things — the header,
its flags, and the files it had already included — to a job file, which the
tool reads with `-job <file>`. See DECISIONS.md §2.9 for why the context is
compiled into the saved copy but kept hidden from the compile that loads it.

With a daemon running, drop `-dry-run` and results are stored:

```
clang-header-cache-daemon -v &
clang-header-cache-populate -Xcc -Isrc src/*.h
```

A compile uses the cache when `CLANG_HEADER_CACHE` names the daemon's socket.
It asks only about headers on the daemon's ask-list: those some compile took at
least `-ask-cutoff-us` (default 226) to read. `-list-stored` also lists every
header with a stored copy, for trying things out. The compiler asks the daemon
which shared-memory region it writes, and refuses one that is not that
daemon's.

```
CLANG_HEADER_CACHE=/tmp/clang-header-cache.sock clang -fmodules \
    -fno-implicit-module-maps -c anet.c
```

`CLANG_HEADER_CACHE_STATS=1` prints hits and rejection counts when the compile
ends, and where the cache's time went: waiting for the daemon, checking
candidates (each check step on its own line), loading, exporting, and
reading headers as text; `2` also names the macro behind each rejection and
prints a line per cache query and per header read as text, with its time.
`CLANG_HEADER_CACHE_DUMP_PARTS=<dir>` also writes `<part>.sizes`: the bits
each declaration and macro of the part took, with its file and line. With
`-ftime-trace`, the cache's work shows as `HeaderCache Query`, `Ask`,
`Check` and `Export` events. The daemon's `-v` prints the stored bytes by
kind when it exits. `scratch/e2e/build_bench.py`
builds redis-server one compile at a time with plain clang, as a first build
and as a rebuild, and sums these times.

A compile fills the cache itself (DECISIONS.md §2.17): each listed header
it has to read, it exports as its own part and pushes to the daemon.
`CLANG_HEADER_CACHE_EXPORT_PARTS` picks how: unset, serve what fits and export
the misses; `0`, serve only; `populate`, serve nothing and export every listed
header no stored copy fits. `scratch/e2e/export_e2e.py` runs redis through
all three.

A part refers to each declaration from outside its header by name, and the
loading compile resolves the name to its own declaration (DECISIONS.md
§2.18). `CLANG_HEADER_CACHE_OUTSIDE=copy` writes a deep copy of each instead,
as the first version did; a compile then stops exporting at its first load.
`CLANG_HEADER_CACHE_DUMP_PARTS=<dir>` also writes each exported part to
`<dir>/<module name>.pcm`, with the outside declarations it requires in a
`.txt` beside it.

For looking at one part with no daemon: `CLANG_HEADER_CACHE_EXPORT=<abs
header>=<out.pcm>` writes the part to a file, and
`CLANG_HEADER_CACHE_LOAD=<abs header>=<in.pcm>` serves the header from it in a
later compile, checking nothing.
`CLANG_HEADER_CACHE_JOBS=<dir>` makes the compile write, for every header it
reads, the job that builds that header's saved copy; the populate tool runs
them with the build's flags added:

```
clang-header-cache-populate -job <dir>/jobs-XXXX.txt -Xcc -Isrc
```

## Tests

```
ninja AllClangUnitTests
./tools/clang/unittests/AllClangUnitTests \
    --gtest_filter='*ASTCache*:*CacheProtocol*:*Daemon*:*EndToEnd*:*DependencySerialization*:*SymbolDependency*:*Producer*:*Load*:*Hit*:*AskList*'
```

`EndToEndTest` is the one that matters: it runs a daemon on a thread and a
client inside a real compile, then checks each rejection reason on its own —
including that an entry built with `T` as `int` is accepted where `T` is still
`int` and rejected where `T` is `long`, where `T` is a variable, and where `T`
does not exist.

## State

Working: recording, the protocol, the daemon, the producer, the client's
matching and loading, and serving an `#include` from the cache -- written in a
`.c` file or in another header -- with the header's macro effects replayed.

Also working: filling the cache from the real compile (exported parts), which
removed the conflicts between saved copies built after different `.c` files.
Not done: a part that refers to a copy the compile loaded (imports), so a
compile stops exporting at its first hit. NEXT.md has the order.
