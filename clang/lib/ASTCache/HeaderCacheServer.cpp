//===- HeaderCacheServer.cpp ------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/HeaderCacheServer.h"
#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/DeclBoundaryObserver.h"
#include "clang/Lex/HeaderCacheHook.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/ModuleMap.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ModuleFile.h"
#include "clang/Serialization/ModuleManager.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <iterator>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Answers the preprocessor at an #include: is there a stored parse of this
/// header that fits this compile, and if so, what should stand in for the
/// file's contents.
class HeaderCacheServer : public HeaderCacheHook {
public:
  HeaderCacheServer(CompilerInstance &CI, std::unique_ptr<CacheClient> Client)
      : CI(CI), Client(std::move(Client)) {}

  void report(llvm::raw_ostream &OS) const {
    const CacheClient::Stats &S = Client->stats();
    OS << "header cache: " << S.Hits << " hits / " << S.Lookups
       << " lookups, " << S.CandidatesRejected << " candidates rejected, "
       << SkippedAtBoundary << " not at a declaration boundary\n";
    // Which check rejected is what separates "nothing matched" from "the
    // cache is checking the wrong thing".
    for (unsigned R = 1; R != std::size(S.RejectedFor); ++R)
      if (S.RejectedFor[R])
        OS << "  rejected: " << S.RejectedFor[R] << " x "
           << describe(static_cast<MismatchReason>(R)) << "\n";
  }

  Module *tryServe(FileEntryRef File, SourceLocation IncludeLoc) override {
    Preprocessor &PP = CI.getPreprocessor();

    // Only headers a .c file includes directly are ever stored, so anything
    // deeper cannot hit and should not cost a round trip to the daemon.
    if (!PP.getSourceManager().isInMainFile(IncludeLoc))
      return nullptr;

    // A stored parse is spliced in where the #include sits, so that point
    // must be between declarations rather than inside one: an #include
    // between the declarators of "int a, b;" is not a place declarations can
    // arrive.
    DeclBoundaryObserver *Boundary = PP.getDeclBoundaryObserver();
    if (!Boundary || !Boundary->isAtDeclarationBoundary()) {
      ++SkippedAtBoundary;
      return nullptr;
    }

    // Deciding between stored entries reads the live symbol table.
    if (!CI.hasSema())
      return nullptr;

    // Entries are stored under the absolute, dot-free path the producer
    // writes, but a compile names a file however it was reached: redis
    // compiles anet.c from its own directory, so "anet.h" next to it is
    // named relatively.
    llvm::SmallString<256> Path(File.getName());
    CI.getFileManager().makeAbsolutePath(Path);
    llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);

    HeaderDependencySet Deps;
    llvm::StringRef Bytes = Client->lookup(Path, PP, &CI.getSema(), &Deps);
    if (Bytes.empty())
      return nullptr;

    Module *Target = load(Bytes);
    if (!Target)
      return nullptr;

    // What the header does to its includer's pragma state, which entering
    // the file would have done.
    applyPragmaDelta(CI.getSema(), Deps.Entry, Deps.Exit);
    return Target;
  }

private:
  /// Hands \p Bytes to the module reader under a name no file has -- they
  /// live in the daemon's shared region, not on disk -- and returns the
  /// submodule holding the header itself, which is the only part a loading
  /// compile may see. Its sibling holds the context the header was parsed
  /// in and stays hidden.
  Module *load(llvm::StringRef Bytes) {
    if (!CI.getASTReader())
      CI.createASTReader();
    ASTReader &Reader = *CI.getASTReader();

    std::string Name = "/__hc_memory/" + std::to_string(Loaded++) + ".pcm";
    Reader.getModuleManager().addInMemoryBuffer(
        Name,
        llvm::MemoryBuffer::getMemBuffer(Bytes, Name,
                                         /*RequiresNullTerminator=*/false));

    serialization::ModuleFile *MF = nullptr;
    if (!CI.loadModuleFile(Name, MF) || !MF)
      return nullptr;

    ModuleMap &MM = CI.getPreprocessor().getHeaderSearchInfo().getModuleMap();
    Module *Top = MM.findModule(MF->ModuleName);
    return Top ? Top->findSubmodule(TargetSubmoduleName) : nullptr;
  }

  CompilerInstance &CI;
  std::unique_ptr<CacheClient> Client;
  /// Only to give each loaded entry a name of its own.
  unsigned Loaded = 0;
  unsigned SkippedAtBoundary = 0;
};

/// Prints the server's counts when the main file ends. Not from the server's
/// destructor: cc1 runs with -disable-free, so the preprocessor that owns the
/// server is never destroyed and a destructor would never run.
class StatsReport : public PPCallbacks {
public:
  explicit StatsReport(const HeaderCacheServer &Server) : Server(Server) {}
  void EndOfMainFile() override { Server.report(llvm::errs()); }

private:
  const HeaderCacheServer &Server;
};

} // namespace

void clang::ast_cache::installHeaderCacheHook(
    CompilerInstance &CI, std::unique_ptr<CacheClient> Client,
    bool ReportStats) {
  if (!Client || !CI.hasPreprocessor())
    return;
  auto Server = std::make_unique<HeaderCacheServer>(CI, std::move(Client));
  if (ReportStats)
    CI.getPreprocessor().addPPCallbacks(std::make_unique<StatsReport>(*Server));
  CI.getPreprocessor().setHeaderCacheHook(std::move(Server));
}

void clang::ast_cache::installHeaderCacheHook(CompilerInstance &CI) {
  const char *Socket = std::getenv("CLANG_HEADER_CACHE");
  if (!Socket || !*Socket)
    return;

  llvm::Expected<std::unique_ptr<CacheClient>> Client =
      CacheClient::connect(Socket);
  if (!Client) {
    // No daemon listening is ordinary: compile exactly as without a cache.
    llvm::consumeError(Client.takeError());
    return;
  }

  const char *Stats = std::getenv("CLANG_HEADER_CACHE_STATS");
  installHeaderCacheHook(CI, std::move(*Client), Stats && *Stats);
}
