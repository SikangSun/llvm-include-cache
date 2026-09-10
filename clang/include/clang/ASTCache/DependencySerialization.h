//===- DependencySerialization.h --------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A byte encoding for HeaderDependencySet, so a header's dependency set can
// travel inside the same AST file as the Decl graph it constrains (see
// ASTCacheModuleFileExtension.h).
//
// Deliberately a flat, self-describing blob rather than a bitstream record
// per dependency: the set is read as a unit at validation time and never
// queried piecewise, and keeping it independent of llvm::BitstreamWriter
// lets the encoding be tested on its own.
//
// Every string is length-prefixed rather than NUL-terminated, because
// ValueDependency::TokenSequence contains embedded NULs by construction --
// they are its token separator.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H
#define LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H

#include "clang/ASTCache/DependencyRecord.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace clang {
namespace ast_cache {

/// Bumped whenever the encoding below changes shape. A reader that sees a
/// version it does not know declines the whole block rather than guessing:
/// a misparsed dependency set is a dependency set with the wrong contents,
/// which is exactly the failure mode that validates where it must not.
constexpr uint32_t DependencyEncodingVersion = 2;

/// Appends the encoding of \p Deps to \p Out.
void encodeDependencySet(const HeaderDependencySet &Deps,
                         llvm::SmallVectorImpl<char> &Out);

/// Decodes what encodeDependencySet produced. Returns std::nullopt on a
/// version mismatch, a truncated buffer, or any length that runs past the
/// end -- never a partially populated set.
std::optional<HeaderDependencySet> decodeDependencySet(llvm::StringRef Data);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H
