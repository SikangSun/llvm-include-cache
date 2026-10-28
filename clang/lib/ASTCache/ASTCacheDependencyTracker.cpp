//===- ASTCacheDependencyTracker.cpp ---------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/STLExtras.h"

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
    if (Measuring || RecordFiles || PiecesBase)
      FirstEntered.try_emplace(&FE->getFileEntry(), FID);
    if (RecordFiles && (!OpenFiles.empty() || (TrackOnly && TrackOnly(FID)))) {
      FileReads R;
      R.FID = FID;
      R.Path = FE->getName().str();
      R.Enter = ++Clock;
      FileIndex[FID] = Files.size();
      OpenFiles.push_back(Files.size());
      Files.push_back(std::move(R));
    }

    // Every open frame's closure covers everything entered beneath it.
    for (Frame &F : Stack) {
      F.Closure.insert(FID);
      F.ClosureFiles.insert(&FE->getFileEntry());
      // A file the base's own lines include starts a piece; one below it
      // is in that piece.
      if (PiecesState *P = F.P.get()) {
        if (P->Depth++ == 0) {
          P->Rec.Pieces.push_back({FID, P->NoPieces});
          P->Cur = P->NoPieces ? -1 : int32_t(P->Rec.Pieces.size() - 1);
        }
        P->Rec.PieceOf[FID] = P->Cur;
      }
    }
    if (TrackOnly && !TrackOnly(FID))
      return;

    Frame New;
    New.FID = FID;
    New.Path = FE->getName().str();
    New.Closure.insert(FID);
    New.ClosureFiles.insert(&FE->getFileEntry());
    if (PiecesBase && PiecesBase(FID))
      New.P = std::make_unique<PiecesState>();
    Stack.push_back(std::move(New));
    return;
  }

  if (Reason != ExitFile)
    return;

  for (Frame &F : Stack)
    if (PiecesState *P = F.P.get(); P && P->Rec.PieceOf.count(PrevFID))
      if (--P->Depth == 0)
        P->Cur = -1;

  if (!OpenFiles.empty() && Files[OpenFiles.back()].FID == PrevFID) {
    Files[OpenFiles.back()].Exit = ++Clock;
    OpenFiles.pop_back();
  }

  // PrevFID names the file just left. Only close a frame that matches it --
  // leaving a synthetic buffer produces an ExitFile for a FileID no frame was
  // ever pushed for, and must not pop an unrelated header's frame.
  if (Stack.empty() || Stack.back().FID != PrevFID)
    return;

  closeFrame(Stack.back());
  Stack.pop_back();
}

void ASTCacheDependencyTracker::closeFrame(Frame &F) {
  if (Measuring) {
    Independence I;
    I.FID = F.FID;
    I.Path = F.Path;
    I.Closure.assign(F.Closure.begin(), F.Closure.end());
    I.ReadFrom = std::move(F.ReadFrom);
    I.AltersFrom = std::move(F.AltersFrom);
    I.Poisoned = F.Poisoned;
    Measured.push_back(std::move(I));
  }
  // A poisoned frame emits nothing. Emitting a partial record would produce
  // an entry with fewer constraints than the compile it was recorded from,
  // which is precisely the shape that validates where it must not.
  if (F.Poisoned)
    return;
  std::unique_ptr<PiecesRecord> Pieces;
  if (PiecesState *P = F.P.get()) {
    // A dependency read in a piece that turned out not to be one is the
    // base's; so is a crossing such a piece provides.
    auto Remap = [&](auto &Deps) {
      for (auto &D : Deps)
        if (D.Cond)
          D.Cond = P->eff(int32_t(D.Cond) - 1) + 1;
    };
    Remap(F.Deps.Conditional);
    Remap(F.Deps.Syntactic);
    Remap(F.Deps.Symbol);
    Remap(F.Deps.Inclusion);
    Remap(F.Deps.Includes);
    Remap(F.Deps.FileNames);
    llvm::erase_if(P->Rec.Crossings, [&](const PiecesRecord::Crossing &C) {
      return P->eff(C.Provider) < 0;
    });
    for (auto &[FID, K] : P->Rec.PieceOf)
      K = P->eff(K);
    for (const auto &[II, S] : P->Names)
      if (S.Changer != -3)
        P->Rec.ChangedBy[II] = P->eff(S.Changer);
    Pieces = std::make_unique<PiecesRecord>(std::move(P->Rec));
  }
  HeaderRecord Rec;
  Rec.HeaderPath = std::move(F.Path);
  Rec.Deps = std::move(F.Deps);
  Completed.push_back(std::move(Rec));
  CompletedFIDs.push_back(F.FID);
  CompletedSites.push_back(std::move(F.At));
  CompletedPieces.push_back(std::move(Pieces));
}

void ASTCacheDependencyTracker::finalize() {
  recordHeldTests();
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
  std::string FoundPath, FoundName;
  if (Found) {
    FoundPath = PP.getFileManager().getCanonicalName(*Found).str();
    FoundName = Found->getName().str();
  }

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
    Dep.Name = FoundName;
    // In a base, the piece holding it. Where the piece is skipped, its files
    // were read already, and only the names they were opened by are
    // compared (matchesLiveState): a skipped piece's declarations can still
    // reach the compile, a type the base's lines name, under those names.
    if (F.P && Where != F.FID)
      Dep.Cond = uint32_t(F.P->Rec.PieceOf.lookup(Where) + 1);
    if (Where != F.FID) {
      auto It = F.EntryOf.find(Where);
      // A file in the region that no recorded lookup entered.
      if (It == F.EntryOf.end()) {
        F.Poisoned = true;
        continue;
      }
      Dep.Includer = It->second;
    }
    if (DumpSites) {
      SiteLoc = Loc;
      site(F, 'n',
           K == IncludeDependency::Kind::Include ||
                   K == IncludeDependency::Kind::IncludeNext
               ? "include"
               : "has_include",
           FoundPath.empty() ? Spelled : StringRef(FoundPath), FileID());
    }
    F.Deps.Includes.push_back(std::move(Dep));
    F.At.Includes.push_back(Where);
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

  // The same, in the file holding the #include (recordFiles).
  if (FileReads *F = currentFile()) {
    const IdentifierInfo *G =
        HS.getFileInfo(SkippedFile).getControllingMacro(HS.getExternalLookup());
    if (G) {
      FileID O = stateOrigin(G, PP.getMacroDefinition(G));
      if (!FilenameTok.getLocation().isMacroID())
        F->Skips.push_back(
            {PP.getFileManager().getCanonicalName(SkippedFile).str(),
             G->getName().str(), O, PP.getSpelling(FilenameTok),
             FilenameTok.getLocation(), ++Clock});
      if (O != F->FID) {
        auto [It, New] = F->Seen->Test.try_emplace(G, true, O);
        if (New || It->second != std::make_pair(true, O)) {
          It->second = {true, O};
          ConditionalDependency D;
          D.MacroName = G->getName().str();
          D.WasDefined = true;
          F->Conditional.push_back({std::move(D), O, ++Clock});
        }
      }
    } else if (F->Seen->Once.insert(&SkippedFile.getFileEntry()).second) {
      FileID O = FirstEntered.lookup(&SkippedFile.getFileEntry());
      InclusionDependency D;
      D.HeaderPath = SkippedFile.getName().str();
      D.Why = HS.hasFileBeenImported(SkippedFile)
                  ? InclusionDependency::Reason::Import
                  : InclusionDependency::Reason::PragmaOnce;
      F->Inclusion.push_back({std::move(D), O, ++Clock});
    }
  }

  // A guard-macro skip is exactly "the file was entered and its #ifndef was
  // immediately false", so it is an ordinary conditional dependency on that
  // macro being defined. #pragma once and #import have no macro to compare,
  // so those record file identity instead.
  const IdentifierInfo *Guard =
      HS.getFileInfo(SkippedFile).getControllingMacro(HS.getExternalLookup());
  FileID GuardOrigin =
      Guard ? stateOrigin(Guard, PP.getMacroDefinition(Guard)) : FileID();

  for (Frame &F : Stack) {
    if (F.P) {
      piecesSkip(F, SkippedFile, FilenameTok, Guard, GuardOrigin);
      continue;
    }
    if (!F.SeenSkipped.insert(&SkippedFile.getFileEntry()).second)
      continue;

    if (Guard) {
      if (!F.SeenConditional.insert(Guard).second)
        continue;
      // The file was read earlier inside this header, which defined the
      // guard itself: stdio.h reaching <features.h> a second time.
      if (GuardOrigin.isValid() && F.holds(GuardOrigin))
        continue;
      noteRead(F, GuardOrigin, "skips " + SkippedFile.getName());
      SiteLoc = FilenameTok.getLocation();
      site(F, 'c', "guard", Guard->getName(), GuardOrigin);
      ConditionalDependency Dep;
      Dep.MacroName = Guard->getName().str();
      Dep.WasDefined = true;
      F.Deps.Conditional.push_back(std::move(Dep));
      continue;
    }

    // Likewise for a #pragma once file this header itself read earlier.
    if (F.ClosureFiles.contains(&SkippedFile.getFileEntry()))
      continue;
    if (Measuring)
      noteRead(F, FirstEntered.lookup(&SkippedFile.getFileEntry()),
               "skips " + SkippedFile.getName());

    InclusionDependency Dep;
    Dep.HeaderPath = SkippedFile.getName().str();
    Dep.Why = HS.hasFileBeenImported(SkippedFile)
                  ? InclusionDependency::Reason::Import
                  : InclusionDependency::Reason::PragmaOnce;
    SiteLoc = FilenameTok.getLocation();
    site(F, 'i', "once", Dep.HeaderPath,
         FirstEntered.lookup(&SkippedFile.getFileEntry()));
    F.Deps.Inclusion.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::recordConditional(const Token &MacroNameTok,
                                                    const MacroDefinition &MD) {
  const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  if (!II)
    return;
  SiteLoc = MacroNameTok.getLocation();
  recordConditional(II, static_cast<bool>(MD), stateOrigin(II, MD));
}

void ASTCacheDependencyTracker::recordConditional(const IdentifierInfo *II,
                                                  bool WasDefined,
                                                  FileID Origin) {
  // The file whose #if this is: its own #define pins the state.
  if (FileReads *F = currentFile(); F && Origin != F->FID) {
    auto [It, New] = F->Seen->Test.try_emplace(II, WasDefined, Origin);
    if (New || It->second != std::make_pair(WasDefined, Origin)) {
      It->second = {WasDefined, Origin};
      ConditionalDependency D;
      D.MacroName = II->getName().str();
      D.WasDefined = WasDefined;
      F->Conditional.push_back({std::move(D), Origin, ++Clock});
    }
  }
  for (Frame &F : Stack) {
    if (F.P) {
      piecesConditional(F, II, WasDefined, Origin);
      continue;
    }
    // The first test in a frame decides, as for syntactic dependencies: a
    // state the frame's own text set before testing it stays pinned however
    // often it is tested again.
    if (!F.SeenConditional.insert(II).second)
      continue;
    if (Origin.isValid() && F.holds(Origin))
      continue;
    noteRead(F, Origin, "tests " + II->getName());
    site(F, 'c', SiteUse, II->getName(), Origin);
    ConditionalDependency Dep;
    Dep.MacroName = II->getName().str();
    Dep.WasDefined = WasDefined;
    F.Deps.Conditional.push_back(std::move(Dep));
  }
}

void ASTCacheDependencyTracker::recordHeldTests() {
  for (const HeldTest &T : Held) {
    SiteLoc = T.Loc;
    recordConditional(T.II, T.WasDefined, T.Origin);
  }
  Held.clear();
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
  const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  if (Stack.empty() || !II)
    return;
  // The state is read now: the expression is still being evaluated, and
  // nothing in it can change a macro.
  Held.push_back(
      {Range.getBegin(), II, static_cast<bool>(MD), stateOrigin(II, MD)});
}

void ASTCacheDependencyTracker::DefinedOperatorsNeeded(
    bool Known, ArrayRef<SourceLocation> Needed) {
  for (const HeldTest &T : Held)
    if (!Known || llvm::is_contained(Needed, T.Loc)) {
      SiteLoc = T.Loc;
      recordConditional(T.II, T.WasDefined, T.Origin);
    }
  Held.clear();
}

void ASTCacheDependencyTracker::If(SourceLocation Loc,
                                   SourceRange ConditionRange,
                                   ConditionValueKind ConditionValue) {
  recordHeldTests();
}

void ASTCacheDependencyTracker::Elif(SourceLocation Loc,
                                     SourceRange ConditionRange,
                                     ConditionValueKind ConditionValue,
                                     SourceLocation IfLoc) {
  recordHeldTests();
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
  SiteUse = "value";
  recordConditional(IdentifierTok, MacroDefinition());
  SiteUse = "test";
}

void ASTCacheDependencyTracker::expansionCandidate(const Token &Identifier) {
  if (Stack.empty())
    return;
  const IdentifierInfo *II = Identifier.getIdentifierInfo();
  if (!II)
    return;
  if (FileReads *F = currentFile()) {
    if (II->isOutOfDate())
      PP.refreshIdentifier(*II);
    const MacroInfo *MI = II->hasMacroDefinition()
                              ? PP.getMacroDefinition(II).getMacroInfo()
                              : nullptr;
    auto It = F->Seen->Macro.find(II);
    if (It == F->Seen->Macro.end() || It->second != MI)
      fileRead(F, II, MI, FileID());
  }
  // The common case: a name already seen. If the innermost frame has it,
  // every enclosing frame has it too. In a base, a name read again by the
  // same group, in the state it read before, records nothing new.
  if (const PiecesState *P = Stack.back().P.get()) {
    if (Stack.size() == 1) {
      auto It = P->Names.find(II);
      if (It != P->Names.end()) {
        const NameState &S = It->second;
        if (S.Syn != -2 && (S.Syn == P->Cur || S.Syn == -1) &&
            (S.Changer == -3 || S.Changer == P->Cur))
          return;
      }
    }
  } else if (Stack.back().SeenSyntactic.contains(II)) {
    return;
  }
  // This runs just before HandleIdentifier would update an identifier that a
  // module load left out of date, so do that here first, or the macro state
  // read below could be stale.
  PP.refreshIdentifier(*II);
  SiteLoc = Identifier.getLocation();
  recordSyntactic(II, PP.getMacroDefinition(II).getMacroInfo());
}

void ASTCacheDependencyTracker::PragmaPushMacro(SourceLocation,
                                                const IdentifierInfo *II,
                                                const MacroInfo *) {
  for (Frame &F : Stack) {
    ++F.MacroPushes[II];
    if (PiecesState *P = F.P.get()) {
      P->NoPieces = true;
      for (PiecesRecord::Piece &Piece : P->Rec.Pieces)
        Piece.Bad = true;
      P->Cur = -1;
    }
  }
}

void ASTCacheDependencyTracker::PragmaPopMacro(SourceLocation Loc,
                                               const IdentifierInfo *II,
                                               const MacroInfo *) {
  // The frames the pop takes from below are the innermost ones: an inner
  // frame's pushes are made inside every frame around it.
  bool TookFromBelow = false;
  for (Frame &F : Stack) {
    auto It = F.MacroPushes.find(II);
    if (It != F.MacroPushes.end() && It->second) {
      --It->second;
      continue;
    }
    if (PiecesState *P = F.P.get()) {
      // Pieces cannot hold a push or pop: the base's lines are one.
      P->NoPieces = true;
      for (PiecesRecord::Piece &Piece : P->Rec.Pieces)
        Piece.Bad = true;
      P->Cur = -1;
      NameState &S = P->Names[II];
      if (S.Syn == -2) {
        S.Syn = -1;
        TookFromBelow = true;
      }
      continue;
    }
    TookFromBelow |= F.SeenSyntactic.insert(II).second;
  }
  // Every frame sees what the frames inside it see (Frame::SeenSyntactic):
  // the frames whose own push the pop took see the name now, at the
  // definition their push saved.
  if (TookFromBelow) {
    PP.refreshIdentifier(*II);
    SiteLoc = Loc;
    recordSyntactic(II, PP.getMacroDefinition(II).getMacroInfo());
  }
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
  SiteLoc = MacroNameTok.getLocation();
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
    if (F.P && FID != F.FID)
      Dep.Cond = uint32_t(F.P->Rec.PieceOf.lookup(FID) + 1);
    F.Deps.FileNames.push_back(std::move(Dep));
    F.At.FileNames.push_back(FID);
  }
}

void ASTCacheDependencyTracker::fileRead(FileReads *F,
                                         const IdentifierInfo *II,
                                         const MacroInfo *MI, FileID) {
  F->Seen->Macro[II] = MI;
  SourceManager &SM = PP.getSourceManager();
  // Defined, or undefined, by the file's own text: pinned by it.
  FileID Origin = MI ? SM.getFileID(MI->getDefinitionLoc())
                     : stateOrigin(II, MacroDefinition());
  if (Origin == F->FID)
    return;
  SyntacticDependency D;
  D.Name = II->getName().str();
  if (MI) {
    std::optional<std::string> Tokens =
        serializeMacroTokens(*MI, SM, PP.getLangOpts());
    if (!Tokens) {
      // As for a frame: a definition whose text cannot be read cannot be
      // checked later.
      poisonOpenFrames();
      return;
    }
    D.Required = SyntacticDependency::State::ExpandsTo;
    D.TokenSequence = std::move(*Tokens);
  }
  F->Syntactic.push_back({std::move(D), Origin, ++Clock});
}

void ASTCacheDependencyTracker::recordSyntactic(const IdentifierInfo *II,
                                                const MacroInfo *MI) {
  FileID DefFID =
      MI ? PP.getSourceManager().getFileID(MI->getDefinitionLoc()) : FileID();
  // For measureIndependence: a name read as plain text depends on the #undef
  // that made it so, if a file made one.
  FileID ReadOrigin = DefFID;
  if (Measuring && !MI)
    ReadOrigin = stateOrigin(II, MacroDefinition());

  // The same sighting answers differently per frame: a macro is internal to
  // a frame whose closure contains its definition, and external -- so a
  // dependency -- to every frame nested beneath it.
  std::optional<std::string> Sequence;
  bool Built = false;

  for (Frame &F : Stack) {
    if (F.P) {
      piecesSyntactic(F, II, MI, DefFID, Sequence, Built);
      continue;
    }
    if (!F.SeenSyntactic.insert(II).second)
      continue;
    if (MI && F.holds(DefFID))
      continue;
    noteRead(F, ReadOrigin,
             (MI ? "expands " : "reads as plain text ") + II->getName());
    if (DumpSites)
      site(F, 's',
           Folding                         ? "fold"
           : PP.isParsingIfOrElifDirective() ? "value"
           : MI                              ? "expand"
                                             : "plain",
           II->getName(),
           MI ? DefFID : stateOrigin(II, MacroDefinition()));

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

  // In the file holding the query (recordFiles).
  if (auto It = FileIndex.find(QueryFID); It != FileIndex.end()) {
    FileReads &F = Files[It->second];
    if (ResolvedFID != F.FID) {
      auto [S, New] = F.Seen->Type.try_emplace(
          Name, static_cast<unsigned>(Classification), ResolvedFID);
      auto Now =
          std::make_pair(static_cast<unsigned>(Classification), ResolvedFID);
      if (New || S->second != Now) {
        S->second = Now;
        SymbolDependency D;
        D.Name = Name->getName().str();
        D.Classification = Classification;
        D.ResolvedTypeName = Ctx.ResolvedTypeName.str();
        F.Symbol.push_back({std::move(D), ResolvedFID, ++Clock});
      }
    }
  }

  for (Frame &F : Stack) {
    // Only headers whose region actually covers the query depend on it.
    if (!F.Closure.contains(QueryFID))
      continue;
    if (F.P) {
      piecesSymbol(F, Name, Classification, QueryFID, ResolvedFID,
                   HaveResolved, Ctx.ResolvedTypeName);
      continue;
    }
    // Answered from inside this header's own closure: its text pins the
    // answer, so it cannot vary between compiles.
    if (HaveResolved && F.holds(ResolvedFID))
      continue;
    if (!F.SeenSymbol.insert(Name).second)
      continue;
    noteRead(F, ResolvedFID, "names " + Name->getName());
    SiteLoc = Ctx.QueryLoc;
    site(F, 'y', Folding ? "fold" : "type", Name->getName(), ResolvedFID);

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

void ASTCacheDependencyTracker::noteRead(Frame &F, FileID Origin,
                                         const llvm::Twine &What) {
  if (!Measuring || Origin.isInvalid() || F.Closure.contains(Origin))
    return;
  if (!F.ReadFrom.count(Origin))
    F.ReadFrom[Origin] = What.str();
}

void ASTCacheDependencyTracker::site(Frame &F, char Kind, const char *Use,
                                     llvm::StringRef Name, FileID Origin) {
  if (DumpSites)
    F.At.Reads.push_back({Kind, Use, Name.str(), SiteLoc, ++Clock, Origin});
}

void ASTCacheDependencyTracker::declarationSite(const NamedDecl *D,
                                                SourceLocation Loc) {
  if (Stack.empty() || !D || Loc.isInvalid() || !D->getIdentifier())
    return;
  SourceManager &SM = PP.getSourceManager();
  FileID UseFID = SM.getFileID(SM.getExpansionLoc(Loc));
  FileID DeclFID = SM.getFileID(SM.getExpansionLoc(D->getLocation()));
  SiteLoc = Loc;
  for (Frame &F : Stack)
    // Only an outside declaration used in the header's own text.
    if (!F.P && F.Closure.contains(UseFID) && !F.holds(DeclFID) &&
        F.SeenDeclarationUse.insert(D->getCanonicalDecl()).second)
      site(F, 'd', "decl", D->getName(), DeclFID);
}

void ASTCacheDependencyTracker::MacroDefined(const Token &MacroNameTok,
                                             const MacroDirective *MD) {
  for (Frame &F : Stack)
    if (F.P)
      piecesChange(F, MacroNameTok.getIdentifierInfo());
  if (FileReads *F = currentFile(); F && MD) {
    FileID Replaced;
    if (const MacroDirective *Prev = MD->getPrevious()) {
      SourceLocation L = Prev->getLocation();
      if (const auto *Def = dyn_cast<DefMacroDirective>(Prev))
        if (const MacroInfo *Old = Def->getInfo())
          L = Old->getDefinitionLoc();
      if (L.isValid())
        Replaced = PP.getSourceManager().getFileID(L);
    }
    F->Changes.push_back({MacroNameTok.getIdentifierInfo(), Replaced, ++Clock});
  }
  if (!Measuring || Stack.empty() || !MD)
    return;
  const MacroDirective *Prev = MD->getPrevious();
  if (!Prev)
    return;
  SourceLocation Loc = Prev->getLocation();
  if (const auto *Def = dyn_cast<DefMacroDirective>(Prev)) {
    const MacroInfo *Old = Def->getInfo();
    const MacroInfo *New = MD->getMacroInfo();
    if (Old && New && Old->isIdenticalTo(*New, PP, /*Syntactically=*/true))
      return;
    if (Old)
      Loc = Old->getDefinitionLoc();
  }
  FileID Origin =
      Loc.isValid() ? PP.getSourceManager().getFileID(Loc) : FileID();
  if (Origin.isInvalid())
    return;
  for (Frame &F : Stack)
    if (!F.Closure.contains(Origin) && !F.AltersFrom.count(Origin))
      F.AltersFrom[Origin] =
          "redefines " + MacroNameTok.getIdentifierInfo()->getName().str();
}

void ASTCacheDependencyTracker::MacroUndefined(const Token &MacroNameTok,
                                               const MacroDefinition &MD,
                                               const MacroDirective *Undef) {
  for (Frame &F : Stack)
    if (F.P)
      piecesChange(F, MacroNameTok.getIdentifierInfo());
  if (FileReads *F = currentFile()) {
    FileID Replaced;
    if (const MacroInfo *MI = MD.getMacroInfo())
      Replaced = PP.getSourceManager().getFileID(MI->getDefinitionLoc());
    F->Changes.push_back({MacroNameTok.getIdentifierInfo(), Replaced, ++Clock});
  }
  if (!Measuring || Stack.empty())
    return;
  const MacroInfo *MI = MD.getMacroInfo();
  if (!MI)
    return;
  FileID Origin = PP.getSourceManager().getFileID(MI->getDefinitionLoc());
  if (Origin.isInvalid())
    return;
  for (Frame &F : Stack)
    if (!F.Closure.contains(Origin) && !F.AltersFrom.count(Origin))
      F.AltersFrom[Origin] =
          "undefines " + MacroNameTok.getIdentifierInfo()->getName().str();
}

const ASTCacheDependencyTracker::Independence *
ASTCacheDependencyTracker::independenceFor(FileID FID) const {
  for (const Independence &I : Measured)
    if (I.FID == FID)
      return &I;
  return nullptr;
}

void ASTCacheDependencyTracker::poisonOpenFrames() {
  for (Frame &F : Stack)
    F.Poisoned = true;
}

const HeaderDependencySet *
ASTCacheDependencyTracker::recordFor(FileID FID) const {
  for (size_t I = 0; I != CompletedFIDs.size(); ++I)
    if (CompletedFIDs[I] == FID)
      return &Completed[I].Deps;
  return nullptr;
}

const HeaderDependencySet *
ASTCacheDependencyTracker::findByPath(llvm::StringRef PathSuffix) const {
  for (const HeaderRecord &Rec : Completed)
    if (llvm::StringRef(Rec.HeaderPath).ends_with(PathSuffix))
      return &Rec.Deps;
  return nullptr;
}

void ASTCacheDependencyTracker::foldServed(
    SourceLocation Loc, int FirstID, unsigned NumIDs, const MetadataView &Deps,
    llvm::function_ref<SourceLocation(llvm::StringRef)> Resolve, bool Linked) {
  SourceManager &SM = PP.getSourceManager();
  FileID Where = SM.getFileID(SM.getExpansionLoc(Loc));
  bool Unfoldable = !Deps.submodules().empty() ||
                    (!Linked && !Deps.warningPragmas().empty()) ||
                    !Deps.macroStacks().empty();
  for (Frame &F : Stack) {
    if (!F.Closure.contains(Where))
      continue;
    if (Unfoldable)
      F.Poisoned = true;
    else
      F.Folded.push_back({FirstID, NumIDs});
  }
  if (Unfoldable)
    return;

  // What the copy's text tested, expanded and named, as if read here: each
  // frame depends on what of it a state outside the frame decides.
  Folding = true;
  SiteUse = "fold";
  SiteLoc = Loc;
  for (const ConditionalView &C : Deps.conditional()) {
    const IdentifierInfo *II = PP.getIdentifierInfo(C.MacroName);
    recordConditional(II, C.WasDefined,
                      stateOrigin(II, PP.getMacroDefinition(
                                          const_cast<IdentifierInfo *>(II))));
  }
  for (const NameView &N : Deps.notAMacro())
    recordSyntactic(PP.getIdentifierInfo(N.Name), nullptr);
  for (const ExpandsToView &E : Deps.expandsTo()) {
    IdentifierInfo *II = PP.getIdentifierInfo(E.Name);
    recordSyntactic(II, PP.getMacroInfo(II));
  }
  for (const SymbolView &S : Deps.symbol()) {
    QueryContext Ctx;
    Ctx.QueryLoc = Loc;
    Ctx.ResolvedLoc = Resolve(S.Name);
    Ctx.ResolvedTypeName = S.ResolvedTypeName;
    recordSymbol(PP.getIdentifierInfo(S.Name), S.Classification, Ctx);
  }
  Folding = false;
  SiteUse = "test";

  for (size_t I = 0; I != Stack.size(); ++I) {
    Frame &F = Stack[I];
    if (!F.Closure.contains(Where))
      continue;
    // A #pragma once skip of a file the frame read stays inside it.
    for (const InclusionView &V : Deps.inclusion()) {
      OptionalFileEntryRef FE =
          PP.getFileManager().getOptionalFileRef(V.HeaderPath);
      if (FE && F.ClosureFiles.contains(&FE->getFileEntry()))
        continue;
      InclusionDependency D;
      D.HeaderPath = V.HeaderPath.str();
      D.Why = V.Why;
      F.Deps.Inclusion.push_back(std::move(D));
      if (F.P)
        F.P->Rec.Origins[3].push_back(FileID());
    }
    // The copy's #include searches and __FILE__s, numbered after the
    // frame's own: the copy's header is the file the frame's pending
    // #include found.
    int32_t Self = -1;
    for (auto [FrameIndex, Index] : PendingEntries)
      if (FrameIndex == I)
        Self = Index;
    if (Self < 0) {
      F.Poisoned = true;
      continue;
    }
    int32_t Base = F.Deps.Includes.size();
    for (const IncludeView &V : Deps.includes()) {
      IncludeDependency D;
      D.K = V.K;
      D.Angled = V.Angled;
      D.Spelled = V.Spelled.str();
      D.Includer = V.Includer < 0 ? Self : Base + V.Includer;
      D.Found = V.Found.str();
      D.Name = V.Name.str();
      F.Deps.Includes.push_back(std::move(D));
      F.At.Includes.push_back(Where);
    }
    for (const FileNameView &V : Deps.fileNames()) {
      FileNameDependency D;
      D.Which = V.Which;
      D.File = V.File < 0 ? Self : Base + V.File;
      D.Text = V.Text.str();
      F.Deps.FileNames.push_back(std::move(D));
      F.At.FileNames.push_back(Where);
    }
  }
}

//===----------------------------------------------------------------------===//
// CLANG_HEADER_CACHE_PIECES: a base's dependencies, marked with their piece
// as they are recorded.
//===----------------------------------------------------------------------===//

void ASTCacheDependencyTracker::piecesInternal(Frame &F,
                                               const IdentifierInfo *II,
                                               PiecesRecord::Crossing C) {
  PiecesState &P = *F.P;
  int32_t R = P.eff(P.Cur);
  int32_t G = P.eff(C.Provider);
  if (G == R)
    return;
  // A piece reading what the base's lines or another piece set depends on
  // them: it is the base's.
  if (R >= 0) {
    P.bad(R);
    R = -1;
  }
  if (G < 0)
    return;
  // The base's lines read what piece G set.
  uintptr_t State = C.Cond ? 2 + C.Cond->WasDefined
                    : C.Sym ? 8 + uintptr_t(C.Sym->Classification)
                    : C.Syn && C.Syn->Required ==
                                   SyntacticDependency::State::ExpandsTo
                        ? 16 + std::hash<std::string>()(C.Syn->TokenSequence)
                        : 4;
  if (!P.Crossed.insert({II, G, State}).second)
    return;
  C.Provider = G;
  P.Rec.Crossings.push_back(std::move(C));
}

void ASTCacheDependencyTracker::piecesConditional(Frame &F,
                                                  const IdentifierInfo *II,
                                                  bool WasDefined,
                                                  FileID Origin) {
  PiecesState &P = *F.P;
  int32_t R = P.Cur;
  if (P.Depth == 0 && !P.FirstRootTest)
    P.FirstRootTest = II;
  NameState &S = P.Names[II];
  if (Origin.isValid() && F.holds(Origin)) {
    int16_t Changer = S.Changer;
    if (S.Cond == -2)
      S.Cond = R;
    else if (S.Cond != R)
      S.Cond = -1;
    PiecesRecord::Crossing C;
    C.Provider = Changer == -3 ? -1 : Changer;
    C.Cond = ConditionalDependency{II->getName().str(), WasDefined, 0};
    piecesInternal(F, II, std::move(C));
    return;
  }
  if (S.Cond == -2) {
    S.Cond = R;
    noteRead(F, Origin, "tests " + II->getName());
    P.depOf(II, 0) = F.Deps.Conditional.size();
    F.Deps.Conditional.push_back(
        {II->getName().str(), WasDefined, uint32_t(R + 1)});
    P.Rec.Origins[0].push_back(Origin);
    return;
  }
  // The same state outside read by another group: the base's.
  if (S.Cond != R && S.Cond != -1) {
    S.Cond = -1;
    if (int32_t D = P.depOf(II, 0); D >= 0)
      F.Deps.Conditional[D].Cond = 0;
  }
}

void ASTCacheDependencyTracker::piecesSyntactic(
    Frame &F, const IdentifierInfo *II, const MacroInfo *MI, FileID DefFID,
    std::optional<std::string> &Sequence, bool &Built) {
  PiecesState &P = *F.P;
  int32_t R = P.Cur;
  NameState &S = P.Names[II];
  int16_t Changer = S.Changer;
  auto Tokens = [&]() -> bool {
    if (!Built) {
      Sequence =
          serializeMacroTokens(*MI, PP.getSourceManager(), PP.getLangOpts());
      Built = true;
    }
    return Sequence.has_value();
  };
  if (S.Syn == -2) {
    S.Syn = R;
    if (!(MI && F.holds(DefFID))) {
      noteRead(F, MI ? DefFID : FileID(),
               (MI ? "expands " : "reads as plain text ") + II->getName());
      SyntacticDependency Dep;
      Dep.Name = II->getName().str();
      if (MI) {
        if (!Tokens()) {
          F.Poisoned = true;
          return;
        }
        Dep.Required = SyntacticDependency::State::ExpandsTo;
        Dep.TokenSequence = *Sequence;
      }
      Dep.Cond = uint32_t(R + 1);
      int32_t Index = F.Deps.Syntactic.size();
      F.Deps.Syntactic.push_back(std::move(Dep));
      P.Rec.Origins[1].push_back(MI ? DefFID : FileID());
      P.depOf(II, 1) = Index;
    }
  } else if (S.Syn != R && S.Syn != -1) {
    S.Syn = -1;
    if (int32_t D = P.depOf(II, 1); D >= 0)
      F.Deps.Syntactic[D].Cond = 0;
  }
  // A state set inside the base: by this group, another piece or the
  // base's lines.
  if (Changer == -3 || Changer == R)
    return;
  PiecesRecord::Crossing C;
  C.Provider = Changer;
  SyntacticDependency Dep;
  Dep.Name = II->getName().str();
  if (MI) {
    if (!Tokens()) {
      F.Poisoned = true;
      return;
    }
    Dep.Required = SyntacticDependency::State::ExpandsTo;
    Dep.TokenSequence = *Sequence;
  }
  C.Syn = std::move(Dep);
  piecesInternal(F, II, std::move(C));
}

void ASTCacheDependencyTracker::piecesSymbol(
    Frame &F, const IdentifierInfo *Name, SymbolClassification Classification,
    FileID QueryFID, FileID ResolvedFID, bool HaveResolved,
    llvm::StringRef ResolvedTypeName) {
  PiecesState &P = *F.P;
  auto GroupOf = [&](FileID FID) {
    if (FID == F.FID)
      return int32_t(-1);
    auto It = P.Rec.PieceOf.find(FID);
    return It == P.Rec.PieceOf.end() ? int32_t(-1) : P.eff(It->second);
  };
  int32_t R = GroupOf(QueryFID);
  if (HaveResolved && F.holds(ResolvedFID)) {
    int32_t G = GroupOf(ResolvedFID);
    if (G == R)
      return;
    if (R >= 0) {
      P.bad(R);
      R = -1;
    }
    if (G < 0)
      return;
    // The base's lines name what piece G declared: checked where another
    // version of G is read.
    PiecesRecord::Crossing C;
    C.Provider = G;
    C.Sym = SymbolDependency{Name->getName().str(), Classification,
                             ResolvedTypeName.str(), 0};
    piecesInternal(F, Name, std::move(C));
    return;
  }
  NameState &S = P.Names[Name];
  if (S.Sym == -2) {
    S.Sym = R;
    noteRead(F, ResolvedFID, "names " + Name->getName());
    P.depOf(Name, 2) = F.Deps.Symbol.size();
    F.Deps.Symbol.push_back({Name->getName().str(), Classification,
                             ResolvedTypeName.str(), uint32_t(R + 1)});
    P.Rec.Origins[2].push_back(HaveResolved ? ResolvedFID : FileID());
    return;
  }
  if (S.Sym != R && S.Sym != -1) {
    S.Sym = -1;
    if (int32_t D = P.depOf(Name, 2); D >= 0)
      F.Deps.Symbol[D].Cond = 0;
  }
}

void ASTCacheDependencyTracker::piecesChange(Frame &F,
                                             const IdentifierInfo *II) {
  PiecesState &P = *F.P;
  int32_t R = P.Cur;
  NameState &S = P.Names[II];
  if (R >= 0) {
    // A piece replacing what another group set, or changing what another
    // group read before it: read first, another version of it would change
    // what those lines read.
    auto Other = [&](int16_t G) { return G != -2 && P.eff(G) != R; };
    if ((S.Changer != -3 && P.eff(S.Changer) != R) || Other(S.Cond) ||
        Other(S.Syn) || Other(S.Sym))
      P.bad(R);
  }
  // The base's own include guard, which its first test tested, is no
  // change a header it leaves out could read.
  if (!(P.Depth == 0 && P.Changes == 0 && II == P.FirstRootTest))
    ++P.Changes;
  S.Changer = int16_t(P.Cur);
}

void ASTCacheDependencyTracker::piecesSkip(Frame &F,
                                           const FileEntryRef &SkippedFile,
                                           const Token &FilenameTok,
                                           const IdentifierInfo *Guard,
                                           FileID GuardOrigin) {
  PiecesState &P = *F.P;
  if (Guard) {
    // An #include in the base's own lines of a header read before the
    // base: one the copy leaves out.
    if (P.Depth == 0 && GuardOrigin.isValid() && !F.holds(GuardOrigin) &&
        !FilenameTok.getLocation().isMacroID()) {
      PiecesRecord::Skip K;
      K.Path = PP.getFileManager().getCanonicalName(SkippedFile).str();
      K.Guard = Guard->getName().str();
      K.Spelled = PP.getSpelling(FilenameTok);
      K.Origin = GuardOrigin;
      K.Loc = FilenameTok.getLocation();
      K.Before[0] = F.Deps.Conditional.size();
      K.Before[1] = F.Deps.Syntactic.size();
      K.Before[2] = F.Deps.Symbol.size();
      K.Before[3] = F.Deps.Inclusion.size();
      K.Clean = P.Changes == 0;
      P.Rec.Skips.push_back(std::move(K));
    }
    piecesConditional(F, Guard, true, GuardOrigin);
    return;
  }
  // A #pragma once or #import file read before: inside the base, by a
  // group, or outside it.
  int32_t R = P.Cur;
  const FileEntry *FE = &SkippedFile.getFileEntry();
  FileID O = FirstEntered.lookup(FE);
  if (F.ClosureFiles.contains(FE)) {
    int32_t G = -1;
    if (auto It = P.Rec.PieceOf.find(O); It != P.Rec.PieceOf.end())
      G = P.eff(It->second);
    if (G != R && R >= 0)
      P.bad(R);
    return;
  }
  auto [It, New] = P.Skipped.try_emplace(
      FE, int16_t(R), int32_t(F.Deps.Inclusion.size()));
  if (!New) {
    if (It->second.first != R && It->second.first != -1) {
      F.Deps.Inclusion[It->second.second].Cond = 0;
      It->second.first = -1;
    }
    return;
  }
  if (Measuring)
    noteRead(F, O, "skips " + SkippedFile.getName());
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  InclusionDependency Dep;
  Dep.HeaderPath = SkippedFile.getName().str();
  Dep.Why = HS.hasFileBeenImported(SkippedFile)
                ? InclusionDependency::Reason::Import
                : InclusionDependency::Reason::PragmaOnce;
  Dep.Cond = uint32_t(R + 1);
  F.Deps.Inclusion.push_back(std::move(Dep));
  P.Rec.Origins[3].push_back(O);
}
