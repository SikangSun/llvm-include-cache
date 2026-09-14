# Shelved: a read-set argument for the dependency record

Not a plan of record. This is the line of thinking to pick up if the work is
written up, kept here so it does not have to be reconstructed.

## The frame

Parsing a header is a function of two things: the header's text, and the
compiler state it is parsed into. Caching a parsed header is memoization of
that function. An entry is sound exactly when the live state agrees with the
recorded state on **everything the parse actually read**.

So "dependency" is not a category anyone invents. It is the *read set*, and
the only question is whether what we record covers it.

## How the record is built today, and why that cannot be a theorem

The recorder observes a set of callbacks that happened to be reachable:
definedness tests, macro expansions, the four Sema name queries, skipped
includes, pragma snapshots. Everything outside that set is an omission, and
an omitted dependency makes an entry match *more* compiles, not fewer.

Every gap so far was found by building a counterexample by hand:
`#if UNDEFINED > 5`, guard-suppressed includes, `#elifdef`, incoming pragma
state, the outgoing-effect category. There is no method behind that, and no
test that reports "this record is missing something".

## The claim worth aiming at

Not "we thought of everything", but:

> Every read of state component X goes through API Y. We instrument Y.
> Therefore the recorded read set is complete for X.

That converts completeness into something mechanically checkable: make the
state private, force reads through the instrumented accessors, and a
violation becomes a compile error instead of an omission nobody noticed.
Macro state is the clean case — definedness, replacement list, and the
`push_macro` history are reached through a small set of `Preprocessor`
methods.

## Getting the totality, finitely

The mutable state a header's processing can read is enumerable, because it is
the members of a few classes: `Preprocessor`, `HeaderSearch`, the
`IdentifierTable`, the file system as seen through `FileManager`, and Sema's
pragma stacks. Each member falls in exactly one of five buckets:

1. fixed by the compile's flags, so the PCM's own configuration check covers
   it;
2. fixed by the header's own text, so it cannot vary;
3. recorded as a dependency;
4. required to be at its default, or the header is declined;
5. any read of it excludes the header outright.

That table is the contribution: the categories fall out of an enumeration
instead of out of examples. It also names things currently treated casually:

- the file system is an input — `__has_include`, and include resolution
  itself;
- `#include_next` depends on where in the search path the current file was
  found (already learned the hard way, see DECISIONS.md 2.9);
- poisoned identifiers (`#pragma GCC poison`);
- diagnostic mappings (`#pragma clang diagnostic`), which decide whether a
  header compiles at all;
- the segment and visibility pragmas, beyond `pack` and `float_control`.

## Where it stops

The preprocessor layer looks closable this way. The Sema layer does not, at
the same rigour: the AST tier also relies on Clang merging a loaded
declaration with one the compile already has, and that is a structural
equivalence argument about C types, not a read-set argument.

One honest structure: prove the preprocessor-level read set complete by
mediation, state the merge condition as an assumption discharged by Clang's
existing module semantics, and support the whole with differential testing —
build the benchmarks both ways and run each project's own test suite.

## First step, when picked up

Survey the members of `Preprocessor` alone and produce the five-bucket table
for that one class. It is a few hundred members across the eventual five
classes; one class is enough to see whether the method pays for itself, and
it should surface missing dependency kinds before a counterexample does.
