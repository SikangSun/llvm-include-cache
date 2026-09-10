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

namespace clang {
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

/// Applies the difference between \p Entry and \p Exit to \p S, reproducing
/// what a textual include of the header would have left behind. Called after
/// a stored result is loaded, since loading does not replay a pragma the
/// header set and did not pop.
void applyPragmaDelta(Sema &S, const PragmaSnapshot &Entry,
                      const PragmaSnapshot &Exit);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_PRAGMASTATECHECK_H
