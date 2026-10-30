//===- PPHGeneration.cpp - Rules for generating a pre-parsed header -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/PPHGeneration.h"
#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"
#include <memory>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// The absolute, dot-free spelling of \p Name, as the header cache names a
/// header it looks up (HeaderCacheServer.cpp, absolutePath).
std::string absolutePath(FileManager &FM, llvm::StringRef Name) {
  llvm::SmallString<256> Path(Name);
  FM.makeAbsolutePath(Path);
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

class GenerationRules : public PPCallbacks {
  CompilerInstance &CI;
  CacheClient &Client;
  std::string Target;
  FileID TargetFID;
  /// The include guards of the headers the compile read as text.
  std::shared_ptr<llvm::StringSet<>> Guards;

public:
  GenerationRules(CompilerInstance &CI, CacheClient &Client,
                  llvm::StringRef Target,
                  std::shared_ptr<llvm::StringSet<>> Guards)
      : CI(CI), Client(Client), Target(Target.str()),
        Guards(std::move(Guards)) {}

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID PrevFID) override {
    SourceManager &SM = CI.getSourceManager();
    if (Reason == ExitFile && PrevFID.isValid())
      if (OptionalFileEntryRef FE = SM.getFileEntryRefForID(PrevFID)) {
        HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();
        if (const IdentifierInfo *G = HS.getFileInfo(*FE).getControllingMacro(
                HS.getExternalLookup()))
          Guards->insert(G->getName());
      }
    if (Reason == EnterFile && TargetFID.isInvalid()) {
      FileID F = SM.getFileID(Loc);
      if (OptionalFileEntryRef FE = SM.getFileEntryRefForID(F))
        if (absolutePath(CI.getFileManager(), FE->getName()) == Target)
          TargetFID = F;
      return;
    }
    // PPH required an include guard: libcpp's controlling macro, the whole
    // file inside #ifndef X ... #endif, or #pragma once (GCC
    // libcpp/files.c, cpp_main_missing_guard). The preprocessor records the
    // controlling macro as it leaves the file, before saying it left.
    if (Reason == ExitFile && PrevFID.isValid() && PrevFID == TargetFID) {
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(PrevFID);
      HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();
      HeaderFileInfo &HFI = HS.getFileInfo(*FE);
      if (!HFI.isPragmaOnce && !HFI.getControllingMacro(HS.getExternalLookup()))
        Client.refuseImage("it has no include guard");
    }
  }

  // GCC's generating compile wrote nothing for a header that runs
  // #include_next (pph-core.c, pph_include_handler).
  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef, bool, CharSourceRange,
                          OptionalFileEntryRef, StringRef, StringRef,
                          const Module *, bool,
                          SrcMgr::CharacteristicKind) override {
    if (TargetFID.isValid() &&
        CI.getSourceManager().getFileID(HashLoc) == TargetFID &&
        IncludeTok.getIdentifierInfo() &&
        IncludeTok.getIdentifierInfo()->getPPKeywordID() ==
            tok::pp_include_next)
      Client.refuseImage("it runs #include_next");
  }

  // Its guard was defined before it was included: by -include, say.
  void EndOfMainFile() override {
    if (TargetFID.isInvalid())
      Client.refuseImage("it was never entered");
  }
};

} // namespace

void clang::ast_cache::installPPHGenerationRules(CompilerInstance &CI,
                                                 CacheClient &Client,
                                                 llvm::StringRef Target) {
  Preprocessor &PP = CI.getPreprocessor();
  auto Guards = std::make_shared<llvm::StringSet<>>();
  PP.addPPCallbacks(
      std::make_unique<GenerationRules>(CI, Client, Target, Guards));

  // The image's "after" values (CheckPolicy::After): each name its record
  // lists that is a macro when the image is written. The generating compile
  // reads only the header, so its macro table then is the header's end
  // state. Each is marked 'g' if it is the include guard of a header the
  // image holds or links, else 'n': GCC applied the after value to every
  // name, which passes a header that tested a name before it defined it --
  // redis's zmalloc.h tests __GLIBC__ before including any libc header --
  // where the name is already defined; the "safe" policy applies it to
  // guards alone, what it was there for.
  Client.endState([&PP, Guards](const HeaderDependencySet &Deps) {
    for (const LinkRecord &L : Deps.Links)
      if (!L.Guard.empty())
        Guards->insert(L.Guard);
    llvm::StringMap<std::string> After;
    auto Note = [&](llvm::StringRef Name) {
      if (After.count(Name))
        return;
      IdentifierInfo *II = PP.getIdentifierInfo(Name);
      PP.refreshIdentifier(*II);
      if (const MacroInfo *MI = PP.getMacroDefinition(II).getMacroInfo())
        if (std::optional<std::string> T = serializeMacroTokens(
                *MI, PP.getSourceManager(), PP.getLangOpts()))
          After[Name] = (Guards->contains(Name) ? "g" : "n") + std::move(*T);
    };
    for (const ConditionalDependency &D : Deps.Conditional)
      Note(D.MacroName);
    for (const SyntacticDependency &D : Deps.Syntactic)
      Note(D.Name);
    for (const MacroEffect &E : Deps.MacroEffects)
      Note(E.Name);
    return After;
  });
}
