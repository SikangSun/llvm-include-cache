//===- MacroStateDigest.h --------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CACHEOBSERVER_MACROSTATEDIGEST_H
#define LLVM_CLANG_CACHEOBSERVER_MACROSTATEDIGEST_H

#include "llvm/Support/MD5.h"

namespace clang {
class Preprocessor;

namespace cache_observer {

/// Computes a deterministic digest of a Preprocessor's currently-defined
/// macro table. The digest only depends on macro names and the token
/// sequence of their replacement lists (sorted by name first, since the
/// Preprocessor's internal macro map has unspecified iteration order), so
/// two preprocessors with the same active #defines always produce the same
/// digest regardless of process/memory layout.
///
/// Deliberately does not use llvm::hash_value/hash_combine: those are
/// seeded per-process to resist hash-flooding and would produce a
/// different digest for the same macro state on every compiler
/// invocation, which is unusable as a cache key.
///
/// Recomputation is lazy: callers observing MacroDefined/MacroUndefined
/// should call invalidate(); snapshot() only re-hashes the whole macro
/// table the next time it is actually needed.
class MacroStateDigest {
public:
  explicit MacroStateDigest(Preprocessor &PP) : PP(PP) {}

  void invalidate() { Dirty = true; }

  const llvm::MD5::MD5Result &snapshot();

private:
  Preprocessor &PP;
  bool Dirty = true;
  llvm::MD5::MD5Result Cached = {};
};

} // namespace cache_observer
} // namespace clang

#endif // LLVM_CLANG_CACHEOBSERVER_MACROSTATEDIGEST_H
