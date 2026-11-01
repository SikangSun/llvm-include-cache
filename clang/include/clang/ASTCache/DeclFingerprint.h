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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace clang {
class Decl;
class IdentifierInfo;
class NamedDecl;

namespace ast_cache {

/// When a struct or union's fingerprint holds its layout.
enum class LayoutPart {
  /// Always, computing the layout if the compile has not.
  Always,
  /// Only if the compile computed the layout already. A part holds a
  /// constant from the layout, such as a sizeof in an array bound, only if
  /// the compile computed the layout while it parsed the part.
  IfComputed,
  /// Never.
  Never,
};

/// \p D's fingerprint, or std::nullopt for a declaration a part cannot depend
/// on by name: one without a name, or not at file scope.
///
/// The fingerprint says what kind of declaration \p D is and what the
/// part's own declarations were compiled against: for a struct, union or
/// enum, whether it is defined and, if so, Clang's ODR hash of the
/// definition, and for a struct or union its layout; for a typedef, the
/// canonical type it names and any aligned attribute, with the description
/// of the struct, union or enum it names; for an enumerator, its type and
/// value; for a function or variable, its canonical type and asm label, and
/// whether this compile has a definition of it, with the function's ODR hash
/// or the hash of the variable's initializer.
///
/// \p Counts, when set, says which redeclarations the fingerprint looks at
/// for a definition or an asm label. The exporting compile passes those made
/// before the header began: a hit checks the loading compile where the
/// header begins, before the part adds its own definition of, say, bsearch.
///
/// \p Layout says when a struct or union's description holds its layout.
///
/// With \p Masked, the names a compile made from __COUNTER__ values, a
/// function definition whose body uses one also gets " masked <hash>": its
/// ODR hash with each such name hashed the same. Two such definitions that
/// differ only in those names do the same thing.
std::optional<std::string> declarationFingerprint(
    const NamedDecl &D, llvm::function_ref<bool(const Decl *)> Counts = nullptr,
    LayoutPart Layout = LayoutPart::Always,
    const llvm::DenseSet<const IdentifierInfo *> *Masked = nullptr);

/// Whether fingerprint \p F holds a layout.
bool hasLayout(llvm::StringRef F);

/// The C name space \p D's name is looked up in.
DeclarationDependency::Space nameSpaceOf(const NamedDecl &D);

/// Whether a live declaration with fingerprint \p Live satisfies a part that
/// carries a copy fingerprinted \p Carried. Equal fingerprints always do. A
/// part that carries only a declaration of a struct, union, enum, function or
/// variable is also satisfied by a definition, since a declaration merges
/// with one; a part that carries a definition needs the same definition, or,
/// for a function with a masked hash, one with the same masked hash.
bool fingerprintSatisfies(llvm::StringRef Carried, llvm::StringRef Live);

/// What Clang's reader compares to merge a declaration it loads with one of
/// the same name the compile has (ASTContext::isSameEntity, for C): the
/// kind and the linkage, and a typedef's type, a tag's kind, an enumerator's
/// value, a function's type as its first declaration wrote it, or a
/// variable's type as \p D has it. Two declarations with different keys
/// stay two. None for a declaration not at file scope or of another kind.
std::optional<std::string> mergeKey(const NamedDecl &D);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DECLFINGERPRINT_H
