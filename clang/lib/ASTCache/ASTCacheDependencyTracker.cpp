//===- ASTCacheDependencyTracker.cpp ---------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"

using namespace clang;
using namespace clang::ast_cache;

ASTCacheDependencyTracker::ASTCacheDependencyTracker(Preprocessor &PP)
    : PP(PP) {}

void ASTCacheDependencyTracker::FileChanged(SourceLocation Loc,
                                             FileChangeReason Reason,
                                             SrcMgr::CharacteristicKind FileType,
                                             FileID PrevFID) {
  SourceManager &SM = PP.getSourceManager();

  if (Reason == EnterFile) {
    FileID FID = SM.getFileID(Loc);

    // Synthetic buffers carry externally supplied state (-D macros live in
    // the predefines buffer), so they must never join a closure: doing so
    // would classify every command-line macro as internal and drop it.
    OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
    if (!FE)
      return;

    // Every open frame's closure covers everything entered beneath it.
    for (Frame &F : Stack)
      F.Closure.insert(FID);

    Frame New;
    New.FID = FID;
    New.Path = FE->getName().str();
    New.Closure.insert(FID);
    Stack.push_back(std::move(New));
    return;
  }

  if (Reason != ExitFile)
    return;

  // PrevFID names the file just left. Only close a frame that matches it --
  // leaving a synthetic buffer produces an ExitFile for a FileID no frame was
  // ever pushed for, and must not pop an unrelated header's frame.
  if (Stack.empty() || Stack.back().FID != PrevFID)
    return;

  closeFrame(Stack.back());
  Stack.pop_back();
}

void ASTCacheDependencyTracker::closeFrame(Frame &F) {
  // A poisoned frame emits nothing. Emitting a partial record would produce
  // an entry with fewer constraints than the compile it was recorded from,
  // which is precisely the shape that validates where it must not.
  if (F.Poisoned)
    return;
  HeaderRecord Rec;
  Rec.HeaderPath = std::move(F.Path);
  Rec.Deps = std::move(F.Deps);
  Completed.push_back(std::move(Rec));
}

void ASTCacheDependencyTracker::finalize() {
  // Innermost first, matching the order ordinary ExitFile closes produce.
  while (!Stack.empty()) {
    closeFrame(Stack.back());
    Stack.pop_back();
  }
}

void ASTCacheDependencyTracker::FileSkipped(
    const FileEntryRef &SkippedFile, const Token &FilenameTok,
    SrcMgr::CharacteristicKind FileType) {
  if (Stack.empty())
    return;

  HeaderSearch &HS = PP.getHeaderSearchInfo();

  // A guard-macro skip is exactly "the file was entered and its #ifndef was
  // immediately false", so it is an ordinary conditional dependency on that
  // macro being defined. #pragma once and #import have no macro to compare,
  // so those record file identity instead.
  const IdentifierInfo *Guard =
      HS.getFileInfo(SkippedFile).getControllingMacro(HS.getExternalLookup());

  for (Frame &F : Stack) {
    if (!F.SeenSkipped.insert(&SkippedFile.getFileEntry()).second)
      continue;

    if (Guard) {
      if (!F.SeenConditional.insert(Guard).second)
        continue;
      ConditionalDependency Dep;
      Dep.MacroName = Guard->getName().str();
      Dep.WasDefined = true;
      F.Deps.Conditional.push_back(std::move(Dep));
      continue;
    }

    InclusionDependency Dep;
    Dep.HeaderPath = SkippedFile.getName().str();
    Dep.Why = HS.hasFileBeenImported(SkippedFile)
                  ? InclusionDependency::Reason::Import
                  : InclusionDependency::Reason::PragmaOnce;
    F.Deps.Inclusion.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::recordConditional(const Token &MacroNameTok,
                                                    const MacroDefinition &MD) {
  const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  if (!II)
    return;

  for (Frame &F : Stack) {
    if (!F.SeenConditional.insert(II).second)
      continue;
    ConditionalDependency Dep;
    Dep.MacroName = II->getName().str();
    Dep.WasDefined = static_cast<bool>(MD);
    F.Deps.Conditional.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::Ifdef(SourceLocation Loc,
                                       const Token &MacroNameTok,
                                       const MacroDefinition &MD) {
  recordConditional(MacroNameTok, MD);
}

void ASTCacheDependencyTracker::Ifndef(SourceLocation Loc,
                                        const Token &MacroNameTok,
                                        const MacroDefinition &MD) {
  recordConditional(MacroNameTok, MD);
}

void ASTCacheDependencyTracker::Defined(const Token &MacroNameTok,
                                         const MacroDefinition &MD,
                                         SourceRange Range) {
  recordConditional(MacroNameTok, MD);
}

void ASTCacheDependencyTracker::Elifdef(SourceLocation Loc,
                                         const Token &MacroNameTok,
                                         const MacroDefinition &MD) {
  recordConditional(MacroNameTok, MD);
}

void ASTCacheDependencyTracker::Elifndef(SourceLocation Loc,
                                          const Token &MacroNameTok,
                                          const MacroDefinition &MD) {
  recordConditional(MacroNameTok, MD);
}

void ASTCacheDependencyTracker::UndefinedIdentifierInDirective(
    const Token &IdentifierTok) {
  // The directive's outcome depended on this identifier having no definition,
  // which is the same fact an #ifdef records -- reuse the same path, with the
  // MacroDefinition absent standing for "was not defined".
  recordConditional(IdentifierTok, MacroDefinition());
}

void ASTCacheDependencyTracker::MacroExpands(const Token &MacroNameTok,
                                              const MacroDefinition &MD,
                                              SourceRange Range,
                                              const MacroArgs *Args) {
  if (Stack.empty())
    return;

  const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  if (!II)
    return;

  const MacroInfo *MI = MD.getMacroInfo();
  if (!MI)
    return;

  // Known, still-open gap: a macro that is currently *undefined* and used in
  // a #if/#elif arithmetic expression ("#if SOME_UNDEFINED > 5") never
  // reaches this callback -- the expression evaluator substitutes 0 without
  // going through macro expansion, and no callback names the identifier.
  // Closing that needs the identifiers DirectiveEvalResult already collects
  // to be surfaced through PPCallbacks::If/Elif.

  FileID DefFID =
      PP.getSourceManager().getFileID(MI->getDefinitionLoc());

  // The same expansion answers the provenance question differently per
  // frame: internal to a frame whose closure contains the definition,
  // external -- and so a dependency -- to every frame nested beneath it.
  std::string Sequence;
  bool Built = false;

  for (Frame &F : Stack) {
    if (F.Closure.contains(DefFID))
      continue;
    if (!F.SeenValue.insert(II).second)
      continue;

    if (!Built) {
      // Shared with the client's match check -- see MacroTokens.h for why
      // these must not be two implementations.
      Sequence = serializeMacroTokens(*MI);
      Built = true;
    }

    ValueDependency Dep;
    Dep.MacroName = II->getName().str();
    Dep.TokenSequence = Sequence;
    F.Deps.Value.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::recordSymbol(
    const IdentifierInfo *Name, SymbolClassification Classification,
    const QueryContext &Ctx) {
  if (Stack.empty() || !Name)
    return;

  SourceManager &SM = PP.getSourceManager();

  // A query with no location cannot be attributed to a header, and guessing
  // would file it against the wrong one. Poison instead of guessing.
  if (Ctx.QueryLoc.isInvalid()) {
    poisonOpenFrames();
    return;
  }

  FileID QueryFID = SM.getFileID(SM.getExpansionLoc(Ctx.QueryLoc));
  bool HaveResolved = Ctx.ResolvedLoc.isValid();
  FileID ResolvedFID =
      HaveResolved ? SM.getFileID(SM.getExpansionLoc(Ctx.ResolvedLoc))
                   : FileID();

  for (Frame &F : Stack) {
    // Only headers whose region actually covers the query depend on it.
    if (!F.Closure.contains(QueryFID))
      continue;
    // Answered from inside this header's own closure: its text pins the
    // answer, so it cannot vary between compiles.
    if (HaveResolved && F.Closure.contains(ResolvedFID))
      continue;
    if (!F.SeenSymbol.insert(Name).second)
      continue;

    SymbolDependency Dep;
    Dep.Name = Name->getName().str();
    Dep.Classification = Classification;
    Dep.ResolvedTypeName = Ctx.ResolvedTypeName.str();
    F.Deps.Symbol.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::recordTypeNameQuery(const IdentifierInfo *Name,
                                                     bool IsType,
                                                     const QueryContext &Ctx) {
  recordSymbol(Name,
               IsType ? SymbolClassification::Type
                      : SymbolClassification::NonType,
               Ctx);
}

void ASTCacheDependencyTracker::recordClassification(
    const IdentifierInfo *Name, const Sema::NameClassification &Result,
    const QueryContext &Ctx) {
  SymbolClassification C;
  switch (Result.getKind()) {
  case NameClassificationKind::Type:
    C = SymbolClassification::Type;
    break;
  case NameClassificationKind::TypeTemplate:
  case NameClassificationKind::VarTemplate:
  case NameClassificationKind::FunctionTemplate:
  case NameClassificationKind::UndeclaredTemplate:
    C = SymbolClassification::Template;
    break;
  case NameClassificationKind::Unknown:
  case NameClassificationKind::Error:
    C = SymbolClassification::Unknown;
    break;
  default:
    C = SymbolClassification::NonType;
    break;
  }
  recordSymbol(Name, C, Ctx);
}

void ASTCacheDependencyTracker::recordTemplateNameQuery(
    const IdentifierInfo *Name, TemplateNameKind Kind) {
  poisonOpenFrames();
}

void ASTCacheDependencyTracker::recordDeductionGuideNameQuery(
    const IdentifierInfo *Name, bool IsDeductionGuide) {
  poisonOpenFrames();
}

void ASTCacheDependencyTracker::poisonOpenFrames() {
  for (Frame &F : Stack)
    F.Poisoned = true;
}

const HeaderDependencySet *
ASTCacheDependencyTracker::findByPath(llvm::StringRef PathSuffix) const {
  for (const HeaderRecord &Rec : Completed)
    if (llvm::StringRef(Rec.HeaderPath).ends_with(PathSuffix))
      return &Rec.Deps;
  return nullptr;
}
