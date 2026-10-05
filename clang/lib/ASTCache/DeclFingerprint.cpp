//===- DeclFingerprint.cpp --------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/DeclFingerprint.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// "defined <hash>" or "declared", for a struct, union or enum.
void describeTag(llvm::raw_ostream &OS, const TagDecl &T) {
  if (auto *R = dyn_cast<RecordDecl>(&T)) {
    if (RecordDecl *Def = R->getDefinition())
      OS << "defined " << Def->getODRHash();
    else
      OS << "declared";
    return;
  }
  if (auto *E = dyn_cast<EnumDecl>(&T)) {
    if (EnumDecl *Def = E->getDefinition())
      OS << "defined " << Def->getODRHash();
    else
      OS << "declared";
    return;
  }
  OS << "declared";
}

} // namespace

std::optional<std::string>
clang::ast_cache::declarationFingerprint(const NamedDecl &D) {
  if (!D.getDeclName().isIdentifier() || D.getName().empty() ||
      !D.getDeclContext()->getRedeclContext()->isFileContext())
    return std::nullopt;
  std::string S;
  llvm::raw_string_ostream OS(S);

  if (auto *TD = dyn_cast<TypedefNameDecl>(&D)) {
    QualType T = TD->getUnderlyingType();
    OS << "typedef " << T.getCanonicalType().getAsString();
    if (const TagDecl *Tag = T->getAsTagDecl()) {
      OS << " ";
      describeTag(OS, *Tag);
    }
    return S;
  }
  if (auto *Tag = dyn_cast<TagDecl>(&D)) {
    OS << Tag->getKindName() << " ";
    describeTag(OS, *Tag);
    return S;
  }
  if (auto *VD = dyn_cast<ValueDecl>(&D)) {
    OS << (isa<FunctionDecl>(VD) ? "function " : "variable ")
       << VD->getType().getCanonicalType().getAsString();
    // The most recent declaration carries every asm label and definition
    // seen so far.
    const Decl *Latest = VD->getMostRecentDecl();
    if (const auto *A = Latest->getAttr<AsmLabelAttr>())
      OS << " asm(" << A->getLabel() << ")";
    if (auto *F = dyn_cast<FunctionDecl>(VD)) {
      const FunctionDecl *Def = nullptr;
      if (F->isDefined(Def))
        OS << " defined " << const_cast<FunctionDecl *>(Def)->getODRHash();
      else
        OS << " declared";
    } else if (auto *V = dyn_cast<VarDecl>(VD)) {
      OS << (V->getDefinition() ? " defined" : " declared");
    }
    return S;
  }
  return std::nullopt;
}

DeclarationDependency::Space clang::ast_cache::nameSpaceOf(const NamedDecl &D) {
  return isa<TagDecl>(D) ? DeclarationDependency::Space::Tag
                         : DeclarationDependency::Space::Ordinary;
}

bool clang::ast_cache::fingerprintSatisfies(llvm::StringRef Carried,
                                            llvm::StringRef Live) {
  if (Carried == Live)
    return true;
  // A carried declaration, not a definition, merges with a live definition
  // of the same kind and type: everything up to " declared" must match.
  if (!Carried.consume_back(" declared"))
    return false;
  size_t Defined = Live.rfind(" defined");
  return Defined != llvm::StringRef::npos &&
         Live.take_front(Defined) == Carried;
}
