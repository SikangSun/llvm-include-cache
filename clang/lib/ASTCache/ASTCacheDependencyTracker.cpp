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
    if (FE && PendingFile == &FE->getFileEntry())
      for (auto [Frame, Entry] : PendingEntries)
        Stack[Frame].EntryOf[FID] = Entry;
    PendingFile = nullptr;
    PendingEntries.clear();
    if (!FE)
      return;

    // Every open frame's closure covers everything entered beneath it.
    for (Frame &F : Stack) {
      F.Closure.insert(FID);
      F.ClosureFiles.insert(&FE->getFileEntry());
    }

    Frame New;
    New.FID = FID;
    New.Path = FE->getName().str();
    New.Closure.insert(FID);
    New.ClosureFiles.insert(&FE->getFileEntry());
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

void ASTCacheDependencyTracker::InclusionDirective(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FilenameRange, OptionalFileEntryRef File,
    StringRef SearchPath, StringRef RelativePath, const Module *SuggestedModule,
    bool ModuleImported, SrcMgr::CharacteristicKind FileType) {
  PendingFile = nullptr;
  PendingEntries.clear();
  // Not found is an error, and a compile with an error saves nothing.
  if (!File)
    return;
  const IdentifierInfo *Directive = IncludeTok.getIdentifierInfo();
  bool Next = Directive && Directive->getPPKeywordID() == tok::pp_include_next;
  recordLookup(HashLoc,
               Next ? IncludeDependency::Kind::IncludeNext
                    : IncludeDependency::Kind::Include,
               IsAngled, FileName, File);
  PendingFile = &File->getFileEntry();
}

void ASTCacheDependencyTracker::HasInclude(
    SourceLocation Loc, StringRef FileName, bool IsAngled,
    OptionalFileEntryRef File, SrcMgr::CharacteristicKind FileType) {
  bool Next = NextHasIncludeIsNext;
  NextHasIncludeIsNext = false;
  recordLookup(Loc,
               Next ? IncludeDependency::Kind::HasIncludeNext
                    : IncludeDependency::Kind::HasInclude,
               IsAngled, FileName, File);
  PendingEntries.clear();
}

void ASTCacheDependencyTracker::recordLookup(SourceLocation Loc,
                                             IncludeDependency::Kind K,
                                             bool Angled, StringRef Spelled,
                                             OptionalFileEntryRef Found) {
  SourceManager &SM = PP.getSourceManager();
  FileID Where = SM.getFileID(SM.getExpansionLoc(Loc));
  std::string FoundPath;
  if (Found)
    FoundPath = PP.getFileManager().getCanonicalName(*Found).str();

  for (size_t I = 0; I != Stack.size(); ++I) {
    Frame &F = Stack[I];
    if (!F.Closure.contains(Where))
      continue;
    // With -fms-compatibility a quoted lookup also searches beside every file
    // on the include stack, which a hit's lookup from one includer does not
    // reproduce.
    if (!Angled && PP.getLangOpts().MSVCCompat) {
      F.Poisoned = true;
      continue;
    }
    IncludeDependency Dep;
    Dep.K = K;
    Dep.Angled = Angled;
    Dep.Spelled = Spelled.str();
    Dep.Found = FoundPath;
    if (Where != F.FID) {
      auto It = F.EntryOf.find(Where);
      // A file in the region that no recorded lookup entered.
      if (It == F.EntryOf.end()) {
        F.Poisoned = true;
        continue;
      }
      Dep.Includer = It->second;
    }
    F.Deps.Includes.push_back(std::move(Dep));
    PendingEntries.push_back({I, int32_t(F.Deps.Includes.size() - 1)});
  }
}

void ASTCacheDependencyTracker::FileSkipped(
    const FileEntryRef &SkippedFile, const Token &FilenameTok,
    SrcMgr::CharacteristicKind FileType) {
  PendingFile = nullptr;
  PendingEntries.clear();
  if (Stack.empty())
    return;

  HeaderSearch &HS = PP.getHeaderSearchInfo();

  // A guard-macro skip is exactly "the file was entered and its #ifndef was
  // immediately false", so it is an ordinary conditional dependency on that
  // macro being defined. #pragma once and #import have no macro to compare,
  // so those record file identity instead.
  const IdentifierInfo *Guard =
      HS.getFileInfo(SkippedFile).getControllingMacro(HS.getExternalLookup());
  FileID GuardOrigin =
      Guard ? stateOrigin(Guard, PP.getMacroDefinition(Guard)) : FileID();

  for (Frame &F : Stack) {
    if (!F.SeenSkipped.insert(&SkippedFile.getFileEntry()).second)
      continue;

    if (Guard) {
      if (!F.SeenConditional.insert(Guard).second)
        continue;
      // The file was read earlier inside this header, which defined the
      // guard itself: stdio.h reaching <features.h> a second time.
      if (GuardOrigin.isValid() && F.Closure.contains(GuardOrigin))
        continue;
      ConditionalDependency Dep;
      Dep.MacroName = Guard->getName().str();
      Dep.WasDefined = true;
      F.Deps.Conditional.push_back(std::move(Dep));
      continue;
    }

    // Likewise for a #pragma once file this header itself read earlier.
    if (F.ClosureFiles.contains(&SkippedFile.getFileEntry()))
      continue;

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

  FileID Origin = stateOrigin(II, MD);
  for (Frame &F : Stack) {
    // The first test in a frame decides, as for syntactic dependencies: a
    // state the frame's own text set before testing it stays pinned however
    // often it is tested again.
    if (!F.SeenConditional.insert(II).second)
      continue;
    if (Origin.isValid() && F.Closure.contains(Origin))
      continue;
    ConditionalDependency Dep;
    Dep.MacroName = II->getName().str();
    Dep.WasDefined = static_cast<bool>(MD);
    F.Deps.Conditional.push_back(std::move(Dep));
  }
}

FileID ASTCacheDependencyTracker::stateOrigin(const IdentifierInfo *II,
                                              const MacroDefinition &MD) const {
  SourceLocation Loc;
  if (const MacroInfo *MI = MD.getMacroInfo())
    Loc = MI->getDefinitionLoc();
  else if (const MacroDirective *Latest = PP.getLocalMacroDirectiveHistory(II))
    if (isa<UndefMacroDirective>(Latest))
      Loc = Latest->getLocation();
  return Loc.isValid() ? PP.getSourceManager().getFileID(Loc) : FileID();
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

void ASTCacheDependencyTracker::expansionCandidate(const Token &Identifier) {
  if (Stack.empty())
    return;
  const IdentifierInfo *II = Identifier.getIdentifierInfo();
  if (!II)
    return;
  // The common case: a name already seen. If the innermost frame has it,
  // every enclosing frame has it too.
  if (Stack.back().SeenSyntactic.contains(II))
    return;
  // This runs just before HandleIdentifier would update an identifier that a
  // module load left out of date, so do that here first, or the macro state
  // read below could be stale.
  PP.refreshIdentifier(*II);
  recordSyntactic(II, PP.getMacroDefinition(II).getMacroInfo());
}

void ASTCacheDependencyTracker::MacroExpands(const Token &MacroNameTok,
                                             const MacroDefinition &MD,
                                             SourceRange Range,
                                             const MacroArgs *Args) {
  const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  const MacroInfo *MI = MD.getMacroInfo();
  if (Stack.empty() || !II || !MI)
    return;
  if (MI->isBuiltinMacro()) {
    if (II->isStr("__FILE__") || II->isStr("__FILE_NAME__"))
      recordFileName(MacroNameTok.getLocation(), II->isStr("__FILE_NAME__"));
    else if (II->isStr("__has_include_next"))
      NextHasIncludeIsNext = true;
    else if (II->isStr("__has_include"))
      NextHasIncludeIsNext = false;
  }
  recordSyntactic(II, MI);
}

void ASTCacheDependencyTracker::recordFileName(SourceLocation Loc,
                                               bool LastComponentOnly) {
  SourceManager &SM = PP.getSourceManager();
  // The same lookup Preprocessor::ExpandBuiltinMacro makes: the file is the
  // one the outermost macro use is written in.
  PresumedLoc PLoc = SM.getPresumedLoc(Loc);
  if (PLoc.isInvalid())
    return;
  FileID FID = SM.getFileID(SM.getExpansionLoc(Loc));
  OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
  // A #line in the file's own text named it, and every compile reads that
  // #line the same way.
  if (!FE || llvm::StringRef(PLoc.getFilename()) != FE->getName())
    return;

  std::string Text = fileMacroText(PLoc.getFilename(), LastComponentOnly,
                                   PP.getLangOpts(), PP.getTargetInfo());
  for (Frame &F : Stack) {
    if (!F.Closure.contains(FID))
      continue;
    int32_t File = -1;
    if (FID != F.FID) {
      auto It = F.EntryOf.find(FID);
      if (It == F.EntryOf.end()) {
        F.Poisoned = true;
        continue;
      }
      File = It->second;
    }
    std::string Key = (LastComponentOnly ? "N" : "F") + std::to_string(File);
    if (!F.SeenFileNames.insert(Key).second)
      continue;
    FileNameDependency Dep;
    Dep.Which = LastComponentOnly ? FileNameDependency::Macro::FileName
                                  : FileNameDependency::Macro::File;
    Dep.File = File;
    Dep.Text = Text;
    F.Deps.FileNames.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::recordSyntactic(const IdentifierInfo *II,
                                                const MacroInfo *MI) {
  FileID DefFID =
      MI ? PP.getSourceManager().getFileID(MI->getDefinitionLoc()) : FileID();

  // The same sighting answers differently per frame: a macro is internal to
  // a frame whose closure contains its definition, and external -- so a
  // dependency -- to every frame nested beneath it.
  std::optional<std::string> Sequence;
  bool Built = false;

  for (Frame &F : Stack) {
    if (!F.SeenSyntactic.insert(II).second)
      continue;
    if (MI && F.Closure.contains(DefFID))
      continue;

    SyntacticDependency Dep;
    Dep.Name = II->getName().str();
    if (MI) {
      if (!Built) {
        // Shared with the client's match check -- see MacroTokens.h for why
        // these must not be two implementations.
        Sequence =
            serializeMacroTokens(*MI, PP.getSourceManager(), PP.getLangOpts());
        Built = true;
      }
      // A definition whose text cannot be read cannot be checked later, so
      // this header cannot be saved.
      if (!Sequence) {
        F.Poisoned = true;
        continue;
      }
      Dep.Required = SyntacticDependency::State::ExpandsTo;
      Dep.TokenSequence = *Sequence;
    }
    F.Deps.Syntactic.push_back(std::move(Dep));
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
