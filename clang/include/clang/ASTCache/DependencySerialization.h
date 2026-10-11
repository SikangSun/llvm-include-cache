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
// SyntacticDependency::TokenSequence contains embedded NULs by construction --
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
constexpr uint32_t DependencyEncodingVersion = 9;

/// Appends the encoding of \p Deps to \p Out.
void encodeDependencySet(const HeaderDependencySet &Deps,
                         llvm::SmallVectorImpl<char> &Out);

/// Decodes what encodeDependencySet produced. Returns std::nullopt on a
/// version mismatch, a truncated buffer, or any length that runs past the
/// end -- never a partially populated set.
std::optional<HeaderDependencySet> decodeDependencySet(llvm::StringRef Data);

/// Removes every NotAMacro syntactic dependency from \p Deps and returns their
/// names as one list: each name followed by a NUL. These are the bulk of a
/// record -- several hundred for a system header -- so the daemon keeps them
/// in the shared region and a lookup reply carries only where they are.
std::string extractNotAMacroNames(HeaderDependencySet &Deps);

/// The names in a list built by extractNotAMacroNames. std::nullopt for a list
/// that does not end in a NUL or holds an empty name, so a damaged list cannot
/// pass as a shorter one.
std::optional<llvm::SmallVector<llvm::StringRef, 0>>
splitNameList(llvm::StringRef List);

/// Removes the macro effects from \p Deps and returns them as one list in
/// splitNameList's form, each name prefixed with '+' (defined at the end of
/// the header) or '-' (undefined). A header that pulls in a system header
/// touches thousands of macros, and they are needed only on a hit, so the
/// daemon keeps them in the shared region like the NotAMacro names.
std::string extractMacroEffects(HeaderDependencySet &Deps);

/// The effects in a list built by extractMacroEffects, or std::nullopt for a
/// damaged list.
std::optional<std::vector<MacroEffect>>
decodeMacroEffects(llvm::StringRef List);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H
