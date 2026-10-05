//===- DeclFingerprint.h ---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What must be equal for a declaration a header's part carries to merge with
// a loading compile's own declaration of the same name (DeclarationDependency).
// Shared, like MacroTokens.h, by the compile that exports the part and the
// client that checks a hit, which must not compute it two ways.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_DECLFINGERPRINT_H
#define LLVM_CLANG_ASTCACHE_DECLFINGERPRINT_H

#include "clang/ASTCache/DependencyRecord.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace clang {
class NamedDecl;

namespace ast_cache {

/// \p D's fingerprint, or std::nullopt for a declaration a part cannot depend
/// on by name: one without a name, or not at file scope.
///
/// The fingerprint says what kind of declaration \p D is and what a merge
/// compares: for a struct, union or enum, whether it is defined and, if so,
/// Clang's ODR hash of the definition; for a typedef, the canonical type it
/// names, with the definition's hash if that is a struct, union or enum; for
/// a function or variable, its canonical type and asm label, and whether this
/// compile has a definition of it with its ODR hash.
std::optional<std::string> declarationFingerprint(const NamedDecl &D);

/// The C name space \p D's name is looked up in.
DeclarationDependency::Space nameSpaceOf(const NamedDecl &D);

/// Whether a live declaration with fingerprint \p Live satisfies a part that
/// carries a copy fingerprinted \p Carried. Equal fingerprints always do. A
/// part that carries only a declaration of a struct, union, enum, function or
/// variable is also satisfied by a definition, since a declaration merges
/// with one; a part that carries a definition needs the same definition.
bool fingerprintSatisfies(llvm::StringRef Carried, llvm::StringRef Live);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DECLFINGERPRINT_H
