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
#include "llvm/ADT/DenseSet.h"
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
                    bool LogServed, WarningPragmaTracker &Pragmas)
      : CI(CI), Client(std::move(Client)), LogServed(LogServed),
        Pragmas(Pragmas),
        WarningFlags(warningFlagsHash(CI.getDiagnosticOpts())) {
    // For measuring what skipping saves: CLANG_HEADER_CACHE_REPLAY_ALL=1
    // replays every effect, as before it (REPORT.md I8).
    const char *All = std::getenv("CLANG_HEADER_CACHE_REPLAY_ALL");
    ReplayAll = All && llvm::StringRef(All) == "1";
  }

  void report(llvm::raw_ostream &OS) const {
    const CacheClient::Stats &S = Client->stats();
    OS << "header cache: " << S.Hits << " hits / " << S.Lookups << " lookups, "
       << S.CandidatesRejected << " candidates rejected, " << SkippedAtBoundary
       << " not at a declaration boundary, " << NotListed
       << " not on the ask-list, " << InsideWritten
       << " inside a header being written\n";
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
       << C.SearchesRedone << " redone, " << C.SearchesReused
       << " copies' reused from the build), " << Step(CheckCost::FileNames)
       << " __FILE__, " << Step(CheckCost::FileStamps) << " file stamps ("
       << C.FilesCompared << " compared), " << Step(CheckCost::Declarations)
       << " declarations\n";
    if (OtherCopySameName)
      OS << "  fitting copies passed over, another copy of the same name "
            "loaded: "
         << OtherCopySameName << "\n";
    if (DiagnosticsReplayed)
      OS << "  diagnostics printed again from copies: " << DiagnosticsReplayed
         << "\n";
    OS << "  macro effects replayed: " << EffectsReplayed
       << ", skipped: " << EffectsSkipped << "\n";
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
    for (const std::string &Line : S.Checks)
      OS << "  check " << Line << "\n";
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
         << ms(E.Micros) << " ms writing and pushing, " << E.Submodules
         << " headers held as submodules\n";
      OS << "header cache write time: " << ms(E.WriterMicros)
         << " ms in Clang's writer, "
         << ms(E.ComposeMicros) << " in compose (" << ms(E.ChooseMicros)
         << " choosing, " << ms(E.DepsMicros) << " tagging dependencies)\n";
      for (const std::string &Line : E.Independence)
        OS << "  indep " << Line << "\n";
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
    NextReason.clear();
    std::string Path = storedName(File);
    if (!Client->isListed(Path)) {
      ++NotListed;
      NextReason = "not-listed";
      return nullptr;
    }

    // Inside a header this compile will write, everything is read as text:
    // its part is a folded copy, and the writer can fold in only what the
    // compile parsed itself -- a served header's declarations would be
    // written as references to its PCM, which no part can carry. Nothing is
    // asked either, and the header is not exported on its own; the folded
    // copy holds it.
    if (Exporter && !PopulateOnly && Exporter->writing()) {
      ++InsideWritten;
      NextReason = "inside-a-header-being-written";
      return nullptr;
    }

    // A stored parse is spliced in where the #include sits, so that point
    // must be between declarations rather than inside one: an #include
    // between the declarators of "int a, b;" is not a place declarations can
    // arrive.
    DeclBoundaryObserver *Boundary = PP.getDeclBoundaryObserver();
    if (!Boundary || !Boundary->isAtDeclarationBoundary()) {
      ++SkippedAtBoundary;
      NextReason = "inside-a-declaration";
      return nullptr;
    }

    // Deciding between stored entries reads the live symbol table.
    if (!CI.hasSema()) {
      NextReason = "no-sema";
      return nullptr;
    }

    // Loading a module file sets __COUNTER__ to the value its producer ended
    // with, which counts the uses in the context the copy was built after.
    // The saved header itself used none -- the producer declines one that
    // does -- so the counter after it is the counter before it.
    uint32_t Counter = PP.getCounterValue();
    MetadataView Deps;
    Module *Target = nullptr;
    IncludeSite Site{File, CurDir, IncludeLoc,
                     warningState(WarningFlags, &Pragmas),
                     searchContext(File, CurDir)};
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
          Target = load(Bytes, ModuleName, IncludeLoc);
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
      if (LogServed)
        NextReason = Client->lastCandidates() == 0 ? std::string("no-saved-PCM")
                     : Client->lastRejection() != MismatchReason::None
                         ? "rejected:" + describe(Client->lastRejection()).str()
                         : std::string("load-refused");
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

    // The copy's references to declarations outside it resolve to the ones
    // the check found here, as plain clang's did where it read the header;
    // otherwise each resolves when first read, to the name's latest
    // declaration, which may come after this #include (REPORT.md Q8).
    const LoadedCopy &Copy = LoadedTargets.find(LastModuleName)->second;
    if (Copy.Fresh) {
      llvm::ArrayRef<NamedDecl *> Found = Client->matchedDeclarations();
      IdentifierTable &Idents = PP.getIdentifierTable();
      size_t I = 0;
      auto Pin = [&](MetadataSection<DeclarationView> Decls) {
        for (const DeclarationView &D : Decls) {
          if (I == Found.size())
            break;
          // Null for one that does not apply where the hit is.
          if (NamedDecl *To = Found[I++])
            CI.getASTReader()->pinNameReference(
                *Copy.MF, Idents.get(D.Name),
                D.In == DeclarationDependency::Space::Tag, To);
        }
      };
      Pin(Deps.declarations());
    }

    // REPORT.md T17: the submodules that load, each found inside its
    // parent's module, and which elements of the metadata apply.
    const std::vector<bool> &Loaded = Client->lastSubmodules();
    std::vector<Module *> Subs;
    for (const SubmoduleView &M : Deps.submodules()) {
      Module *In = M.Parent < 0 ? Target : Subs[M.Parent];
      Subs.push_back(In ? In->findSubmodule(M.Name) : nullptr);
    }
    std::vector<char> Applies{true};
    std::vector<int> OwnerOf{-1};
    for (const ConditionView &C : Deps.conditions()) {
      bool OwnerLoads = C.Owner < 0 || (C.Owner < (int)Loaded.size() &&
                                        Loaded[C.Owner]);
      bool ProviderSkipped = C.Provider < 0 ||
                             C.Provider >= (int)Loaded.size() ||
                             !Loaded[C.Provider];
      Applies.push_back(OwnerLoads && ProviderSkipped);
      OwnerOf.push_back(C.Owner);
    }

    // What the header does to its includer's macros and pragma state, which
    // entering the file would have done. The module's macros are made visible
    // first, so the directives replayed below override them rather than the
    // other way around.
    auto Replaying = std::chrono::steady_clock::now();
    // The pushes go first: they save this compile's definitions, which the
    // copy's own would hide once visible.
    replayMacroStacks(Deps.macroStacks(), IncludeLoc);
    {
      llvm::TimeTraceScope S("HeaderCache MakeVisible");
      PP.makeModuleVisible(Target, IncludeLoc);
      for (size_t K = 0; K != Subs.size(); ++K)
        if (K < Loaded.size() && Loaded[K] && Subs[K])
          PP.makeModuleVisible(Subs[K], IncludeLoc);
    }
    {
      llvm::TimeTraceScope S("HeaderCache ReplayEffects");
      replayMacroEffects(Deps.macroEffects(), *Target, IncludeLoc, Applies,
                         OwnerOf, Subs);
    }
    {
      llvm::TimeTraceScope S("HeaderCache PragmaDelta");
      applyPragmaDelta(CI.getSema(), Deps.entry(), Deps.exit());
    }
    printDiagnostics(Deps.diagnostics(), *Copy.MF, Applies);
    // The warning pragmas the header left in force, as its text would have.
    for (const WarningPragmaView &W : Deps.warningPragmas())
      Pragmas.apply(CI.getDiagnostics(), W.Pragma, IncludeLoc);
    uint64_t Replay = micros(Replaying);
    ReplayMicros += Replay;
    Timings.Hits.push_back({Path, Took + Replay});
    Log(Replay);
    return Target;
  }

  /// A header the compile is entering. Timed until it is left, headers it
  /// includes on the way counted in, since a saved copy holds all of them.
  void headerEntered(OptionalFileEntryRef File) {
    Open.push_back({File ? &File->getFileEntry() : nullptr,
                    File ? File->getName().str() : std::string(),
                    std::chrono::steady_clock::now(), cacheMicros(), 0,
                    NextReason.empty() ? std::string("not-asked")
                                       : std::move(NextReason)});
    NextReason.clear();
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
    // Its own time: the headers inside it and the cache's work while it was
    // open taken out, so the own times of all reads add up to the time spent
    // reading headers as text.
    uint64_t Own = Micros - F.ChildMicros - (cacheMicros() - F.CacheAtStart);
    if (!Open.empty())
      Open.back().ChildMicros += Micros - (cacheMicros() - F.CacheAtStart);
    if (F.Entry && LogServed)
      TextLines.push_back(storedName(F.Name) + " " + std::to_string(Micros) +
                          " depth " + std::to_string(Open.size()) + " own " +
                          std::to_string(Own) + " why " + F.Reason);
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
    if (!Timings.Headers.empty() || !Timings.Hits.empty())
      Client->reportTimings(Timings);
    Timings.Headers.clear();
    Timings.Hits.clear();
  }

private:
  /// What a header's own #include searches depend on besides the files
  /// present (REPORT.md I9): the include path -- each directory as written,
  /// its kind and whether it is a system one, where the angled and system
  /// ones begin -- the working directory, the options that change how a
  /// search goes, the name this compile found the header by, and the
  /// directory it found it in.
  uint64_t searchContext(FileEntryRef File, ConstSearchDirIterator Dir) {
    HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();
    if (!SearchPathHash) {
      std::string K;
      auto Num = [&](uint64_t V) {
        K.append(reinterpret_cast<const char *>(&V), sizeof(V));
      };
      auto Str = [&](llvm::StringRef V) {
        K += V;
        K += '\0';
      };
      Str(CI.getFileManager().getFileSystemOpts().WorkingDir);
      if (llvm::ErrorOr<std::string> CWD =
              CI.getFileManager().getVirtualFileSystem()
                  .getCurrentWorkingDirectory())
        Str(*CWD);
      const HeaderSearchOptions &O = HS.getHeaderSearchOpts();
      Str(O.Sysroot);
      for (const std::string &F : O.VFSOverlayFiles)
        Str(F);
      Num(CI.getLangOpts().MSVCCompat);
      unsigned Index = 0, Angled = 0, System = 0;
      for (const DirectoryLookup &D : HS.search_dir_range()) {
        if (HS.angled_dir_begin() != HS.search_dir_end() &&
            &D == &*HS.angled_dir_begin())
          Angled = Index;
        if (HS.system_dir_begin() != HS.search_dir_end() &&
            &D == &*HS.system_dir_begin())
          System = Index;
        Num(D.getLookupType());
        Num(D.getDirCharacteristic());
        Str(D.getName());
        ++Index;
      }
      Num(Angled);
      Num(System);
      Num(Index);
      SearchPathHash = llvm::xxh3_64bits(K) | 1;
    }
    std::string K;
    K.append(reinterpret_cast<const char *>(&SearchPathHash),
             sizeof(SearchPathHash));
    K += File.getName();
    K += '\0';
    int64_t At = Dir ? static_cast<int64_t>(HS.searchDirIdx(*Dir)) : -1;
    K.append(reinterpret_cast<const char *>(&At), sizeof(At));
    return llvm::xxh3_64bits(K);
  }
  uint64_t SearchPathHash = 0;

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
  ///
  /// Most effects need nothing: loading the copy made its macros visible,
  /// and the visible definition is the only one. Finding the name for each,
  /// which searches every loaded copy's table, was most of a hit's cost
  /// (REPORT.md I8). An effect is skipped when this compile has never seen
  /// its name and no earlier copy touched it. The first use of the name then
  /// finds it, the copy's definition with it, in every loaded copy.
  ///
  /// A name the compile has seen is replayed: loading a copy marks every
  /// name already seen as out of date, and some readers of a macro do not
  /// bring one up to date -- the check that skips an #include whose guard
  /// macro is defined, #pragma push_macro. Another precompiled file -- a
  /// PCH -- may hold directives of its own, so with one loaded every effect
  /// is replayed.
  /// The header's #pragma pop_macro and push_macro that reach outside it,
  /// as its text would have run them: the pops first, then the pushes, all
  /// before the header's own definitions (REPORT.md Q11). The check made
  /// sure each pop takes what it took where the copy was written.
  void replayMacroStacks(MetadataSection<MacroStackView> Stacks,
                         SourceLocation Loc) {
    Preprocessor &PP = CI.getPreprocessor();
    for (const MacroStackView &Stack : Stacks) {
      const MacroStackEffect &E = Stack.Effect;
      TouchedByCopies.insert(llvm::xxh3_64bits(E.Name));
      IdentifierInfo *II = PP.getIdentifierInfo(E.Name);
      PP.refreshIdentifier(*II);
      for (size_t I = 0, N = E.Popped.size(); I != N; ++I)
        PP.popMacro(II, Loc);
      for (uint32_t I = 0; I != E.Pushes; ++I)
        PP.pushMacro(II);
    }
  }

  /// \p Applies, \p OwnerOf and \p Subs (REPORT.md T17): whether each
  /// condition holds, the submodule it belongs to, and each submodule's
  /// module. An effect of a submodule that does not load is the compile's
  /// own header's to have had; one of a submodule that loads has its
  /// definition in the submodule's module.
  void replayMacroEffects(MetadataSection<MacroEffectView> Effects,
                          Module &Target, SourceLocation Loc,
                          llvm::ArrayRef<char> Applies = {},
                          llvm::ArrayRef<int> OwnerOf = {},
                          llvm::ArrayRef<Module *> Subs = {}) {
    Preprocessor &PP = CI.getPreprocessor();
    IdentifierTable &Idents = PP.getIdentifierTable();
    bool OnlyCopies =
        !ReplayAll &&
        CI.getASTReader()->getModuleManager().size() == CopiesLoaded;
    for (const MacroEffectView &E : Effects) {
      if (E.Cond && E.Cond < Applies.size() && !Applies[E.Cond])
        continue;
      Module *In = &Target;
      if (E.Cond && E.Cond < OwnerOf.size() && OwnerOf[E.Cond] >= 0 &&
          OwnerOf[E.Cond] < (int)Subs.size() && Subs[OwnerOf[E.Cond]])
        In = Subs[OwnerOf[E.Cond]];
      uint64_t Hash = llvm::xxh3_64bits(E.Name);
      // A name the copy leaves a macro is one, replayed or not, for the
      // check's filter (REPORT.md I14).
      if (E.Defined)
        PP.noteMacroNameHash(Hash);
      bool Earlier = !TouchedByCopies.insert(Hash).second;
      if (OnlyCopies && !Earlier && Idents.find(E.Name) == Idents.end()) {
        ++EffectsSkipped;
        continue;
      }
      ++EffectsReplayed;
      IdentifierInfo *II = PP.getIdentifierInfo(E.Name);
      // Loads the module's macros for this name, if the name existed before
      // the load and is now out of date.
      PP.refreshIdentifier(*II);
      if (!E.Defined) {
        PP.appendMacroDirective(II, new (PP.getPreprocessorAllocator())
                                        UndefMacroDirective(Loc));
        continue;
      }
      ModuleMacro *MM = PP.getModuleMacro(In, II);
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
  /// The diagnostics a copy's header printed where the copy was written,
  /// printed again: the copy is served only under the same warning flags,
  /// and where no warning pragma is in force, so they are the ones the
  /// header's text would print here (REPORT.md Q34).
  void printDiagnostics(MetadataSection<DiagnosticView> Saved,
                        serialization::ModuleFile &MF,
                        llvm::ArrayRef<char> Applies = {}) {
    if (Saved.empty())
      return;
    ASTReader &Reader = *CI.getASTReader();
    SourceManager &SM = CI.getSourceManager();
    auto Range = [&](const SavedDiagnostic::Range &R) {
      SourceRange Within(Reader.ReadSourceLocation(MF, R.Begin),
                         Reader.ReadSourceLocation(MF, R.End));
      return R.TokenRange ? CharSourceRange::getTokenRange(Within)
                          : CharSourceRange::getCharRange(Within);
    };
    for (const DiagnosticView &Ref : Saved) {
      const SavedDiagnostic &D = Ref.Diagnostic;
      // Printed by the compile's own header where its submodule is skipped.
      if (D.Cond && D.Cond < Applies.size() && !Applies[D.Cond])
        continue;
      llvm::SmallVector<CharSourceRange, 2> Ranges;
      for (const SavedDiagnostic::Range &R : D.Ranges)
        Ranges.push_back(Range(R));
      llvm::SmallVector<FixItHint, 1> FixIts;
      for (const SavedDiagnostic::FixIt &F : D.FixIts) {
        FixItHint &H = FixIts.emplace_back();
        H.RemoveRange = Range(F.Remove);
        H.CodeToInsert = F.Insert;
        H.BeforePreviousInsertions = F.BeforePreviousInsertions;
      }
      StoredDiagnostic Stored(
          static_cast<DiagnosticsEngine::Level>(D.Level), D.ID, D.Message,
          FullSourceLoc(Reader.ReadSourceLocation(MF, D.Loc), SM), Ranges,
          FixIts);
      CI.getDiagnostics().Report(Stored);
      ++DiagnosticsReplayed;
    }
  }

  /// REPORT.md T17: the compile's own declaration for each declaration of
  /// a skipped submodule that the copy's other lines refer to, found by
  /// name where the header is included. False if the compile has none of
  /// some name: the copy is then not used.
  bool resolveProvided(const MetadataView &Deps,
                       const std::vector<bool> &Loads,
                       std::vector<std::pair<uint32_t, Decl *>> &Swaps) {
    Sema &S = CI.getSema();
    IdentifierTable &Idents = CI.getPreprocessor().getIdentifierTable();
    for (const ProvidedView &D : Deps.provided()) {
      size_t K = D.Cond - 1;
      if (!D.Cond || (K < Loads.size() && Loads[K]))
        continue;
      NamedDecl *Live = ASTReader::lookupAtFileScope(
          S, Idents.get(D.Name), D.In == DeclarationDependency::Space::Tag,
          /*CreateBuiltin=*/true);
      if (!Live)
        return false;
      QualType Under;
      if (auto *TD = dyn_cast<TypedefNameDecl>(Live))
        Under = TD->getUnderlyingType();
      for (const ProvidedView::Use &U : D.Uses) {
        Decl *To = nullptr;
        using How = ProvidedDeclaration::How;
        if (U.Of == How::Self) {
          To = Live;
        } else if (U.Of == How::TagOfTypedef) {
          if (!Under.isNull())
            To = Under->getAsTagDecl();
        } else {
          RecordDecl *R = !Under.isNull() ? Under->getAsRecordDecl()
                                          : dyn_cast<RecordDecl>(Live);
          if (R && (R = R->getDefinition()))
            for (NamedDecl *M : R->lookup(&Idents.get(U.Member)))
              if (isa<FieldDecl, IndirectFieldDecl>(M)) {
                To = M;
                break;
              }
        }
        if (!To)
          return false;
        Swaps.push_back({U.Index, To});
      }
    }
    return true;
  }

  Module *load(llvm::StringRef Bytes, llvm::StringRef ModuleName,
               SourceLocation IncludeLoc) {
    LastModuleName = ModuleName.str();
    // Loaded already in this compile: an unguarded header such as stddef.h,
    // included again in a state its copy also fits. A second module of the
    // same name is a fatal error, and the first already holds the same
    // declarations, so the hit only needs them visible again and the effects
    // replayed.
    //
    // Another copy can have the same name: a name is taken before the outside
    // declarations a copy refers to are known, so two copies of a header
    // that differ only in those share it. Such a copy is not the loaded one.
    // In `int f(); #include "h.h" int f(int); #include "h.h"`, with h.h
    // `extern __typeof__(f) *fp;`, the loaded copy's fp has no prototype and
    // the second copy's has f(int)'s; taking the first again leaves fp
    // without one (REPORT.md Q9). Loading the second too would declare a
    // second fp, since Clang merges two copies' declarations only when they
    // are the same. Plain clang reads the second include as text, which
    // makes its fp a redeclaration of the first, and so does this compile.
    // REPORT.md T17: the submodules this hit skips, whose declarations the
    // reader leaves unread, each the copy's lines refer to standing for the
    // compile's own.
    const std::vector<bool> &Loads = Client->lastSubmodules();
    const std::vector<std::string> &Names = Client->lastSubmoduleNames();
    const std::vector<std::string> &Paths = Client->lastSubmodulePaths();
    std::vector<std::string> Skips;
    for (size_t K = 0; K != Names.size(); ++K)
      if (K >= Loads.size() || !Loads[K])
        Skips.push_back(Names[K]);

    auto Known = LoadedTargets.find(ModuleName);
    if (Known != LoadedTargets.end()) {
      // A submodule left unread where the copy was loaded cannot load now.
      if (Skips.size() != Names.size()) {
        llvm::StringMap<Module *> ByName;
        std::vector<Module *> Stack{Known->second.Target->getTopLevelModule()};
        while (!Stack.empty()) {
          Module *M = Stack.back();
          Stack.pop_back();
          for (Module *Sub : M->submodules()) {
            ByName[Sub->Name] = Sub;
            Stack.push_back(Sub);
          }
        }
        for (size_t K = 0; K != Names.size(); ++K)
          if (K < Loads.size() && Loads[K])
            if (Module *Sub = ByName.lookup(Names[K]);
                Sub && CI.getASTReader()->isPartSkipped(Sub)) {
              ++LoadsRefused;
              return nullptr;
            }
      }
      if (Known->second.Bytes.data() == Bytes.data() ||
          Known->second.Bytes == Bytes) {
        Known->second.Fresh = false;
        return Known->second.Target;
      }
      ++OtherCopySameName;
      return nullptr;
    }

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
    // Imported where it is included: a diagnostic printed again from it
    // names that #include in its "In file included from" line.
    if (!Skips.empty()) {
      ASTReader::PartSkip Skip;
      Skip.Names = Skips;
      for (size_t K = 0; K != Paths.size(); ++K)
        if (OptionalFileEntryRef FE =
                CI.getFileManager().getOptionalFileRef(Paths[K]))
          Skip.Headers.push_back(
              {&FE->getFileEntry(), K >= Loads.size() || !Loads[K]});
      if (const std::optional<MetadataView> &Deps = Client->lastMetadata())
        if (!resolveProvided(*Deps, Loads, Skip.Swaps)) {
          ++LoadsRefused;
          return nullptr;
        }
      Reader.setPartSkip(std::move(Skip));
    }
    bool Read = Reader.ReadAST(Name, serialization::MK_ExplicitModule,
                               IncludeLoc, ASTReader::ARR_ConfigurationMismatch,
                               &MF) == ASTReader::Success;
    Reader.setPartSkip(std::nullopt);
    if (!Read || !MF) {
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
    LoadedTargets[MF->ModuleName] = {Target, Bytes, MF, /*Fresh=*/true};
    ++CopiesLoaded;
    return Target;
  }

  CompilerInstance &CI;
  std::unique_ptr<CacheClient> Client;
  /// Only to give each loaded entry a name of its own.
  unsigned Loaded = 0;
  /// Copies the module reader accepted.
  unsigned CopiesLoaded = 0;
  /// Each saved copy loaded so far, by module name, for a hit on it again:
  /// the submodule served and the bytes it was read from.
  struct LoadedCopy {
    Module *Target;
    llvm::StringRef Bytes;
    serialization::ModuleFile *MF;
    /// Loaded by the latest hit on it, not reused.
    bool Fresh;
  };
  /// The module name of the copy the latest load returned.
  std::string LastModuleName;
  llvm::StringMap<LoadedCopy> LoadedTargets;
  /// Fitting copies passed over because another copy of the same name was
  /// loaded (see load).
  unsigned OtherCopySameName = 0;
  /// Which warning pragmas are in force, and this compile's warning flags
  /// (warningFlagsHash).
  WarningPragmaTracker &Pragmas;
  uint64_t WarningFlags;
  unsigned DiagnosticsReplayed = 0;
  /// Hashes of every name a loaded copy's macro effects touched.
  llvm::DenseSet<uint64_t> TouchedByCopies;
  unsigned EffectsReplayed = 0, EffectsSkipped = 0;
  bool ReplayAll = false;
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
  /// Listed headers read as text because a header this compile will write
  /// was open.
  unsigned InsideWritten = 0;
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
    /// Time in the headers it included, the cache's work in them excluded.
    uint64_t ChildMicros;
    /// Why the cache did not serve it (LogServed only): not-listed,
    /// inside-a-header-being-written, inside-a-declaration, no-saved-PCM,
    /// rejected:<check>, load-refused, or not-asked when the cache was not
    /// consulted.
    std::string Reason;
  };
  std::string NextReason;
  /// The cache's own time so far: lookups, the loads they made included,
  /// and writing parts.
  uint64_t cacheMicros() const {
    return LookupMicros + ReplayMicros +
           (Exporter ? Exporter->stats().Micros : 0);
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
/// Hands every diagnostic to the consumer the compile had, and to the
/// exporter (PartExporter::diagnostic).
class CapturingConsumer : public DiagnosticConsumer {
public:
  CapturingConsumer(DiagnosticConsumer &Next,
                    std::unique_ptr<DiagnosticConsumer> Owned,
                    PartExporter &Exporter)
      : Next(Next), Owned(std::move(Owned)), Exporter(Exporter) {}

  void BeginSourceFile(const LangOptions &LO, const Preprocessor *PP) override {
    Next.BeginSourceFile(LO, PP);
  }
  void EndSourceFile() override { Next.EndSourceFile(); }
  void finish() override { Next.finish(); }
  void clear() override {
    DiagnosticConsumer::clear();
    Next.clear();
  }
  bool IncludeInDiagnosticCounts() const override {
    return Next.IncludeInDiagnosticCounts();
  }
  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    // Counted here too: the compile's result reads this consumer's counts.
    DiagnosticConsumer::HandleDiagnostic(Level, Info);
    Next.HandleDiagnostic(Level, Info);
    Exporter.diagnostic(Level, Info);
  }

private:
  DiagnosticConsumer &Next;
  std::unique_ptr<DiagnosticConsumer> Owned;
  PartExporter &Exporter;
};

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
  auto Tracker = std::make_unique<WarningPragmaTracker>();
  WarningPragmaTracker &Pragmas = *Tracker;
  CI.getPreprocessor().addPPCallbacks(std::move(Tracker));
  auto Server = std::make_unique<HeaderCacheServer>(CI, std::move(Client),
                                                    Detailed, Pragmas);
  if (Export != ExportMode::None) {
    HeaderCacheServer *S = Server.get();
    PartExporter &E = PartExporter::install(
        CI, [S](PushRequest &&R) { S->push(R); }, &Pragmas);
    S->exportMisses(E, Export == ExportMode::PopulateOnly);
    // The diagnostics printed while a header being exported is open go into
    // its part (REPORT.md Q34).
    DiagnosticsEngine &Diags = CI.getDiagnostics();
    std::unique_ptr<DiagnosticConsumer> Owned = Diags.takeClient();
    DiagnosticConsumer *Next = Owned ? Owned.get() : Diags.getClient();
    Diags.setClient(new CapturingConsumer(*Next, std::move(Owned), E),
                    /*ShouldOwnClient=*/true);
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
    auto Start = std::chrono::steady_clock::now();
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
                 << ", "
                 << std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - Start)
                        .count()
                 << " us\n";
    return Top;
  }

private:
  CompilerInstance &CI;
  std::string Header, In;
  bool Served = false;
};

} // namespace

/// Whether \p CI writes a precompiled file: a Clang module or a precompiled
/// header.
static bool writesPrecompiledFile(const CompilerInstance &CI) {
  if (CI.getLangOpts().isCompilingModule())
    return true;
  switch (CI.getFrontendOpts().ProgramAction) {
  case frontend::GenerateModule:
  case frontend::GenerateModuleInterface:
  case frontend::GenerateReducedModuleInterface:
  case frontend::GenerateHeaderUnit:
  case frontend::GeneratePCH:
    return true;
  default:
    return false;
  }
}

void clang::ast_cache::installHeaderCacheHook(CompilerInstance &CI) {
  // A compile that writes a precompiled file does nothing with the cache.
  // Served a copy, it writes a file that refers to the copy, which lives
  // only in the daemon's memory: a module build served one crashed, and a
  // compile using a precompiled header built with one stopped at "module
  // file '/__hc_memory/0.pcm' not found" (REPORT.md Q10).
  if (writesPrecompiledFile(CI))
    return;

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

  // The build this compile belongs to: within one, the daemon shares what
  // each copy's #include searches found (REPORT.md I9).
  if (const char *Build = std::getenv("CLANG_HEADER_CACHE_BUILD"))
    if (*Build)
      (*Client)->setBuild(llvm::xxh3_64bits(llvm::StringRef(Build)) | 1);

  // 1 prints the counts when the main file ends; 2 also names what failed
  // for each rejected candidate.
  const char *Stats = std::getenv("CLANG_HEADER_CACHE_STATS");
  llvm::StringRef Level = Stats ? Stats : "";
  (*Client)->logRejections(Level == "2" || Level == "3");
  (*Client)->logChecks(Level == "3");
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
