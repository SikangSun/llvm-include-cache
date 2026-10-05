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
#include "clang/ASTCache/PartExporter.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/DeclBoundaryObserver.h"
#include "clang/Lex/HeaderCacheHook.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/ModuleMap.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/ModuleFile.h"
#include "clang/Serialization/ModuleManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <optional>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// The absolute, dot-free spelling of \p Name: what the producer writes and
/// what entries are stored under. A compile names a file however it was
/// reached -- redis compiles anet.c from its own directory, so "anet.h" next
/// to it is named relatively.
std::string absolutePath(FileManager &FM, llvm::StringRef Name) {
  llvm::SmallString<256> Path(Name);
  FM.makeAbsolutePath(Path);
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

/// The text after #define that recreates \p MI as \p Name: `NAME body` or
/// `NAME(a, b) body`.
std::string defineText(const Preprocessor &PP, llvm::StringRef Name,
                       const MacroInfo &MI) {
  std::string S = Name.str();
  if (MI.isFunctionLike()) {
    S += '(';
    llvm::ListSeparator Sep(", ");
    for (const IdentifierInfo *P : MI.params()) {
      S += Sep;
      // `...` is stored as a parameter named __VA_ARGS__.
      S +=
          P->getName() == "__VA_ARGS__" ? llvm::StringRef("...") : P->getName();
    }
    // GNU's `#define F(args...)`: the last parameter carries the dots.
    if (MI.isGNUVarargs())
      S += "...";
    S += ')';
  }
  for (const Token &T : MI.tokens()) {
    // The first body token is set off from the name; a later one keeps the
    // space it had, so `a ## b` and `a##b` both come back as written.
    if (&T == MI.tokens_begin() || T.hasLeadingSpace())
      S += ' ';
    S += PP.getSpelling(T);
  }
  return S;
}

/// Records, for each header the compile reads at a declaration boundary, the
/// request that builds its saved copy: the header, and the lines the compile
/// had run before the header's #include along the chain of files that
/// included it (ProduceRequest::Context). When the main file ends they go
/// into a new job file in the directory CLANG_HEADER_CACHE_JOBS names, for
/// clang-header-cache-populate.
///
/// The flags are left out. This compile sees its cc1 options, while a saved
/// copy is built from driver flags, which the build that ran this compile has
/// and passes to the populate tool.
///
/// Example: main.c includes stdio.h, and stdio.h defines _STDIO_H and then
/// includes bits/stdio_lim.h. The job for bits/stdio_lim.h replays main.c's
/// lines before `#include <stdio.h>`, then `#define _STDIO_H` and stdio.h's
/// other lines before `#include <bits/stdio_lim.h>`.
class JobRecorder {
public:
  JobRecorder(CompilerInstance &CI, std::string Dir)
      : CI(CI), Dir(std::move(Dir)) {}

  void entered(OptionalFileEntryRef File) {
    const FileEntry *FE = File ? &File->getFileEntry() : nullptr;
    Frame F;
    F.File = FE;
    if (Frames.empty()) {
      // The main file.
      F.Recordable = FE != nullptr;
    } else if (Announced && FE && FE == AnnouncedFile) {
      // The #include just announced: its line belongs to the includer once
      // this file is left, and to no context inside this file.
      F.Recordable = Frames.back().Recordable;
      F.EnteredBy = std::move(Announced);
      if (Job)
        Jobs.push_back(std::move(*Job));
      forget();
    } else {
      // The predefines buffer, or a file entered some other way. What runs
      // there -- -D flags, -include files -- comes back through the flags.
      settle();
      F.Recordable = false;
    }
    if (FE)
      F.Dir = llvm::sys::path::parent_path(
                  absolutePath(CI.getFileManager(), File->getName()))
                  .str();
    Frames.push_back(std::move(F));
  }

  void exited() {
    settle();
    if (Frames.empty())
      return;
    Frame F = Frames.pop_back_val();
    if (F.EnteredBy && !Frames.empty() && Frames.back().Recordable)
      Frames.back().Lines.push_back(std::move(*F.EnteredBy));
  }

  void inclusion(const Token &IncludeTok, llvm::StringRef FileName,
                 bool IsAngled, OptionalFileEntryRef File,
                 llvm::StringRef SearchPath) {
    settle();
    if (Frames.empty() || !Frames.back().Recordable || !File)
      return;
    IncludeRef Ref = refFor(IncludeTok, FileName, IsAngled, *File, SearchPath);
    Announced = ContextEntry(Ref);
    AnnouncedFile = &File->getFileEntry();
    // A header entered inside a declaration can never be served there, so
    // building a copy for that #include would be wasted.
    DeclBoundaryObserver *Boundary =
        CI.getPreprocessor().getDeclBoundaryObserver();
    if (!Boundary || !Boundary->isAtDeclarationBoundary())
      return;
    ProduceRequest R;
    R.Header = std::move(Ref);
    for (const Frame &F : Frames)
      R.Context.insert(R.Context.end(), F.Lines.begin(), F.Lines.end());
    Job = std::move(R);
  }

  void defined(const Token &MacroNameTok, const MacroDirective *MD) {
    settle();
    if (Frames.empty() || !Frames.back().Recordable || !MD ||
        !MD->getMacroInfo())
      return;
    Frames.back().Lines.push_back(ContextEntry::define(defineText(
        CI.getPreprocessor(), MacroNameTok.getIdentifierInfo()->getName(),
        *MD->getMacroInfo())));
  }

  void undefined(const Token &MacroNameTok) {
    settle();
    if (Frames.empty() || !Frames.back().Recordable)
      return;
    Frames.back().Lines.push_back(
        ContextEntry::undef(MacroNameTok.getIdentifierInfo()->getName().str()));
  }

  /// Writes the jobs to a new file. A job that cannot be written -- a path
  /// with a line break in it -- is left out rather than losing the rest.
  void write() {
    settle();
    std::vector<ProduceRequest> Writable;
    for (ProduceRequest &J : Jobs) {
      if (llvm::Expected<std::string> One = encodeJobs({J}))
        Writable.push_back(std::move(J));
      else
        llvm::consumeError(One.takeError());
    }
    Jobs.clear();
    if (Writable.empty())
      return;
    llvm::Expected<std::string> Text = encodeJobs(Writable);
    if (!Text) {
      llvm::consumeError(Text.takeError());
      return;
    }
    llvm::SmallString<256> Model(Dir);
    llvm::sys::path::append(Model, "jobs-%%%%%%%%%%%%.txt");
    int FD;
    llvm::SmallString<256> Path;
    if (llvm::sys::fs::createUniqueFile(Model, FD, Path))
      return;
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS << *Text;
  }

private:
  struct Frame {
    /// Null for a buffer with no file behind it, such as the predefines.
    const FileEntry *File = nullptr;
    /// The file's absolute directory, where a quoted #include looks first.
    std::string Dir;
    /// A real file reached from the main file. Lines in anything else come
    /// from the flags.
    bool Recordable = false;
    /// This file's own #includes, #defines and #undefs so far, in order.
    std::vector<ContextEntry> Lines;
    /// The includer's #include of this file, which joins the includer's
    /// lines once this file is left.
    std::optional<ContextEntry> EnteredBy;
  };

  /// How the producer replays this #include from its generated wrapper,
  /// which sits in a directory of its own. A quoted #include found beside the
  /// file that wrote it, and an #include_next, would search elsewhere from
  /// there, so those go by path; the producer checks that every replay opens
  /// the same file.
  IncludeRef refFor(const Token &IncludeTok, llvm::StringRef FileName,
                    bool IsAngled, FileEntryRef File,
                    llvm::StringRef SearchPath) {
    IncludeRef R;
    R.Path = absolutePath(CI.getFileManager(), File.getName());
    const IdentifierInfo *Kw = IncludeTok.getIdentifierInfo();
    bool Next = Kw && Kw->getPPKeywordID() == tok::pp_include_next;
    bool BesideIncluder =
        !IsAngled && !SearchPath.empty() &&
        absolutePath(CI.getFileManager(), SearchPath) == Frames.back().Dir;
    if (!Next && !BesideIncluder && !llvm::sys::path::is_absolute(FileName))
      R.Spelled =
          (IsAngled ? "<" : "\"") + FileName.str() + (IsAngled ? ">" : "\"");
    return R;
  }

  /// The announced #include entered nothing -- an include guard skipped it,
  /// or the cache served it -- so it ran in the current file.
  void settle() {
    if (Announced && !Frames.empty() && Frames.back().Recordable)
      Frames.back().Lines.push_back(std::move(*Announced));
    forget();
  }

  void forget() {
    Announced.reset();
    AnnouncedFile = nullptr;
    Job.reset();
  }

  CompilerInstance &CI;
  std::string Dir;
  llvm::SmallVector<Frame, 16> Frames;
  /// The #include the preprocessor reported last, until the file it names
  /// is entered or something else happens.
  std::optional<ContextEntry> Announced;
  const FileEntry *AnnouncedFile = nullptr;
  /// The job for the announced header, kept if the header is entered.
  std::optional<ProduceRequest> Job;
  std::vector<ProduceRequest> Jobs;
};

/// Answers the preprocessor at an #include: is there a stored parse of this
/// header that fits this compile, and if so, what should stand in for the
/// file's contents.
class HeaderCacheServer : public HeaderCacheHook {
public:
  HeaderCacheServer(CompilerInstance &CI, std::unique_ptr<CacheClient> Client,
                    bool LogServed)
      : CI(CI), Client(std::move(Client)), LogServed(LogServed) {}

  void report(llvm::raw_ostream &OS) const {
    const CacheClient::Stats &S = Client->stats();
    OS << "header cache: " << S.Hits << " hits / " << S.Lookups << " lookups, "
       << S.CandidatesRejected << " candidates rejected, " << SkippedAtBoundary
       << " not at a declaration boundary, " << NotListed
       << " not on the ask-list\n";
    if (LoadsRefused)
      OS << "  refused by the module reader (built with other options?): "
         << LoadsRefused << "\n";
    OS << "header cache time: " << ms(LookupMicros) << " ms in lookups ("
       << ms(S.DaemonMicros) << " waiting for the daemon, " << ms(S.CheckMicros)
       << " checking candidates, " << ms(LoadMicros) << " loading), "
       << ms(S.ReportMicros) << " reporting timings, " << ms(S.ConnectMicros)
       << " connecting, " << ms(ReplayMicros) << " replaying hits' effects\n";
    const CheckCost &C = S.Check;
    auto Step = [&](CheckCost::Step T) { return ms(C.Nanos[T] / 1000); };
    OS << "header cache check time: " << S.CandidatesChecked
       << " candidates, ms in: " << Step(CheckCost::Conditions)
       << " conditions, " << Step(CheckCost::Macros) << " macros, "
       << Step(CheckCost::TypeNamesAndPragmas) << " type names and pragmas, "
       << Step(CheckCost::HeaderSearches) << " header searches ("
       << C.SearchesRedone << " redone), " << Step(CheckCost::FileNames)
       << " __FILE__, " << Step(CheckCost::FileStamps) << " file stamps ("
       << C.FilesCompared << " compared), " << Step(CheckCost::Declarations)
       << " declarations\n";
    if (EffectsWithoutDefinition)
      OS << "  macro effects without a definition in the module: "
         << EffectsWithoutDefinition << "\n";
    // Which check rejected is what separates "nothing matched" from "the
    // cache is checking the wrong thing".
    for (unsigned R = 1; R != std::size(S.RejectedFor); ++R)
      if (S.RejectedFor[R])
        OS << "  rejected: " << S.RejectedFor[R] << " x "
           << describe(static_cast<MismatchReason>(R)) << "\n";
    for (const std::string &Line : S.Rejections)
      OS << "    " << Line << "\n";
    for (const std::string &Line : Served)
      OS << "  served " << Line << "\n";
    OS << "header cache text time: " << ms(TextMicros) << " ms reading "
       << TextReads
       << " outermost headers as text, the cache's work inside them "
          "excluded\n";
    for (const std::string &Line : QueryLines)
      OS << "  query " << Line << "\n";
    for (const std::string &Line : TextLines)
      OS << "  text " << Line << "\n";
    if (Exporter) {
      const PartExporter::Stats &E = Exporter->stats();
      OS << "header cache: exported " << E.Exported << " parts (" << E.Copied
         << " with deep copies), declined " << E.Declined << ", "
         << ms(E.Micros) << " ms writing and pushing\n";
      if (LogServed) {
        for (const std::string &Line : E.Exports)
          OS << "  exported " << Line << "\n";
        for (const std::string &Line : E.Copies)
          OS << "  deep copies in " << Line << "\n";
        for (const std::string &Line : E.Declines)
          OS << "  not exported " << Line << "\n";
      }
    }
  }

  /// Exports each listed header this compile has to read itself. With
  /// \p PopulateOnly, serves nothing, and exports every listed header no
  /// stored copy fits, for the compiles after it to serve.
  void exportMisses(PartExporter &E, bool PopulateOnly) {
    Exporter = &E;
    this->PopulateOnly = PopulateOnly;
  }

  /// Stores a part this compile exported.
  void push(const PushRequest &R) { Client->push(R); }

  Module *tryServe(FileEntryRef File, SourceLocation IncludeLoc,
                   ConstSearchDirIterator CurDir) override {
    Preprocessor &PP = CI.getPreprocessor();

    // Served wherever the #include is written, in the .c file or in another
    // header. A header included from inside a.h was built after a.h's own
    // earlier lines (ProduceRequest::Context), and its metadata decides, as
    // for any other copy, whether it fits this point of this compile.
    //
    // A header no compile has found slow is parsed without asking: parsing
    // it costs less than the round trip and the load would (DECISIONS.md
    // 2.14). The list is in shared memory; this costs no socket traffic.
    std::string Path = storedName(File);
    if (!Client->isListed(Path)) {
      ++NotListed;
      return nullptr;
    }

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

    // Loading a module file sets __COUNTER__ to the value its producer ended
    // with, which counts the uses in the context the copy was built after.
    // The saved header itself used none -- the producer declines one that
    // does -- so the counter after it is the counter before it.
    uint32_t Counter = PP.getCounterValue();
    HeaderDependencySet Deps;
    Module *Target = nullptr;
    IncludeSite Site{File, CurDir, IncludeLoc};
    bool AnyFits = false;
    size_t HitBytes = 0;
    uint64_t HitLoadMicros = 0;
    const CacheClient::Stats &Before = Client->stats();
    uint64_t DaemonBefore = Before.DaemonMicros,
             CheckBefore = Before.CheckMicros;
    unsigned CheckedBefore = Before.CandidatesChecked;
    auto Asked = std::chrono::steady_clock::now();
    llvm::TimeTraceScope QueryScope("HeaderCache Query", Path);
    Client->lookup(
        Path, PP, &CI.getSema(), &Deps,
        [&](llvm::StringRef Bytes, llvm::StringRef ModuleName) {
          AnyFits = true;
          if (PopulateOnly)
            return false;
          auto Loading = std::chrono::steady_clock::now();
          Target = load(Bytes, ModuleName);
          HitLoadMicros = micros(Loading);
          HitBytes = Bytes.size();
          LoadMicros += HitLoadMicros;
          if (Target && LogServed)
            Served.push_back(Path + " from " + ModuleName.str());
          return Target != nullptr;
        },
        &Site);
    uint64_t Took = micros(Asked);
    LookupMicros += Took;
    // Notes this query, with LogServed. Replay is the time a hit spent
    // replaying the header's macro effects and pragma state, which is not
    // part of Took.
    auto Log = [&](uint64_t Replay) {
      if (!LogServed)
        return;
      const CacheClient::Stats &After = Client->stats();
      QueryLines.push_back(
          Path + (Target ? " hit" : " miss") +
          " total=" + std::to_string(Took) +
          " ask=" + std::to_string(After.DaemonMicros - DaemonBefore) +
          " check=" + std::to_string(After.CheckMicros - CheckBefore) +
          " checked=" +
          std::to_string(After.CandidatesChecked - CheckedBefore) + " load=" +
          std::to_string(HitLoadMicros) + " replay=" + std::to_string(Replay) +
          " bytes=" + std::to_string(HitBytes));
    };
    PP.setCounterValue(Counter);
    if (!Target) {
      Log(0);
      // A miss where a copy could have stood in: save this header's part of
      // this compile for the next one (DECISIONS.md 2.17).
      if (Exporter && !(PopulateOnly && AnyFits))
        Exporter->exportNext(File);
      return nullptr;
    }
    // A header being exported cannot hold a served one: its part would hold
    // the import, not the declarations.
    if (Exporter)
      Exporter->served();

    // What the header does to its includer's macros and pragma state, which
    // entering the file would have done. The module's macros are made visible
    // first, so the directives replayed below override them rather than the
    // other way around.
    auto Replaying = std::chrono::steady_clock::now();
    PP.makeModuleVisible(Target, IncludeLoc);
    replayMacroEffects(Deps.MacroEffects, *Target, IncludeLoc);
    applyPragmaDelta(CI.getSema(), Deps.Entry, Deps.Exit);
    uint64_t Replay = micros(Replaying);
    ReplayMicros += Replay;
    Log(Replay);
    return Target;
  }

  /// A header the compile is entering. Timed until it is left, headers it
  /// includes on the way counted in, since a saved copy holds all of them.
  void headerEntered(OptionalFileEntryRef File) {
    Open.push_back({File ? &File->getFileEntry() : nullptr,
                    File ? File->getName().str() : std::string(),
                    std::chrono::steady_clock::now(), cacheMicros()});
  }

  void headerExited() {
    if (Open.empty())
      return;
    OpenFile F = Open.pop_back_val();
    auto Micros = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - F.Start)
                      .count();
    // Outermost: below it only the main file, which is never left, and
    // buffers with no file behind them, such as the predefines.
    if (F.Entry && llvm::all_of(llvm::drop_begin(Open),
                                [](const OpenFile &O) { return !O.Entry; })) {
      TextMicros += Micros - (cacheMicros() - F.CacheAtStart);
      ++TextReads;
    }
    if (F.Entry && LogServed)
      TextLines.push_back(storedName(F.Name) + " " + std::to_string(Micros) +
                          " depth " + std::to_string(Open.size()));
    // Only a header's first entry in this compile is its parse; a later one
    // of an unguarded header says nothing about what a saved copy saves.
    if (!F.Entry || !Timed.insert(F.Entry).second)
      return;
    Timings.Headers.push_back(
        {storedName(F.Name), static_cast<uint64_t>(Micros)});
  }

  /// Tells the daemon how long this compile took to read each header, so it
  /// can list the slow ones for later compiles.
  void mainFileEnded() {
    if (!Timings.Headers.empty())
      Client->reportTimings(Timings);
    Timings.Headers.clear();
  }

private:
  /// The name entries are stored under; see absolutePath.
  std::string storedName(llvm::StringRef Name) {
    return absolutePath(CI.getFileManager(), Name);
  }
  std::string storedName(FileEntryRef File) {
    return storedName(File.getName());
  }

  /// Leaves every macro the header touched in the state the header left it,
  /// as a local directive at the #include. A local directive overrides the
  /// module's visible macros, which is what makes this match a textual
  /// include: the header's #undef of a macro the includer defined takes
  /// effect, and of two loaded headers that define one name, the later wins.
  /// A definition is the loaded module's own, not a copy made from text.
  void replayMacroEffects(llvm::ArrayRef<MacroEffect> Effects, Module &Target,
                          SourceLocation Loc) {
    Preprocessor &PP = CI.getPreprocessor();
    for (const MacroEffect &E : Effects) {
      IdentifierInfo *II = PP.getIdentifierInfo(E.Name);
      // Loads the module's macros for this name, if the name existed before
      // the load and is now out of date.
      PP.refreshIdentifier(*II);
      if (!E.Defined) {
        PP.appendMacroDirective(II, new (PP.getPreprocessorAllocator())
                                        UndefMacroDirective(Loc));
        continue;
      }
      ModuleMacro *MM = PP.getModuleMacro(&Target, II);
      if (MM && MM->getMacroInfo())
        PP.appendDefMacroDirective(II, MM->getMacroInfo(), Loc);
      else
        ++EffectsWithoutDefinition;
    }
  }

  /// Hands \p Bytes to the module reader under a name no file has -- they
  /// live in the daemon's shared region, not on disk -- and returns the
  /// submodule holding the header itself, which is the only part a loading
  /// compile may see. Its sibling holds the context the header was parsed
  /// in and stays hidden.
  ///
  /// Returns null, silently, when the reader refuses the bytes. The daemon
  /// stores entries by header path alone, so an entry built with other
  /// compiler options -- -fshort-enums, another -std -- is an ordinary
  /// candidate here, and the reader is what notices. CompilerInstance's
  /// loadModuleFile would report that as an error, since
  /// -Wmodule-file-config-mismatch is an error by default, and fail a build
  /// that only needed to parse the header itself.
  Module *load(llvm::StringRef Bytes, llvm::StringRef ModuleName) {
    // Loaded already in this compile: an unguarded header such as stddef.h,
    // included again in a state its copy also fits. A second module of the
    // same name is a fatal error, and the first already holds the same
    // declarations, so the hit only needs them visible again and the effects
    // replayed.
    auto Known = LoadedTargets.find(ModuleName);
    if (Known != LoadedTargets.end())
      return Known->second;

    if (!CI.getASTReader())
      CI.createASTReader();
    ASTReader &Reader = *CI.getASTReader();

    std::string Name = "/__hc_memory/" + std::to_string(Loaded++) + ".pcm";
    Reader.getModuleManager().addInMemoryBuffer(
        Name,
        llvm::MemoryBuffer::getMemBuffer(Bytes, Name,
                                         /*RequiresNullTerminator=*/false));

    serialization::ModuleFile *MF = nullptr;
    // ARR_ConfigurationMismatch makes the reader return ConfigurationMismatch
    // instead of diagnosing it. Any failure unloads what was read.
    if (Reader.ReadAST(Name, serialization::MK_ExplicitModule, SourceLocation(),
                       ASTReader::ARR_ConfigurationMismatch,
                       &MF) != ASTReader::Success ||
        !MF) {
      ++LoadsRefused;
      return nullptr;
    }

    ModuleMap &MM = CI.getPreprocessor().getHeaderSearchInfo().getModuleMap();
    Module *Top = MM.findModule(MF->ModuleName);
    if (!Top)
      return nullptr;
    // What loadModuleFile does on success: the module is known to be loaded,
    // so nothing tries to find or build it again by name.
    MM.cacheModuleLoad(*CI.getPreprocessor().getIdentifierInfo(MF->ModuleName),
                       Top);
    // A copy the producer built keeps the header in its target submodule;
    // a part exported from a real compile is the header, whole.
    Module *Target = Top->findSubmodule(TargetSubmoduleName);
    if (!Target)
      Target = Top;
    LoadedTargets[MF->ModuleName] = Target;
    return Target;
  }

  CompilerInstance &CI;
  std::unique_ptr<CacheClient> Client;
  /// Only to give each loaded entry a name of its own.
  unsigned Loaded = 0;
  /// Each saved copy loaded so far, by module name, for a hit on it again.
  llvm::StringMap<Module *> LoadedTargets;
  /// Each hit's header and module, for CLANG_HEADER_CACHE_STATS=2: which
  /// copy a compile used is what a failure after a hit needs traced to.
  bool LogServed;
  std::vector<std::string> Served;
  unsigned SkippedAtBoundary = 0;
  /// Macros a header left defined whose definition the loaded module did not
  /// carry. Expected to stay zero; the module's own visible macro still
  /// applies to such a name.
  unsigned EffectsWithoutDefinition = 0;
  /// Wall time in lookups, the loads they made included, and in the loads.
  uint64_t LookupMicros = 0, LoadMicros = 0;
  /// Wall time after hits making the PCM's macros visible and replaying the
  /// header's macro effects and pragma state; not part of LookupMicros.
  uint64_t ReplayMicros = 0;

  static uint64_t micros(std::chrono::steady_clock::time_point Start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - Start)
        .count();
  }
  static std::string ms(uint64_t Micros) {
    return llvm::formatv("{0:f1}", Micros / 1000.0).str();
  }
  /// Candidates whose bytes the module reader refused, most likely because
  /// they were built with other compiler options.
  unsigned LoadsRefused = 0;
  unsigned NotListed = 0;
  /// Null unless this compile exports its misses.
  PartExporter *Exporter = nullptr;
  bool PopulateOnly = false;

  struct OpenFile {
    /// Null for a buffer with no file behind it, such as the predefines.
    const FileEntry *Entry;
    std::string Name;
    std::chrono::steady_clock::time_point Start;
    /// cacheMicros() when the file was entered.
    uint64_t CacheAtStart;
  };
  /// The cache's own time so far: lookups, the loads they made included,
  /// and writing parts.
  uint64_t cacheMicros() const {
    return LookupMicros + (Exporter ? Exporter->stats().Micros : 0);
  }
  /// Time reading outermost headers as text, the cache's own work inside
  /// them excluded, and how many such reads.
  uint64_t TextMicros = 0;
  unsigned TextReads = 0;
  /// With LogServed: "<header> hit|miss total= ask= check= checked= load=
  /// replay= bytes=" per lookup (times in us; checked = saved PCMs checked;
  /// replay comes after total and is not part of it), and
  /// "<header> <us> depth <n>" per header read as text, n = 0 outermost.
  std::vector<std::string> QueryLines, TextLines;
  /// Files entered and not yet left, innermost last.
  llvm::SmallVector<OpenFile, 16> Open;
  llvm::SmallPtrSet<const FileEntry *, 32> Timed;
  TimingReport Timings;
};

/// Feeds the server the files the compile enters and leaves, and when the
/// main file ends has it report its timings and, if asked, print its counts.
/// Not from the server's destructor: cc1 runs with -disable-free, so the
/// preprocessor that owns the server is never destroyed and a destructor
/// would never run.
class ServerCallbacks : public PPCallbacks {
public:
  ServerCallbacks(HeaderCacheServer &Server, SourceManager &SM,
                  bool ReportStats, std::unique_ptr<JobRecorder> Recorder)
      : Server(Server), SM(SM), ReportStats(ReportStats),
        Recorder(std::move(Recorder)) {}

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID) override {
    if (Reason == EnterFile) {
      OptionalFileEntryRef File = SM.getFileEntryRefForID(SM.getFileID(Loc));
      Server.headerEntered(File);
      if (Recorder)
        Recorder->entered(File);
    } else if (Reason == ExitFile) {
      Server.headerExited();
      if (Recorder)
        Recorder->exited();
    }
  }

  void InclusionDirective(SourceLocation, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled, CharSourceRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef, const Module *, bool,
                          SrcMgr::CharacteristicKind) override {
    if (Recorder)
      Recorder->inclusion(IncludeTok, FileName, IsAngled, File, SearchPath);
  }

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *MD) override {
    if (Recorder)
      Recorder->defined(MacroNameTok, MD);
  }

  void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &,
                      const MacroDirective *) override {
    if (Recorder)
      Recorder->undefined(MacroNameTok);
  }

  void EndOfMainFile() override {
    Server.mainFileEnded();
    if (Recorder)
      Recorder->write();
    if (ReportStats)
      Server.report(llvm::errs());
  }

private:
  HeaderCacheServer &Server;
  SourceManager &SM;
  bool ReportStats;
  /// Null unless the compile was asked to record jobs.
  std::unique_ptr<JobRecorder> Recorder;
};

} // namespace

void clang::ast_cache::installHeaderCacheHook(
    CompilerInstance &CI, std::unique_ptr<CacheClient> Client, bool ReportStats,
    llvm::StringRef JobsDir, ExportMode Export) {
  if (!Client || !CI.hasPreprocessor())
    return;
  bool Detailed = Client->loggingRejections();
  auto Server =
      std::make_unique<HeaderCacheServer>(CI, std::move(Client), Detailed);
  if (Export != ExportMode::None) {
    HeaderCacheServer *S = Server.get();
    S->exportMisses(
        PartExporter::install(CI, [S](PushRequest &&R) { S->push(R); }),
        Export == ExportMode::PopulateOnly);
  }
  std::unique_ptr<JobRecorder> Recorder;
  if (!JobsDir.empty())
    Recorder = std::make_unique<JobRecorder>(CI, JobsDir.str());
  CI.getPreprocessor().addPPCallbacks(std::make_unique<ServerCallbacks>(
      *Server, CI.getSourceManager(), ReportStats, std::move(Recorder)));
  CI.getPreprocessor().setHeaderCacheHook(std::move(Server));
}

//===----------------------------------------------------------------------===//
// Experiment for DECISIONS.md 2.17, not yet part of the design: export one
// header's own part from an ordinary compile, and serve that header from the
// export in a later compile.
//
//   CLANG_HEADER_CACHE_EXPORT=<header>=<out.pcm>
//   CLANG_HEADER_CACHE_LOAD=<header>=<in.pcm>
//
// <header> is an absolute path. The export is the one a compile makes at a
// miss (PartExporter), written to a file instead of pushed, and the load
// checks nothing: the two are for looking at a part, not for serving one.
//===----------------------------------------------------------------------===//

namespace {

std::pair<std::string, std::string> experimentSetting(const char *Var) {
  const char *V = std::getenv(Var);
  if (!V)
    return {};
  auto [Header, File] = llvm::StringRef(V).split('=');
  return {Header.str(), File.str()};
}

class LoadExperiment : public HeaderCacheHook {
public:
  LoadExperiment(CompilerInstance &CI, std::string Header, std::string In)
      : CI(CI), Header(std::move(Header)), In(std::move(In)) {}

  Module *tryServe(FileEntryRef File, SourceLocation,
                   ConstSearchDirIterator) override {
    if (Served || absolutePath(CI.getFileManager(), File.getName()) != Header)
      return nullptr;
    Served = true;
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
        llvm::MemoryBuffer::getFile(In);
    if (!Buf) {
      llvm::errs() << "header cache load: " << In << ": "
                   << Buf.getError().message() << "\n";
      return nullptr;
    }
    if (!CI.getASTReader())
      CI.createASTReader();
    ASTReader &Reader = *CI.getASTReader();
    std::string Name = "/__hcx_memory/in.pcm";
    Reader.getModuleManager().addInMemoryBuffer(Name, std::move(*Buf));

    Preprocessor &PP = CI.getPreprocessor();
    uint32_t Counter = PP.getCounterValue();
    serialization::ModuleFile *MF = nullptr;
    ASTReader::ASTReadResult R =
        Reader.ReadAST(Name, serialization::MK_ExplicitModule, SourceLocation(),
                       ASTReader::ARR_None, &MF);
    PP.setCounterValue(Counter);
    if (R != ASTReader::Success || !MF) {
      llvm::errs() << "header cache load: the reader refused " << In << "\n";
      return nullptr;
    }
    ModuleMap &MM = PP.getHeaderSearchInfo().getModuleMap();
    Module *Top = MM.findModule(MF->ModuleName);
    if (!Top) {
      llvm::errs() << "header cache load: no module " << MF->ModuleName << "\n";
      return nullptr;
    }
    MM.cacheModuleLoad(*PP.getIdentifierInfo(MF->ModuleName), Top);
    llvm::errs() << "header cache load: served " << Header << " from " << In
                 << "\n";
    return Top;
  }

private:
  CompilerInstance &CI;
  std::string Header, In;
  bool Served = false;
};

} // namespace

void clang::ast_cache::installHeaderCacheHook(CompilerInstance &CI) {
  // The 2.17 experiment, above.
  if (auto [Header, Out] = experimentSetting("CLANG_HEADER_CACHE_EXPORT");
      !Header.empty() && CI.hasPreprocessor()) {
    PartExporter &E = PartExporter::install(CI, [Out = Out](PushRequest &&R) {
      std::error_code EC;
      llvm::raw_fd_ostream OS(Out, EC);
      if (EC) {
        llvm::errs() << "header cache export: " << Out << ": " << EC.message()
                     << "\n";
        return;
      }
      OS << R.Data;
      llvm::errs() << "header cache export: " << R.Filename << " -> " << Out
                   << ", " << R.Data.size() << " bytes, "
                   << R.Deps.Declarations.size()
                   << " outside declarations, module " << R.ModuleName << "\n";
    });
    E.exportWhenEntered(Header);
  }
  if (auto [Header, In] = experimentSetting("CLANG_HEADER_CACHE_LOAD");
      !Header.empty() && CI.hasPreprocessor()) {
    CI.getPreprocessor().setHeaderCacheHook(
        std::make_unique<LoadExperiment>(CI, Header, In));
    return;
  }

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

  // 1 prints the counts when the main file ends; 2 also names what failed
  // for each rejected candidate.
  const char *Stats = std::getenv("CLANG_HEADER_CACHE_STATS");
  llvm::StringRef Level = Stats ? Stats : "";
  (*Client)->logRejections(Level == "2");
  // A directory to record, for every header this compile reads, the job that
  // would build its saved copy.
  const char *Jobs = std::getenv("CLANG_HEADER_CACHE_JOBS");
  // A compile exports each listed header it has to read itself, unless told
  // not to; "populate" exports without serving (DECISIONS.md 2.17).
  const char *ExportVar = std::getenv("CLANG_HEADER_CACHE_EXPORT_PARTS");
  llvm::StringRef ExportSetting = ExportVar ? ExportVar : "";
  ExportMode Export = ExportSetting == "0"          ? ExportMode::None
                      : ExportSetting == "populate" ? ExportMode::PopulateOnly
                                                    : ExportMode::Misses;
  installHeaderCacheHook(CI, std::move(*Client), !Level.empty(),
                         Jobs ? Jobs : "", Export);
}
