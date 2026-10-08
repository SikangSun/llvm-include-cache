//===- PragmaStateCheck.h ---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pragma state is an input to a header's compilation that none of the three
// dependency categories (DependencyRecord.h) can observe.
//
//   #pragma pack(1)
//   #include "point.h"      // struct Point { char c; int x; };
//
// Sema::AddAlignmentAttributesForRecord reads AlignPackStack.CurrentValue and
// hangs a MaxFieldAlignmentAttr on Point, so the same header text under the
// same macro and symbol state yields a differently laid-out record. No macro
// is tested and none is expanded, so the tracker records an empty dependency
// set either way: two compiles that must not share an artifact produce
// identical cache keys.
//
// Serialization does not close this. ASTWriter::WritePackPragmaOptions
// returns early when WritingModule is set, so a module-shaped artifact
// carries no pragma state in either direction -- neither the state its
// population assumed, nor any state the header left behind for its includer.
// The module shape is not optional here: a PCH is a translation-unit prefix
// and cannot be spliced at an arbitrary #include.
//
// So this is handled as a precondition rather than a fourth dependency
// category, and it binds at both ends:
//
//   Entry -- at the #include site, before serving a hit or populating, every
//            pragma stack must be at its default. Checked by the consumer.
//   Exit  -- the header must leave the stacks at their default, since a
//            spliced import does not replay a pragma the header set and did
//            not pop, where a textual include would. Checked by the producer
//            at AST-write time.
//
// Both use the same predicate, which is why it lives here rather than in
// either one. What this gives up is caching an #include wrapped in a
// non-default pragma region -- the idiomatic #pragma pack(push,1) wrapper
// included -- which costs a miss, never a wrong hit.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_PRAGMASTATECHECK_H
#define LLVM_CLANG_ASTCACHE_PRAGMASTATECHECK_H

#include "clang/ASTCache/DependencyRecord.h"
#include "clang/Lex/PPCallbacks.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>

namespace clang {
class DiagnosticOptions;
class DiagnosticsEngine;
class Sema;

namespace ast_cache {

/// The name of the first pragma found in a state a PragmaSnapshot cannot
/// express, or nullptr if there is none. Returning the name rather than a
/// bool keeps the reason reportable, which matters because "this header was
/// not cached" is otherwise indistinguishable from a bug.
///
/// #pragma pack and #pragma float_control are deliberately *not* required to
/// be at their default value -- those are captured by capturePragmaState and
/// travel in the record. What is still required is that their stacks be
/// balanced, since a snapshot holds a value and not a stack.
const char *findNonDefaultPragmaState(const Sema &S);

/// Whether \p S carries no pragma state beyond what a snapshot can express.
inline bool isPragmaStateDefault(const Sema &S) {
  return findNonDefaultPragmaState(S) == nullptr;
}

/// The small-value pragma state active right now, for the Entry/Exit fields
/// of a dependency record. Comparing two snapshots is meaningful; reading a
/// field out of one is not -- the encodings are opaque by design.
PragmaSnapshot capturePragmaState(const Sema &S);

/// A hash of the flags that decide which diagnostics a compile prints, and
/// as what: -W..., -w, -pedantic, -pedantic-errors, -R... and a warning
/// suppression mappings file. A copy holds the diagnostics its header
/// printed under its compile's flags, and is served only under the same
/// (REPORT.md Q34). Flags that change only how a diagnostic looks -- colors,
/// columns, carets -- are not in it.
uint64_t warningFlagsHash(const DiagnosticOptions &Opts);

/// Follows #pragma GCC diagnostic, #pragma clang diagnostic and #pragma
/// warning: which are in force, and how they nest. A copy is served only
/// where the same are in force as where it was written (warningState), and a
/// hit applies again the ones its header left in force (REPORT.md Q34).
class WarningPragmaTracker : public PPCallbacks {
public:
  /// A hash of the pragmas in force here and of how they nest.
  uint64_t fingerprint() const;

  /// Where the pragmas stand: to find what a header left in force.
  struct Mark {
    size_t Depth = 0;
    unsigned Level = 0;
    size_t Changes = 0;
    unsigned Opaque = 0;
  };
  Mark mark() const;

  /// The pragmas made since \p Begin that are still in force, at the level
  /// \p Begin was at; or std::nullopt if a hit could not apply them again:
  /// a push left unpopped, a pop of a push made before \p Begin, or a
  /// #pragma warning.
  std::optional<std::vector<WarningPragma>> since(const Mark &Begin) const;

  /// Applies \p P at \p Loc, as its #pragma did where the header was read.
  void apply(DiagnosticsEngine &Diags, const WarningPragma &P,
             SourceLocation Loc);

  void PragmaDiagnosticPush(SourceLocation, StringRef) override { push(); }
  void PragmaDiagnosticPop(SourceLocation, StringRef) override { pop(); }
  void PragmaDiagnostic(SourceLocation, StringRef, diag::Severity Severity,
                        StringRef Option) override {
    Levels.back().Changes.push_back(
        {static_cast<uint8_t>(Severity), Option.str()});
  }
  void PragmaWarning(SourceLocation, PragmaWarningSpecifier,
                     ArrayRef<int>) override {
    ++Levels.back().Opaque;
  }
  void PragmaWarningPush(SourceLocation, int) override { push(); }
  void PragmaWarningPop(SourceLocation) override { pop(); }

private:
  struct Level {
    unsigned Id;
    std::vector<WarningPragma> Changes;
    /// #pragma warning(disable: ...) and the like, which are not replayed.
    unsigned Opaque = 0;
  };
  void push() { Levels.push_back({NextId++, {}, 0}); }
  /// A pop with nothing pushed changes nothing, as for Clang.
  void pop() {
    if (Levels.size() > 1)
      Levels.pop_back();
  }

  /// The command line's level first; a push adds one.
  std::vector<Level> Levels{Level{0, {}, 0}};
  unsigned NextId = 1;
};

/// What decides which diagnostics a header prints where it is included:
/// \p FlagsHash (warningFlagsHash) and the warning pragmas in force there,
/// \p Pragmas or none.
uint64_t warningState(uint64_t FlagsHash, const WarningPragmaTracker *Pragmas);

/// Applies the difference between \p Entry and \p Exit to \p S, reproducing
/// what a textual include of the header would have left behind. Called after
/// a stored result is loaded, since loading does not replay a pragma the
/// header set and did not pop.
void applyPragmaDelta(Sema &S, const PragmaSnapshot &Entry,
                      const PragmaSnapshot &Exit);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_PRAGMASTATECHECK_H
