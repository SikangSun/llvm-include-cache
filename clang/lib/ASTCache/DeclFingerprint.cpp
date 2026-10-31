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
#include <cstring>
#include <tuple>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// "defined <hash> layout <size>/<align>@<field offsets>" or "declared", for
/// a struct or union; "defined <hash>" or "declared" for an enum. The ODR
/// hash covers the members, but not attributes such as packed and aligned;
/// the layout covers what those change. A part's sizeof of the struct is a
/// constant the exporting compile computed.
void describeTag(llvm::raw_ostream &OS, const TagDecl &T,
                 llvm::function_ref<bool(const Decl *)> Counts,
                 LayoutPart Layout) {
  TagDecl *Def = nullptr;
  for (TagDecl *R : T.redecls())
    if (R->isCompleteDefinition() && (!Counts || Counts(R))) {
      Def = R;
      break;
    }
  if (auto *R = dyn_cast_or_null<RecordDecl>(Def)) {
    OS << "defined " << R->getODRHash();
    if (R->isInvalidDecl() || Layout == LayoutPart::Never ||
        (Layout == LayoutPart::IfComputed &&
         !R->getASTContext().hasComputedRecordLayout(R)))
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
    const NamedDecl &D, llvm::function_ref<bool(const Decl *)> Counts,
    LayoutPart Layout, const llvm::DenseSet<const IdentifierInfo *> *Masked) {
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
      describeTag(OS, *Tag, Counts, Layout);
    }
    return S;
  }
  if (auto *Tag = dyn_cast<TagDecl>(&D)) {
    OS << Tag->getKindName() << " ";
    describeTag(OS, *Tag, Counts, Layout);
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
      // The parser reads one token ahead, so an #include right after a
      // function's closing brace is entered before Sema attaches the body
      // (Parser::ParsingDeclGroupRAII). FunctionDecl::getODRHash would hash
      // the definition without its body and keep that hash for the rest of
      // the compile, for every copy written after.
      if (Def && Def->willHaveBody()) {
        OS << " defined pending";
      } else if (Def) {
        unsigned Hash = Def->getODRHash();
        OS << " defined " << Hash;
        if (Masked && !Masked->empty()) {
          auto IsMasked = [&](const IdentifierInfo *II) {
            return Masked->contains(II);
          };
          ODRHash M;
          M.setMaskedIdentifiers(IsMasked);
          M.AddFunctionDecl(Def);
          if (unsigned MaskedHash = M.CalculateHash(); MaskedHash != Hash)
            OS << " masked " << MaskedHash;
        }
      } else {
        OS << " declared";
      }
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

bool clang::ast_cache::hasLayout(llvm::StringRef F) {
  return F.contains(" layout ");
}

namespace {
/// "<rest> masked <hash>" as the rest and the hash; no hash if there is none.
std::pair<llvm::StringRef, llvm::StringRef> splitMasked(llvm::StringRef F) {
  size_t At = F.rfind(" masked ");
  if (At == llvm::StringRef::npos)
    return {F, {}};
  return {F.take_front(At), F.drop_front(At + strlen(" masked "))};
}
} // namespace

bool clang::ast_cache::fingerprintSatisfies(llvm::StringRef Carried,
                                            llvm::StringRef Live) {
  if (Carried == Live)
    return true;
  llvm::StringRef CarriedMasked, LiveMasked;
  std::tie(Carried, CarriedMasked) = splitMasked(Carried);
  std::tie(Live, LiveMasked) = splitMasked(Live);
  if (Carried == Live)
    return true;
  // Two function definitions that differ only in names made from
  // __COUNTER__ values. A definition without a masked hash uses none.
  if (!CarriedMasked.empty()) {
    size_t C = Carried.rfind(" defined "), L = Live.rfind(" defined ");
    if (C != llvm::StringRef::npos && L != llvm::StringRef::npos &&
        Carried.take_front(C) == Live.take_front(L) &&
        CarriedMasked == (LiveMasked.empty()
                              ? Live.drop_front(L + strlen(" defined "))
                              : LiveMasked))
      return true;
  }
  // A carried declaration, not a definition, merges with a live definition
  // of the same kind and type: everything up to " declared" must match.
  if (!Carried.consume_back(" declared"))
    return false;
  size_t Defined = Live.rfind(" defined");
  return Defined != llvm::StringRef::npos &&
         Live.take_front(Defined) == Carried;
}
