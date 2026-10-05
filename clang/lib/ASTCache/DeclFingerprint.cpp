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
#include "clang/AST/ODRHash.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// "defined <hash> layout <size>/<align>@<field offsets>" or "declared", for
/// a struct or union; "defined <hash>" or "declared" for an enum. The ODR
/// hash covers the members, but not attributes such as packed and aligned;
/// the layout covers what those change. A part's sizeof of the struct is a
/// constant the exporting compile computed.
void describeTag(llvm::raw_ostream &OS, const TagDecl &T,
                 llvm::function_ref<bool(const Decl *)> Counts) {
  TagDecl *Def = nullptr;
  for (TagDecl *R : T.redecls())
    if (R->isCompleteDefinition() && (!Counts || Counts(R))) {
      Def = R;
      break;
    }
  if (auto *R = dyn_cast_or_null<RecordDecl>(Def)) {
    OS << "defined " << R->getODRHash();
    if (R->isInvalidDecl())
      return;
    const ASTRecordLayout &L = R->getASTContext().getASTRecordLayout(R);
    OS << " layout " << L.getSize().getQuantity() << "/"
       << L.getAlignment().getQuantity() << "@";
    for (unsigned I = 0, N = L.getFieldCount(); I != N; ++I)
      OS << (I ? "," : "") << L.getFieldOffset(I);
    return;
  }
  if (auto *E = dyn_cast_or_null<EnumDecl>(Def)) {
    OS << "defined " << E->getODRHash();
    return;
  }
  OS << "declared";
}

/// Whether \p D is declared at file scope: in the translation unit, or in
/// a struct, union or enum there, whose tags and enumerators C puts in the
/// enclosing scope.
bool atFileScope(const NamedDecl &D) {
  for (const DeclContext *DC = D.getDeclContext();; DC = DC->getParent()) {
    if (DC->isTranslationUnit())
      return true;
    if (!isa<TagDecl>(DC))
      return false;
  }
}

} // namespace

std::optional<std::string> clang::ast_cache::declarationFingerprint(
    const NamedDecl &D, llvm::function_ref<bool(const Decl *)> Counts) {
  if (!D.getDeclName().isIdentifier() || D.getName().empty() || !atFileScope(D))
    return std::nullopt;
  std::string S;
  llvm::raw_string_ostream OS(S);

  if (auto *TD = dyn_cast<TypedefNameDecl>(&D)) {
    QualType T = TD->getUnderlyingType();
    OS << "typedef " << T.getCanonicalType().getAsString();
    // An aligned attribute on the typedef changes the layout of what uses
    // it, and the canonical type does not show it.
    if (unsigned Align = TD->getMaxAlignment())
      OS << " aligned " << Align;
    if (const TagDecl *Tag = T->getAsTagDecl()) {
      OS << " ";
      describeTag(OS, *Tag, Counts);
    }
    return S;
  }
  if (auto *Tag = dyn_cast<TagDecl>(&D)) {
    OS << Tag->getKindName() << " ";
    describeTag(OS, *Tag, Counts);
    return S;
  }
  if (auto *EC = dyn_cast<EnumConstantDecl>(&D)) {
    // A part may hold constants computed from the value, such as an array
    // bound.
    OS << "enumerator " << EC->getType().getCanonicalType().getAsString() << " "
       << toString(EC->getInitVal(), 10);
    return S;
  }
  if (auto *VD = dyn_cast<ValueDecl>(&D)) {
    OS << (isa<FunctionDecl>(VD) ? "function " : "variable ")
       << VD->getType().getCanonicalType().getAsString();
    // The most recent declaration that counts carries every asm label seen
    // so far.
    const Decl *Latest = VD->getMostRecentDecl();
    while (Counts && Latest && !Counts(Latest))
      Latest = Latest->getPreviousDecl();
    if (const auto *A = Latest ? Latest->getAttr<AsmLabelAttr>() : nullptr)
      OS << " asm(" << A->getLabel() << ")";
    if (auto *F = dyn_cast<FunctionDecl>(VD)) {
      FunctionDecl *Def = nullptr;
      for (FunctionDecl *R : F->redecls())
        if (R->isThisDeclarationADefinition() && (!Counts || Counts(R))) {
          Def = R;
          break;
        }
      if (Def)
        OS << " defined " << Def->getODRHash();
      else
        OS << " declared";
    } else if (auto *V = dyn_cast<VarDecl>(VD)) {
      const VarDecl *Def = nullptr;
      for (const VarDecl *R : V->redecls())
        if (R->isThisDeclarationADefinition() == VarDecl::Definition &&
            (!Counts || Counts(R))) {
          Def = R;
          break;
        }
      OS << (Def ? " defined" : " declared");
      // Clang folds a const variable's initializer into constants, such as
      // an array bound, which a part may hold.
      if (const Expr *Init = Def ? Def->getInit() : nullptr) {
        ODRHash Hash;
        Hash.AddStmt(Init);
        OS << " init " << Hash.CalculateHash();
      }
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
