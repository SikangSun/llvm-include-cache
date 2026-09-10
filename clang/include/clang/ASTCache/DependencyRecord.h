//===- DependencyRecord.h --------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The data model for a header's AST-cache dependency set: the pieces of
// external state a cached AST for that header actually depends on.
//
// A macro (or, for SymbolDependency, an ordinary identifier) is only a
// dependency at all if its meaning could vary depending on state outside
// the header's own transitive-include closure -- something defined and
// used entirely within the header's own (stable, unchanging-during-a-
// compile) text is never a dependency, no matter how "structurally
// significant" the position it's used in looks. See
// ASTCacheDependencyTracker.h for how these are actually collected.
//
// Deliberately has no dependency on clang::Sema/clang::NameClassification
// (a heavy header) -- SymbolDependency::Classification mirrors
// clang::NameClassificationKind's small integer values without including
// Sema.h; the one place that needs the real enum casts at the point of use.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_DEPENDENCYRECORD_H
#define LLVM_CLANG_ASTCACHE_DEPENDENCYRECORD_H

#include <cstdint>
#include <string>
#include <vector>

namespace clang {
namespace ast_cache {

/// A macro's *definedness* gates a branch (#ifdef/#ifndef/#if defined()).
/// A hit requires the live macro's defined-ness to match WasDefined.
struct ConditionalDependency {
  std::string MacroName;
  bool WasDefined = false;
};

/// A macro's *expansion* substitutes into the cached Decl graph (a type
/// name, an array bound, an attribute argument, a template argument --
/// deliberately not filtered by grammar position, see
/// ASTCacheDependencyTracker). A hit requires the live macro to expand to
/// the exact same token sequence.
struct ValueDependency {
  std::string MacroName;
  /// The macro's replacement-list tokens' spellings, concatenated with a
  /// separator that cannot appear in a single spelling, so two different
  /// token sequences never collide into the same recorded string.
  std::string TokenSequence;
};

/// An identifier's grammar-ambiguity classification (is it a type-name,
/// and if so which one) was relied on while parsing this header. Mirrors
/// clang::NameClassificationKind's cases relevant to this check; see
/// ASTCacheSymbolDependencyTracker.h for the real enum this is cast from.
enum class SymbolClassification : uint8_t {
  Unknown = 0,
  Type = 1,
  NonType = 2,
  Template = 3,
};

struct SymbolDependency {
  std::string Name;
  SymbolClassification Classification = SymbolClassification::Unknown;
  /// For Classification == Type, a stable textual spelling of the
  /// resolved type (e.g. its canonical name), so a hit can check "still
  /// resolves to the same type," not just "still resolves to some type."
  std::string ResolvedTypeName;
};

/// An #include inside the header was suppressed by Clang's multiple-include
/// optimization, so the file was never opened and none of its own directives
/// fired. Where the suppression came from a guard macro it reduces to an
/// ordinary ConditionalDependency on that macro, and is recorded as one --
/// skipping is semantically just "entered the file and its #ifndef was
/// immediately false." #pragma once and #import have no controlling macro, so
/// there is no macro state to compare and the dependency is on file identity
/// instead: a hit requires that this file has likewise already been included.
struct InclusionDependency {
  std::string HeaderPath;
  enum class Reason : uint8_t { PragmaOnce, Import };
  Reason Why = Reason::PragmaOnce;
};

/// The pragma state that was active at a single point, reduced to the values
/// that actually reach a Decl. Only the small-value pragmas are represented:
/// a header reached under one of the others (#pragma clang attribute push,
/// #pragma GCC visibility) is declined outright rather than described here,
/// since their state is a parsed attribute or a private stack type and is
/// neither comparable nor replayable.
struct PragmaSnapshot {
  /// Sema::AlignPackInfo::getRawEncoding of the current #pragma pack/align
  /// value. Zero is not the default -- the default is whatever the invocation
  /// established -- so this is only ever compared, never interpreted.
  uint32_t AlignPackRaw = 0;
  /// FPOptionsOverride::getAsOpaqueInt of the current #pragma float_control
  /// state.
  uint64_t FpPragmaRaw = 0;

  friend bool operator==(const PragmaSnapshot &A, const PragmaSnapshot &B) {
    return A.AlignPackRaw == B.AlignPackRaw && A.FpPragmaRaw == B.FpPragmaRaw;
  }
  friend bool operator!=(const PragmaSnapshot &A, const PragmaSnapshot &B) {
    return !(A == B);
  }
};

struct HeaderDependencySet {
  std::vector<ConditionalDependency> Conditional;
  std::vector<ValueDependency> Value;
  std::vector<SymbolDependency> Symbol;
  std::vector<InclusionDependency> Inclusion;

  /// Pragma state where the header was reached. This is an *input* to the
  /// header's compilation -- "#pragma pack(1)" before an #include changes the
  /// layout of every record inside it, with no macro tested and none expanded
  /// -- so it is part of what a hit must match.
  PragmaSnapshot Entry;

  /// Pragma state the header left behind. This is an *output*: included
  /// textually, a header that sets pack without popping leaves its includer
  /// packed, and loading a stored result does not replay that. The client
  /// applies the difference from Entry after loading.
  PragmaSnapshot Exit;
};

/// One header's dependency set, as observed over the region between entering
/// that header and leaving it.
struct HeaderRecord {
  std::string HeaderPath;
  HeaderDependencySet Deps;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DEPENDENCYRECORD_H
