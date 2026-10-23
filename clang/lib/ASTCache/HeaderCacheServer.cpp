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
    const char *Partial = std::getenv("CLANG_HEADER_CACHE_PARTIAL");
    PartialMode =
        !LinkMode && !FoldServed && Partial && llvm::StringRef(Partial) == "1";
    this->Client->partialHits(PartialMode);
    const char *Pieces = std::getenv("CLANG_HEADER_CACHE_PIECES");
    if (!LinkMode && !FoldServed && !PartialMode && Pieces && *Pieces &&
        llvm::StringRef(Pieces) != "0" &&
        !std::getenv("CLANG_HEADER_CACHE_NO_SUBMODULES")) {
      PiecesMode = true;
      ParseWholeBase = llvm::StringRef(Pieces) == "A";
    }
    this->Client->pieces(PiecesMode);
  }

  void report(llvm::raw_ostream &OS) const {
    const CacheClient::Stats &S = Client->stats();
    OS << "header cache: " << S.Hits << " hits / " << S.Lookups << " lookups, "
       << S.CandidatesRejected << " candidates rejected, " << SkippedAtBoundary
       << " not at a declaration boundary, " << NotListed
       << " not on the ask-list, " << InsideWritten
       << " inside a header being written\n";
    if (PiecesMode)
      OS << "header cache pieces: " << S.BaseFits
         << " bases fit with sections needing another version (" << StandInBases
         << " asked again after them: " << AgainServed << " served, "
         << AgainMissed << " not; " << ParsedBases
         << " parsed with them); versions: " << VersionsServed << " served, "
         << VersionsWritten << " written\n";
    for (const std::string &L : ReuseProbes)
      OS << "  reuse " << L << "\n";
    if (PartialMode)
      OS << "header cache partial hits: " << PartialHits
         << " headers asked again after the headers their copy left out, "
         << S.Partial - PartialHits << " not (no stand-in could be made)\n";
    if (MeasureNested)
      OS << "header cache nested asks: " << NestedAsked << " asked, "
         << NestedFit << " a copy fits, " << NestedNoCopy << " no copy, "
         << ms(NestedMicros) << " ms asking and checking\n";
    if (LinkMode)
      OS << "header cache links: " << LinksLoaded << " loaded, "
         << LinksSkipped << " skipped by their guard, " << LinksFailed
         << " copies not used for a link that failed, " << S.Fetches
         << " fetched (" << S.Fetched << " found)\n";
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
         << " headers held as submodules (" << E.Crossing
         << " macros they set read outside them), " << E.HeldBack
         << " held back as the first time their copy would be written, "
         << E.LinksServed << " links to copies served, " << E.LinksWritten
         << " to copies written\n";
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

  bool seenBefore(llvm::StringRef Header, llvm::StringRef Module) {
    return Client->seenBefore(Header, Module);
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
    // CLANG_HEADER_CACHE_DDG, measurement: every header is read as text.
    static const bool Graph = std::getenv("CLANG_HEADER_CACHE_DDG");
    if (Graph) {
      NextReason = "not-listed";
      return nullptr;
    }
    // CLANG_HEADER_CACHE_PIECES: a section of a copy whose base fits, read
    // again for that copy: its versions are asked for, under the copy's
    // header and its own, and on a miss it is written as a new one.
    std::string VersionOf;
    if (PiecesMode) {
      auto It = PendingVersions.find(
          CI.getFileManager().getCanonicalName(File).str());
      if (It != PendingVersions.end()) {
        VersionOf = It->second;
        Path = VersionOf;
        PendingVersions.erase(It);
      }
    }
    if (VersionOf.empty() && !Client->isListed(Path)) {
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
    if (Exporter && !PopulateOnly && Exporter->writing() && !FoldServed &&
        !LinkMode) {
      ++InsideWritten;
      NextReason = "inside-a-header-being-written";
      if (MeasureNested)
        measureNested(File, IncludeLoc, CurDir, Path);
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
    unsigned ModulesBefore =
        CI.getASTReader() ? CI.getASTReader()->getModuleManager().size() : 0;
    MetadataView Deps;
    Module *Target = nullptr;
    IncludeSite Site{File, CurDir, IncludeLoc,
                     warningState(WarningFlags, &Pragmas, IncludeLoc),
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
    // CLANG_HEADER_CACHE_PIECES: a version serves only whole; the header a
    // stand-in includes after its sections has the copy its plan chose.
    // Any other header the stand-in reads serves only whole.
    CacheClient::PiecesStep Step = CacheClient::PiecesStep::Plan;
    if (!VersionOf.empty())
      Step = CacheClient::PiecesStep::WholeOnly;
    else if (PiecesMode && inStandIn(IncludeLoc))
      Step = &File.getFileEntry() == AskAgain
                 ? CacheClient::PiecesStep::Again
                 : CacheClient::PiecesStep::WholeOnly;
    Client->lookup(
        Path, PP, &CI.getSema(), &Deps,
        [&](llvm::StringRef Bytes, llvm::StringRef ModuleName) {
          AnyFits = true;
          if (PopulateOnly)
            return false;
          auto Loading = std::chrono::steady_clock::now();
          // CLANG_HEADER_CACHE_LINK: the copies it links load first.
          if (LinkMode && Client->lastMetadata() &&
              !Client->lastMetadata()->links().empty()) {
            MetadataView Outer = *Client->lastMetadata();
            if (!loadLinks(Outer, IncludeLoc)) {
              ++LinksFailed;
              return false;
            }
          }
          Target = load(Bytes, ModuleName, IncludeLoc);
          HitLoadMicros = micros(Loading);
          HitBytes = Bytes.size();
          LoadMicros += HitLoadMicros;
          if (Target && LogServed)
            Served.push_back(Path + " from " + ModuleName.str());
          return Target != nullptr;
        },
        &Site, Step);
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
    // A section read inside another left its name pending: none is now.
    if (Step == CacheClient::PiecesStep::Again) {
      ++(Target ? AgainServed : AgainMissed);
      PendingVersions.clear();
      AskAgain = nullptr;
    }
    if (!VersionOf.empty() && Target)
      ++VersionsServed;
    if (!Target && !VersionOf.empty()) {
      Log(0);
      ++VersionsWritten;
      // CLANG_HEADER_CACHE_PROBE_REUSE, measurement: whether a copy of the
      // section's header made elsewhere would fit here instead -- one of
      // its own copies, or one of its versions in another base -- and how
      // large the first that fits is.
      if (ProbeReuse) {
        size_t Own = 0, Other = 0;
        auto Fits = [](size_t &Size) {
          return [&Size](llvm::StringRef Bytes, llvm::StringRef) {
            if (!Size)
              Size = Bytes.size();
            return false;
          };
        };
        std::string Canonical =
            CI.getFileManager().getCanonicalName(File).str();
        Client->lookup(storedName(File), PP, &CI.getSema(), nullptr,
                       Fits(Own), &Site, CacheClient::PiecesStep::WholeOnly);
        Client->lookup("*#section:" + Canonical, PP, &CI.getSema(), nullptr,
                       Fits(Other), &Site, CacheClient::PiecesStep::WholeOnly);
        ReuseProbes.push_back(Canonical + " own=" + std::to_string(Own) +
                              " other=" + std::to_string(Other));
      }
      if (LogServed) {
        QueryLines.back() += " version";
        NextReason = "version-written";
      }
      if (Exporter)
        Exporter->exportNext(File, VersionOf);
      return nullptr;
    }
    if (!Target && PiecesMode && !Client->lastPieces().empty() &&
        !inStandIn(IncludeLoc)) {
      Log(0);
      // CLANG_HEADER_CACHE_PIECES: a copy's base fits and some sections need
      // another version. Each is read for that copy -- served as a version
      // that fits, or parsed and written as one -- and this header is not
      // written: its base is stored already.
      // By canonical path, as the stand-in's #include finds each.
      std::string Base = storedName(File);
      FileManager &FM = CI.getFileManager();
      for (const LeftOutRecord &R : Client->lastPieces())
        if (OptionalFileEntryRef FE = FM.getOptionalFileRef(R.Path))
          PendingVersions[FM.getCanonicalName(*FE).str()] =
              Base + "#section:" + R.Path;
      if (LogServed)
        QueryLines.back() +=
            " pieces=" + std::to_string(Client->lastPieces().size());
      // Policy B: they are read first, from a stand-in, then this header is
      // asked for again and its base serves. Policy A, or where no stand-in
      // can say the same: this header is parsed with them.
      if (!ParseWholeBase) {
        FileID F =
            piecesStandIn(File, IncludeLoc, CurDir, Client->lastPieces());
        if (F.isValid()) {
          ++StandInBases;
          PendingStandIn = {&File.getFileEntry(), F};
          AskAgain = &File.getFileEntry();
          return nullptr;
        }
      }
      ++ParsedBases;
      if (LogServed)
        NextReason = "pieces-parsed";
      return nullptr;
    }
    if (!Target) {
      Log(0);
      // CLANG_HEADER_CACHE_PARTIAL: a copy serves once the headers it left
      // out are parsed. They are entered first, from a stand-in file that
      // then includes this header again; this header is not written.
      if (PartialMode && !Client->lastPartial().empty() &&
          !inStandIn(IncludeLoc)) {
        FileID F = standIn(IncludeLoc, Client->lastPartial());
        if (F.isValid()) {
          ++PartialHits;
          PendingStandIn = {&File.getFileEntry(), F};
          if (LogServed)
            QueryLines.back() += " partial=" +
                                 std::to_string(Client->lastPartial().size());
          return nullptr;
        }
      }
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
    // The copy's references to declarations outside it resolve to the ones
    // the check found here, as plain clang's did where it read the header;
    // otherwise each resolves when first read, to the name's latest
    // declaration, which may come after this #include (REPORT.md Q8).
    const LoadedCopy &Copy = LoadedTargets.find(LastModuleName)->second;

    // A header being exported cannot hold a served one: its part would hold
    // the import, not the declarations -- unless it folds the served copy's
    // declarations in as its own (FoldServed).
    bool Folding = Exporter && FoldServed && Exporter->writing() && Copy.MF;
    if (LinkMode && Exporter && Exporter->writing()) {
      Sema &Sem = CI.getSema();
      IdentifierTable &Ids = PP.getIdentifierTable();
      HeaderSearch &HS = PP.getHeaderSearchInfo();
      const IdentifierInfo *G =
          HS.getFileInfo(File).getControllingMacro(HS.getExternalLookup());
      Exporter->linked(Path, LastModuleName, G ? G->getName() : "", Copy.MF,
                       Deps, IncludeLoc, [&](llvm::StringRef Name) {
                         NamedDecl *D = ASTReader::lookupAtFileScope(
                             Sem, Ids.get(Name), /*Tag=*/false,
                             /*CreateBuiltin=*/false);
                         return D ? D->getLocation() : SourceLocation();
                       }, ModulesBefore, llvm::xxh3_64bits(Copy.Bytes));
    } else if (Folding) {
      Sema &Sem = CI.getSema();
      IdentifierTable &Ids = PP.getIdentifierTable();
      Exporter->served(Copy.MF, &Deps, IncludeLoc, [&](llvm::StringRef Name) {
        NamedDecl *D = ASTReader::lookupAtFileScope(
            Sem, Ids.get(Name), /*Tag=*/false, /*CreateBuiltin=*/false);
        return D ? D->getLocation() : SourceLocation();
      });
    } else if (Exporter) {
      Exporter->served();
    }
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
      // A copy folded into one being written replays every effect, so that
      // the copy written holds each one's definition.
      bool Was = ReplayAll;
      ReplayAll = ReplayAll || Folding;
      replayMacroEffects(Deps.macroEffects(), *Target, IncludeLoc, Applies,
                         OwnerOf, Subs);
      ReplayAll = Was;
    }
    {
      llvm::TimeTraceScope S("HeaderCache PragmaDelta");
      applyPragmaDelta(CI.getSema(), Deps.entry(), Deps.exit());
    }
    if (LinkMode && Exporter)
      Exporter->replaying(true);
    printDiagnostics(Deps.diagnostics(), *Copy.MF, Applies);
    if (LinkMode && Exporter)
      Exporter->replaying(false);
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
  FileID enterInstead(FileEntryRef File, SourceLocation) override {
    if (!PendingStandIn || PendingStandIn->first != &File.getFileEntry())
      return FileID();
    FileID F = PendingStandIn->second;
    PendingStandIn.reset();
    return F;
  }

  void headerEntered(OptionalFileEntryRef File) {
    // A stand-in is no header: what it includes is timed as if included
    // where the #include it stands in for was.
    if (File && isStandIn(File->getName()))
      File = std::nullopt;
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

  /// CLANG_HEADER_CACHE_LINK: a copy a link loads, and the links of its
  /// own, all fetched before any loads, so that a missing one leaves the
  /// compile as it was.
  struct Fetched {
    std::string Path, Module, Guard;
    bool OnGuard = false;
    std::vector<std::string> Names;
    llvm::StringRef Bytes;
    std::optional<MetadataView> Meta;
    std::vector<Fetched> Links;
  };
  /// Each copy fetched for a link in this compile, by module name.
  llvm::StringMap<std::pair<llvm::StringRef, MetadataView>> FetchedCopies;

  bool fetchLinks(const MetadataView &Deps, std::vector<Fetched> &Out,
                  unsigned Depth) {
    if (Depth > 64)
      return false;
    for (const LinkView &L : Deps.links()) {
      Fetched F;
      F.Path = L.Path.str();
      F.Module = L.Module.str();
      F.Guard = L.Guard.str();
      F.OnGuard = L.OnGuard;
      for (llvm::StringRef N : L.Names)
        F.Names.push_back(N.str());
      std::string Key = L.Module.str() + "/" + llvm::utohexstr(L.Hash);
      auto Known = FetchedCopies.find(Key);
      if (Known != FetchedCopies.end()) {
        F.Bytes = Known->second.first;
        F.Meta = Known->second.second;
      } else {
        F.Bytes = Client->fetch(L.Path, L.Module, L.Hash, F.Meta);
        if (F.Bytes.empty() || !F.Meta)
          return false;
        FetchedCopies[Key] = {F.Bytes, *F.Meta};
      }
      if (!fetchLinks(*F.Meta, F.Links, Depth + 1))
        return false;
      Out.push_back(std::move(F));
    }
    return true;
  }

  bool guardDefined(llvm::StringRef Guard) {
    Preprocessor &PP = CI.getPreprocessor();
    if (Guard.empty() || !PP.mayBeMacro(Guard))
      return false;
    IdentifierInfo *II = PP.getIdentifierInfo(Guard);
    PP.refreshIdentifier(*II);
    return PP.isMacroDefined(II);
  }
  bool wasRead(llvm::StringRef Path) {
    Preprocessor &PP = CI.getPreprocessor();
    OptionalFileEntryRef FE = PP.getFileManager().getOptionalFileRef(Path);
    return FE && (PP.alreadyIncluded(*FE) ||
                  PP.getHeaderSearchInfo().hasFileBeenImported(*FE));
  }
  /// The declaration a name a copy takes from a link finds here. Looking it
  /// up also reads it from the module files loaded since the name was last
  /// read: the reader of the copy, which finds it by name, does not.
  NamedDecl *findLinkName(llvm::StringRef N) {
    Preprocessor &PP = CI.getPreprocessor();
    bool Tag = N.starts_with("t:");
    IdentifierInfo *II = PP.getIdentifierInfo(N.drop_front(2));
    PP.refreshIdentifier(*II);
    return ASTReader::lookupAtFileScope(CI.getSema(), *II, Tag,
                                        /*CreateBuiltin=*/true);
  }

  bool declaredHere(const Fetched &F) {
    for (const NameView &N : F.Meta->declares())
      if (!findLinkName(N.Name))
        return false;
    for (const Fetched &C : F.Links)
      if (!declaredHere(C))
        return false;
    return true;
  }

  /// Decides, in load order, which links will be skipped: those whose guard
  /// is defined, now or by a link loaded before them. False if one would be
  /// skipped where its header was not read, or where a declaration the copy
  /// takes from it is missing. Decided before anything loads.
  bool planLinks(const std::vector<Fetched> &Links,
                 llvm::StringSet<> &WillLoad) {
    for (const Fetched &F : Links) {
      if (!F.Guard.empty() && WillLoad.count(F.Guard))
        continue;
      if (guardDefined(F.Guard)) {
        if (!wasRead(F.Path))
          return false;
        for (const std::string &N : F.Names)
          if (!findLinkName(N))
            return false;
        // The compile read the header, as plain clang would have; what the
        // copy and its links declare must be there already, or the copies
        // that refer to it were made with a header the compile has not
        // read so far: git's hash.h, open above its struct object_id.
        if (!declaredHere(F))
          return false;
        continue;
      }
      if (!planLinks(F.Links, WillLoad))
        return false;
      if (!F.Guard.empty())
        WillLoad.insert(F.Guard);
    }
    return true;
  }

  /// Loads what \p Deps links, at the #include \p Loc of the copy linking
  /// it, as hits on the headers there would: each copy after its own links,
  /// its effects replayed. A link whose guard is defined is skipped with
  /// what it links, as the #include would be.
  bool loadLinks(const MetadataView &Deps, SourceLocation Loc) {
    std::vector<Fetched> All;
    if (!fetchLinks(Deps, All, 0))
      return false;
    llvm::StringSet<> WillLoad;
    if (!planLinks(All, WillLoad))
      return false;
    for (const Fetched &F : All)
      if (!loadLink(F, Loc))
        return false;
    for (const Fetched &F : All)
      for (const std::string &N : F.Names)
        findLinkName(N);
    return true;
  }

  bool loadLink(const Fetched &F, SourceLocation Loc) {
    Preprocessor &PP = CI.getPreprocessor();
    if (guardDefined(F.Guard)) {
      if (!wasRead(F.Path))
        return false;
      if (getenv("DEBUG_LINK"))
        llvm::errs() << "LINK skip " << F.Path << " " << F.Module << "\n";
      ++LinksSkipped;
      return true;
    }
    for (const Fetched &C : F.Links)
      if (!loadLink(C, Loc))
        return false;
    for (const Fetched &C : F.Links)
      for (const std::string &N : C.Names)
        findLinkName(N);
    Module *M = load(F.Bytes, F.Module, Loc);
    if (getenv("DEBUG_LINK")) {
      llvm::errs() << "LINK load " << F.Path << " " << F.Module
                   << (M ? "" : " FAILED") << " sublinks:";
      for (const Fetched &C : F.Links) {
        llvm::errs() << " " << llvm::sys::path::filename(C.Path) << "[";
        for (const std::string &N : C.Names)
          llvm::errs() << N << ",";
        llvm::errs() << "]";
      }
      llvm::errs() << " declares " << F.Meta->declares().size() << "\n";
    }
    if (!M)
      return false;
    serialization::ModuleFile *MF = LoadedTargets.find(F.Module)->second.MF;
    const MetadataView &D = *F.Meta;
    replayMacroStacks(D.macroStacks(), Loc);
    // As the #include's own hit does, through Sema: visible, and imported,
    // so that a definition the copy must emit is emitted here.
    CI.getSema().BuildModuleInclude(Loc, M);
    // Its macros too, which only the preprocessor shows (as for a hit).
    PP.makeModuleVisible(M, Loc);
    replayMacroEffects(D.macroEffects(), *M, Loc);
    applyPragmaDelta(CI.getSema(), D.entry(), D.exit());
    if (Exporter)
      Exporter->replaying(true);
    if (MF)
      printDiagnostics(D.diagnostics(), *MF);
    if (Exporter)
      Exporter->replaying(false);
    for (const WarningPragmaView &W : D.warningPragmas())
      Pragmas.apply(CI.getDiagnostics(), W.Pragma, Loc);
    if (OptionalFileEntryRef FE = PP.getFileManager().getOptionalFileRef(F.Path))
      PP.markIncluded(*FE);
    ++LinksLoaded;
    return true;
  }
  unsigned LinksLoaded = 0, LinksSkipped = 0, LinksFailed = 0;

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
  /// CLANG_HEADER_CACHE_PIECES=1 (or B), =A: where a copy's base fits and
  /// some sections need another version, those sections are read for it
  /// first, from a stand-in, and the header asked for again (B); or the
  /// header is parsed with them (A). Either way each such section is served
  /// as a version of the copy that fits, or written as a new one.
  bool PiecesMode = false, ParseWholeBase = false;
  unsigned AgainServed = 0, AgainMissed = 0;
  /// The base a pieces stand-in asks for again, after its sections.
  const FileEntry *AskAgain = nullptr;
  bool ProbeReuse = std::getenv("CLANG_HEADER_CACHE_PROBE_REUSE");
  std::vector<std::string> ReuseProbes;
  unsigned StandInBases = 0, ParsedBases = 0, VersionsServed = 0,
           VersionsWritten = 0;
  /// The sections to read as versions, by canonical path, and the name their
  /// versions are stored under.
  std::map<std::string, std::string> PendingVersions;

  /// The #include at \p IncludeLoc, spelled as written with its delimiters,
  /// or empty where no stand-in can say the same: one a macro wrote, or an
  /// #include_next or #import.
  std::string spelledInclude(SourceLocation IncludeLoc) {
    SourceManager &SM = CI.getSourceManager();
    if (IncludeLoc.isMacroID())
      return {};
    bool Invalid = false;
    llvm::StringRef Buffer =
        SM.getBufferData(SM.getFileID(IncludeLoc), &Invalid);
    if (Invalid)
      return {};
    size_t At = SM.getFileOffset(IncludeLoc);
    if (At >= Buffer.size() || (Buffer[At] != '"' && Buffer[At] != '<'))
      return {};
    size_t End = Buffer.find(Buffer[At] == '<' ? '>' : '"', At + 1);
    size_t Line = Buffer.rfind('\n', At);
    Line = Line == llvm::StringRef::npos ? 0 : Line + 1;
    if (End == llvm::StringRef::npos || Buffer.slice(At, End).contains('\n'))
      return {};
    llvm::StringRef Directive = Buffer.slice(Line, At);
    if (!Directive.contains("include") || Directive.contains("include_next"))
      return {};
    return Buffer.slice(At, End + 1).str();
  }

  /// A file in the includer's directory holding \p Text, standing in for
  /// the #include at \p IncludeLoc.
  FileID makeStandIn(SourceLocation IncludeLoc, const std::string &Text) {
    SourceManager &SM = CI.getSourceManager();
    OptionalFileEntryRef IncluderFE =
        SM.getFileEntryRefForID(SM.getFileID(IncludeLoc));
    if (!IncluderFE)
      return FileID();
    llvm::SmallString<256> Path(
        llvm::sys::path::parent_path(IncluderFE->getName()));
    llvm::sys::path::append(Path,
                            "__hc_partial_" + std::to_string(++StandIns) + ".h");
    FileEntryRef FE =
        CI.getFileManager().getVirtualFileRef(Path, Text.size(), 0);
    SM.overrideFileContents(FE,
                            llvm::MemoryBuffer::getMemBufferCopy(Text, Path));
    SrcMgr::CharacteristicKind Kind = SM.getFileCharacteristic(IncludeLoc);
    CI.getPreprocessor().getHeaderSearchInfo().getFileInfo(FE).DirInfo = Kind;
    return SM.createFileID(FE, IncludeLoc, Kind);
  }

  /// CLANG_HEADER_CACHE_PIECES: a stand-in for the #include of \p Header at
  /// \p IncludeLoc, in the header's own directory: it includes each section
  /// of \p Need as the header's lines spell it, so each is found by the same
  /// search and opened by the same name, then the header by its file name,
  /// which opens it by the name it has. Invalid where that cannot be: an
  /// #include written by a macro, an #include_next or #import, or a header
  /// opened by a name other than its directory's and its file's.
  FileID piecesStandIn(FileEntryRef Header, SourceLocation IncludeLoc,
                       ConstSearchDirIterator CurDir,
                       const std::vector<LeftOutRecord> &Need) {
    SourceManager &SM = CI.getSourceManager();
    Preprocessor &PP = CI.getPreprocessor();
    if (spelledInclude(IncludeLoc).empty())
      return FileID();
    llvm::StringRef HDir = Header.getDir().getName();
    llvm::StringRef File = llvm::sys::path::filename(Header.getName());
    llvm::SmallString<256> Self(HDir);
    llvm::sys::path::append(Self, File);
    if (Self.str() != Header.getName())
      return FileID();
    std::string Text;
    for (const LeftOutRecord &R : Need) {
      if (R.Angled) {
        // Found by the search from its start wherever it is written.
        ConstSearchDirIterator Dir = nullptr;
        OptionalFileEntryRef Found = PP.LookupFile(
            IncludeLoc, R.Spelled, /*isAngled=*/true, /*FromDir=*/nullptr,
            /*FromFile=*/nullptr, &Dir, nullptr, nullptr, nullptr, nullptr,
            nullptr);
        if (!Found || CI.getFileManager().getCanonicalName(*Found) != R.Path)
          return FileID();
        Text += "#include <" + R.Spelled + ">\n";
      } else {
        Text += "#include \"" + R.Spelled + "\"\n";
      }
    }
    Text += "#include \"" + File.str() + "\"\n";
    llvm::SmallString<256> Path(HDir);
    llvm::sys::path::append(Path,
                            "__hc_partial_" + std::to_string(++StandIns) + ".h");
    FileEntryRef FE =
        CI.getFileManager().getVirtualFileRef(Path, Text.size(), 0);
    SM.overrideFileContents(FE,
                            llvm::MemoryBuffer::getMemBufferCopy(Text, Path));
    // A file a quoted #include finds beside its includer is of the
    // includer's kind: the stand-in is of the header's, so that the header
    // and its sections are what they were.
    SrcMgr::CharacteristicKind Kind = CurDir
                                          ? CurDir->getDirCharacteristic()
                                          : SM.getFileCharacteristic(IncludeLoc);
    PP.getHeaderSearchInfo().getFileInfo(FE).DirInfo = Kind;
    return SM.createFileID(FE, IncludeLoc, Kind);
  }

  /// CLANG_HEADER_CACHE_PARTIAL=1: where a copy fails only on what headers
  /// it left out gave it, and the compile has not read them, they are parsed
  /// first and the header asked for again (a partial hit).
  bool PartialMode = false;
  unsigned PartialHits = 0, StandIns = 0;
  /// The header whose #include the stand-in made for it replaces.
  std::optional<std::pair<const FileEntry *, FileID>> PendingStandIn;
  static bool isStandIn(llvm::StringRef Name) {
    return llvm::sys::path::filename(Name).starts_with("__hc_partial_");
  }
  bool inStandIn(SourceLocation Loc) {
    SourceManager &SM = CI.getSourceManager();
    OptionalFileEntryRef FE =
        SM.getFileEntryRefForID(SM.getFileID(SM.getExpansionLoc(Loc)));
    return FE && isStandIn(FE->getName());
  }

  /// A file standing in for the #include at \p IncludeLoc: it includes each
  /// header of \p Plug, then the header as the #include spelled it. It sits
  /// in the includer's directory, so that a quoted #include searches from
  /// where the includer's did. A left-out header spelled with angle
  /// brackets is included as spelled, where an #include_next inside it
  /// continues from where it is found; it must find the same file. Invalid
  /// where no stand-in can say the same: an #include written by a macro, or
  /// an #include_next or #import.
  FileID standIn(SourceLocation IncludeLoc,
                 const std::vector<LeftOutRecord> &Plug) {
    SourceManager &SM = CI.getSourceManager();
    Preprocessor &PP = CI.getPreprocessor();
    if (IncludeLoc.isMacroID())
      return FileID();
    FileID Includer = SM.getFileID(IncludeLoc);
    bool Invalid = false;
    llvm::StringRef Buffer = SM.getBufferData(Includer, &Invalid);
    if (Invalid)
      return FileID();
    size_t At = SM.getFileOffset(IncludeLoc);
    if (At >= Buffer.size() || (Buffer[At] != '"' && Buffer[At] != '<'))
      return FileID();
    size_t End = Buffer.find(Buffer[At] == '<' ? '>' : '"', At + 1);
    size_t Line = Buffer.rfind('\n', At);
    Line = Line == llvm::StringRef::npos ? 0 : Line + 1;
    if (End == llvm::StringRef::npos ||
        Buffer.slice(At, End).contains('\n'))
      return FileID();
    llvm::StringRef Directive = Buffer.slice(Line, At);
    if (!Directive.contains("include") || Directive.contains("include_next"))
      return FileID();
    std::string Text;
    for (const LeftOutRecord &R : Plug) {
      if (!R.Angled) {
        Text += "#include \"" + R.Path + "\"\n";
        continue;
      }
      ConstSearchDirIterator CurDir = nullptr;
      OptionalFileEntryRef Found = PP.LookupFile(
          IncludeLoc, R.Spelled, /*isAngled=*/true, /*FromDir=*/nullptr,
          /*FromFile=*/nullptr, &CurDir, /*SearchPath=*/nullptr,
          /*RelativePath=*/nullptr, /*SuggestedModule=*/nullptr,
          /*IsMapped=*/nullptr, /*IsFrameworkFound=*/nullptr);
      if (!Found || CI.getFileManager().getCanonicalName(*Found) != R.Path)
        return FileID();
      Text += "#include <" + R.Spelled + ">\n";
    }
    Text += "#include " + Buffer.slice(At, End + 1).str() + "\n";
    OptionalFileEntryRef IncluderFE = SM.getFileEntryRefForID(Includer);
    if (!IncluderFE)
      return FileID();
    llvm::SmallString<256> Path(
        llvm::sys::path::parent_path(IncluderFE->getName()));
    llvm::sys::path::append(Path,
                            "__hc_partial_" + std::to_string(++StandIns) + ".h");
    FileEntryRef FE =
        CI.getFileManager().getVirtualFileRef(Path, Text.size(), 0);
    SM.overrideFileContents(FE,
                            llvm::MemoryBuffer::getMemBufferCopy(Text, Path));
    // A file an #include searches from has the header search's record of
    // it, which an #include of the file itself would have made: user or
    // system, as its includer is.
    SrcMgr::CharacteristicKind Kind = SM.getFileCharacteristic(IncludeLoc);
    PP.getHeaderSearchInfo().getFileInfo(FE).DirInfo = Kind;
    return SM.createFileID(FE, IncludeLoc, Kind);
  }

  /// CLANG_HEADER_CACHE_FOLD_SERVED=1: inside a header being written, a
  /// header is asked for and served too, and the copy being written holds
  /// the served copy's declarations as its own.
  bool FoldServed = [] {
    const char *V = std::getenv("CLANG_HEADER_CACHE_FOLD_SERVED");
    return V && llvm::StringRef(V) == "1";
  }();
  /// CLANG_HEADER_CACHE_MEASURE_NESTED=1, measurement only: inside a header
  /// being written, ask for each listed header too and check its copies,
  /// without using one. Each such read's reason says whether one fitted.
  /// CLANG_HEADER_CACHE_LINK=1: inside a header being written, a header is
  /// asked for and served too, and the copy being written links the served
  /// copy, as it links the copy of a header inside it that it writes.
  bool LinkMode = [] {
    const char *V = std::getenv("CLANG_HEADER_CACHE_LINK");
    return V && llvm::StringRef(V) == "1";
  }();
  bool MeasureNested = [] {
    const char *V = std::getenv("CLANG_HEADER_CACHE_MEASURE_NESTED");
    return V && llvm::StringRef(V) == "1";
  }();
  unsigned NestedAsked = 0, NestedFit = 0, NestedNoCopy = 0;
  uint64_t NestedMicros = 0;
  void measureNested(FileEntryRef File, SourceLocation IncludeLoc,
                     ConstSearchDirIterator CurDir, const std::string &Path) {
    Preprocessor &PP = CI.getPreprocessor();
    DeclBoundaryObserver *Boundary = PP.getDeclBoundaryObserver();
    if (!Boundary || !Boundary->isAtDeclarationBoundary() || !CI.hasSema()) {
      NextReason += "/not-at-a-boundary";
      return;
    }
    uint32_t Counter = PP.getCounterValue();
    IncludeSite Site{File, CurDir, IncludeLoc,
                     warningState(WarningFlags, &Pragmas, IncludeLoc),
                     searchContext(File, CurDir)};
    bool Fits = false;
    auto Asked = std::chrono::steady_clock::now();
    Client->lookup(
        Path, PP, &CI.getSema(), nullptr,
        [&](llvm::StringRef, llvm::StringRef) {
          Fits = true;
          return false;
        },
        &Site);
    NestedMicros += micros(Asked);
    PP.setCounterValue(Counter);
    ++NestedAsked;
    if (Fits) {
      ++NestedFit;
      NextReason += "/fits";
    } else if (Client->lastCandidates() == 0) {
      ++NestedNoCopy;
      NextReason += "/no-copy";
    } else {
      NextReason += "/rejected:" + describe(Client->lastRejection()).str();
    }
  }
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
  auto Tracker =
      std::make_unique<WarningPragmaTracker>(CI.getPreprocessor(),
                                             CI.getDiagnosticOpts());
  WarningPragmaTracker &Pragmas = *Tracker;
  CI.getPreprocessor().addPPCallbacks(std::move(Tracker));
  auto Server = std::make_unique<HeaderCacheServer>(CI, std::move(Client),
                                                    Detailed, Pragmas);
  if (Export != ExportMode::None) {
    HeaderCacheServer *S = Server.get();
    PartExporter &E = PartExporter::install(
        CI, [S](PushRequest &&R) { S->push(R); }, &Pragmas);
    S->exportMisses(E, Export == ExportMode::PopulateOnly);
    if (const char *V = std::getenv("CLANG_HEADER_CACHE_SECOND_MISS");
        V && llvm::StringRef(V) == "1")
      E.writeOnlyIfSeen([S](llvm::StringRef Header, llvm::StringRef Module) {
        return S->seenBefore(Header, Module);
      });
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
