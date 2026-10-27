//===- CacheClient.h -------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The compiler's side of the cache: asks the daemon what it has for a header,
// decides whether any of it applies to this compile, and hands back the bytes
// to load.
//
// All the judgement lives here rather than in the daemon. The daemon matches
// on filename and returns every entry it holds; deciding between them needs
// this compile's live macro table and its Sema, which the daemon does not
// have and could not be sent cheaply.
//
// Checks run cheapest-first and stop at the first mismatch, because a header
// with a hundred conditional dependencies usually fails on one of them, and
// the symbol checks are the only ones that cost a name lookup.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_CACHECLIENT_H
#define LLVM_CLANG_ASTCACHE_CACHECLIENT_H

#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/SharedRegion.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Lex/HeaderSearch.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class raw_socket_stream;
}

namespace clang {
class Decl;
class NamedDecl;
class Preprocessor;
class Sema;

namespace ast_cache {

/// Why a candidate entry was rejected. Reported rather than swallowed: "the
/// cache never helps" and "the cache is checking the wrong thing" look
/// identical without this.
enum class MismatchReason {
  None,
  Conditional,
  Syntactic,
  Symbol,
  Inclusion,
  PragmaState,
  FileName,
  Include,
  Declaration,
  FileChanged,
  WarningState,
  MacroStack,
  GuardWithoutHeader,
};

llvm::StringRef describe(MismatchReason R);

/// Where the compile is about to include the header: the file, under the
/// name the compile found it by, the search directory it was found in
/// (null when found without searching), and the #include's location.
/// A hit redoes the header's own #include lookups from here.
struct IncludeSite {
  FileEntryRef File;
  ConstSearchDirIterator Dir;
  SourceLocation Loc;
  /// The compile's warningState at the #include.
  uint64_t WarningState = 0;
  /// What the header's own #include searches depend on besides the files
  /// present: the compile's include path and where this one found the
  /// header (REPORT.md I9). See searchContext in HeaderCacheServer.cpp.
  uint64_t SearchContext = 0;
  /// Whether the names this compile opens files by matter beyond __FILE__,
  /// which is compared apart: debug information repeats them.
  bool NamesMatter = true;
};

/// The outcome of a candidate's #include searches as earlier compiles of the
/// build found it, and as this check finds it (REPORT.md I9).
struct SearchMemo {
  /// The outcome known under a search key, if any.
  llvm::function_ref<std::optional<uint8_t>(uint64_t Key)> Recall;
  /// What the check found where it made searches: one per group of a
  /// grouped copy (HeaderDependencySet::Grouped) it searched.
  llvm::SmallVector<SearchOutcome, 2> Found;
  /// Set when the check compared the copy's files with the file system,
  /// under FilesKey: within a build files do not change, so later compiles
  /// of the build take the outcome instead of looking again.
  std::optional<SearchOutcome> FilesFound;
  /// The file this compile has entered whose canonical path is the one
  /// given, if any (REPORT.md I9): what a check that does not look at the
  /// file system uses to build the search key.
  llvm::function_ref<OptionalFileEntryRef(llvm::StringRef CanonicalPath)>
      FindOpen;
  /// The name the compile has the file whose canonical path is the one
  /// given by: opened as text, or held by a copy it served, whose names
  /// its check compared. None if neither.
  llvm::function_ref<std::optional<llvm::StringRef>(llvm::StringRef)> NameOf;
  /// The key FilesFound is kept under, beside the search keys.
  static constexpr uint64_t FilesKey = 0x46494c45534f4b31ULL;
};

/// Whether the metadata \p Deps still holds in this compile, read in place.
/// \p S must be non-null: all metadata carries pragma state and only Sema
/// holds the live value, so a null Sema rejects every candidate rather than
/// passing checks it cannot run.
///
/// \p Detail, when non-null, receives what failed, e.g. "LIMIT was not
/// defined, is".
///
/// \p Site is where the header is being included. The metadata's #includes
/// and __FILE__s are checked from there, and metadata with either is
/// rejected without one.
///
/// Where checking candidates spends its time, one entry per step, in the
/// order matchesLiveState runs them.
struct CheckCost {
  enum Step {
    /// Each name an #if or #ifdef tested is still defined, or not.
    Conditions,
    /// Each macro the header expanded expands to the same tokens, each name
    /// it read as plain text is still not a macro, and each #include it
    /// skipped is still skippable.
    Macros,
    /// Each name that decided a parse (T * p;) is still a type, the same
    /// one, and #pragma pack and the floating-point pragmas are unchanged.
    TypeNamesAndPragmas,
    /// Each #include in the header, searched for again in this compile's
    /// -I directories, finds the same file.
    HeaderSearches,
    /// Each __FILE__ in the header gives the same text.
    FileNames,
    /// Each file the part holds has the size and modification time it had
    /// when the part was saved.
    FileStamps,
    /// Each outside declaration the part refers to by name has the same
    /// fingerprint in this compile.
    Declarations,
    NumSteps
  };
  uint64_t Nanos[NumSteps] = {};
  unsigned SearchesRedone = 0;
  /// Candidates whose searches an earlier compile of the build made.
  unsigned SearchesReused = 0;
  unsigned FilesCompared = 0;
  /// Elements read, section by section.
  uint64_t Read[MetadataView::NumSections] = {};
};

/// \p Cost, when non-null, is increased by the time each step takes.
/// \p Found, when non-null, receives the declaration each of
/// Deps.declarations() finds here, in order. \p Memo, when non-null, offers
/// the searches' outcome from earlier compiles and receives this one's.
/// \p SkipConditionals, for measurement only, leaves out the definedness
/// tests (REPORT.md T15).
///
/// A copy holding headers as submodules (REPORT.md T17) loads each whose
/// header the compile has not read -- its guard undefined, or its #pragma
/// once file not included -- inside one that loads; each element of its
/// metadata applies by its condition. \p Loaded, when non-null, receives
/// which submodules load.
///
/// \p Plugged, when non-null, says which headers the copy left out
/// (CLANG_HEADER_CACHE_PARTIAL) are to be parsed first: what the copy takes
/// from one of them is not checked.
///
/// CLANG_HEADER_CACHE_PIECES: which sections of a copy fail, where its
/// base -- the header's own lines -- fits.
struct SectionFailures {
  /// Per submodule.
  std::vector<bool> Failing;
  /// The first failure, and with a Detail asked for, what failed.
  MismatchReason First = MismatchReason::None;
  std::string Detail;
};

/// \p Sections, when non-null, checks a copy whose headers are sections
/// (CLANG_HEADER_CACHE_PIECES) in the same pass for its base: what only a
/// section's files read failing marks that section in \p Sections, and the
/// check goes on without the section; anything else failing refuses the
/// copy. None then means the base fits.
///
/// \p Definedness, when non-null, for measurement: every definedness test
/// that fails outside a failing section is added to it ("NAME+" for "was
/// not defined, is", "NAME-" for the opposite) and the check goes on; the
/// result is then the first other failure, else Conditional if any test
/// failed.
///
/// \p StateOnly checks again a copy checked before in the same compile,
/// after headers were read: its files, and what its #includes find, are as
/// they were, and are not looked at. \p Prior, when non-null, is what that
/// check found for each of the copy's declarations, with the declaration's
/// latest redeclaration then: one the same name still finds, with no
/// redeclaration since, is as it was.
MismatchReason matchesLiveState(const MetadataView &Deps, Preprocessor &PP,
                                Sema *S, std::string *Detail = nullptr,
                                const IncludeSite *Site = nullptr,
                                CheckCost *Cost = nullptr,
                                std::vector<NamedDecl *> *Found = nullptr,
                                SearchMemo *Memo = nullptr,
                                bool SkipConditionals = false,
                                std::vector<bool> *Loaded = nullptr,
                                const std::vector<bool> *Plugged = nullptr,
                                SectionFailures *Sections = nullptr,
                                std::vector<std::string> *Definedness = nullptr,
                                bool StateOnly = false,
                                const std::vector<std::pair<const NamedDecl *,
                                                            const Decl *>>
                                    *Prior = nullptr);

class CacheClient {
public:
  /// Connects, asks the daemon which region file it writes, and attaches to
  /// that file. A non-empty \p RegionPath is attached instead, for a test
  /// that wants to name it. Either way the file must be the daemon's own
  /// instance (HelloReply), or nothing the daemon says about offsets can be
  /// trusted and connecting fails. Fails too if no daemon is running, which
  /// is an ordinary condition: the compiler then behaves exactly as it would
  /// without a cache.
  static llvm::Expected<std::unique_ptr<CacheClient>>
  connect(llvm::StringRef SocketPath = DefaultSocketPath,
          llvm::StringRef RegionPath = {});

  ~CacheClient();

  /// The stored result for \p Filename that applies to this compile, or an
  /// empty StringRef if there is none. The returned bytes live in the shared
  /// region and stay valid for this process's lifetime -- the region is
  /// append-only, so nothing rewrites them.
  ///
  /// \p Matched, when non-null, receives the matched entry's metadata, in
  /// the region, whose pragma state, macro effects and diagnostics the
  /// caller needs to replay the header's effect on the compile after
  /// loading.
  ///
  /// \p Use, when given, is offered each candidate that passes the check, in
  /// order, with its module name, and the first it accepts is the result. A
  /// candidate the check cannot rule out can still be unusable -- built with
  /// other compiler options, say -- and a later one may serve.
  ///
  /// \p Site is where the header is being included, as matchesLiveState
  /// takes it.
  ///
  /// CLANG_HEADER_CACHE_PIECES, \p Step: whether a copy whose base fits
  /// may give a plan, lastPieces(); or only a copy that fits whole serves,
  /// for a version of a section; or \p Filename is asked again after its
  /// plan's sections were read, and only the copy the plan chose is checked,
  /// with no request to the daemon.
  enum class PiecesStep { Plan, WholeOnly, Again };
  llvm::StringRef lookup(llvm::StringRef Filename, Preprocessor &PP, Sema *S,
                         MetadataView *Matched = nullptr,
                         llvm::function_ref<bool(llvm::StringRef Bytes,
                                                 llvm::StringRef ModuleName)>
                             Use = nullptr,
                         const IncludeSite *Site = nullptr,
                         PiecesStep Step = PiecesStep::Plan);

  /// The stored copy of \p Filename whose module is \p ModuleName, and its
  /// metadata in \p Meta, with no check: a copy that links it
  /// (CLANG_HEADER_CACHE_LINK) was checked for both. Empty if the daemon
  /// holds none.
  llvm::StringRef fetch(llvm::StringRef Filename, llvm::StringRef ModuleName,
                        uint64_t Hash, std::optional<MetadataView> &Meta);

  /// CLANG_HEADER_CACHE_SECOND_MISS: whether a compile was about to write
  /// the copy of \p Filename named \p ModuleName before; the daemon notes
  /// this one. True if the daemon cannot say, so that the copy is written.
  bool seenBefore(llvm::StringRef Filename, llvm::StringRef ModuleName);

  /// Stores a newly produced entry. Blocks until the daemon acknowledges, so
  /// a caller that does not want to wait should run this off the critical
  /// path.
  bool push(const PushRequest &Req);

  /// Whether \p Path is on the daemon's ask-list, the headers some compile
  /// found slow enough to be worth a saved copy (DECISIONS.md 2.14). Read
  /// from the shared region, with no socket traffic; the parsed list is kept
  /// until the daemon publishes a new one.
  bool isListed(llvm::StringRef Path);

  /// Sends the times this compile took to read its headers. Blocks until the
  /// daemon acknowledges, which is one round trip at the end of the main file.
  bool reportTimings(const TimingReport &Report);

  struct Stats {
    unsigned Lookups = 0;
    unsigned Hits = 0;
    /// CLANG_HEADER_CACHE_PARTIAL: lookups that found a copy usable once the
    /// headers it left out are parsed first.
    unsigned Partial = 0;
    /// CLANG_HEADER_CACHE_PIECES: lookups that found a copy whose base fits
    /// and some of whose sections need another version.
    unsigned BaseFits = 0;
    /// Copies fetched by name for a link, and how many the daemon held.
    unsigned Fetches = 0, Fetched = 0;
    unsigned CandidatesRejected = 0;
    unsigned RejectedFor[13] = {};
    /// With logRejections on: the header and what failed, one per rejected
    /// candidate, such as "/usr/include/stdio.h: __need_size_t was defined,
    /// is not".
    std::vector<std::string> Rejections;
    /// With logChecks on: one line per candidate checked, with where the
    /// check stopped and the time each step took.
    std::vector<std::string> Checks;
    /// Wall time, in microseconds: waiting for the daemon to answer lookups,
    /// checking candidates against the live state, sending this compile's
    /// header timings, and connecting.
    uint64_t DaemonMicros = 0, CheckMicros = 0, ReportMicros = 0,
             ConnectMicros = 0;
    /// Candidates checked, and the check's time step by step.
    unsigned CandidatesChecked = 0;
    CheckCost Check;
  };
  const Stats &stats() const { return Counters; }

  /// What the latest lookup found: how many saved PCMs the daemon returned,
  /// and why the first rejected one was rejected (None if none was).
  unsigned lastCandidates() const { return LastCandidates; }
  MismatchReason lastRejection() const { return LastRejection; }
  /// Which submodules of the latest lookup's match load (REPORT.md T17),
  /// and their names.
  const std::vector<bool> &lastSubmodules() const { return LastSubmodules; }
  const std::vector<std::string> &lastSubmoduleNames() const {
    return LastSubmoduleNames;
  }
  const std::vector<std::string> &lastSubmodulePaths() const {
    return LastSubmodulePaths;
  }
  /// CLANG_HEADER_CACHE_PARTIAL: where the latest lookup found no copy,
  /// the headers a copy left out that, parsed first, would let it serve,
  /// in order; empty for none.
  const std::vector<LeftOutRecord> &lastPartial() const { return LastPartial; }
  void partialHits(bool On) { PartialMode = On; }
  /// CLANG_HEADER_CACHE_PIECES: where the latest lookup found no copy, the
  /// sections of a copy whose base fits that need another version -- one
  /// whose own version does not fit, or one the copy has none of -- in the
  /// order the header includes them; empty for none.
  const std::vector<LeftOutRecord> &lastPieces() const { return LastPieces; }
  void pieces(bool On) { PiecesMode = On; }

  /// The latest lookup's match's metadata, read in place.
  const std::optional<MetadataView> &lastMetadata() const {
    return LastMetadata;
  }

  /// The declaration each outside declaration of the latest lookup's match
  /// found where the header is included, in the order of its
  /// declarations(); null for one that does not apply.
  llvm::ArrayRef<NamedDecl *> matchedDeclarations() const {
    return MatchedDecls;
  }

  /// The build this compile belongs to (CLANG_HEADER_CACHE_BUILD), 0 for
  /// none. Within one build the outcome of a copy's #include searches is
  /// shared through the daemon (REPORT.md I9).
  void setBuild(uint64_t B) { Build = B; }

  /// Keep a line per rejected candidate naming what failed. Off by default:
  /// it builds a string for every rejection.
  void logRejections(bool On) { LogRejections = On; }
  bool loggingRejections() const { return LogRejections; }
  void logChecks(bool On) { LogChecks = On; }

private:
  CacheClient() = default;

  std::unique_ptr<llvm::raw_socket_stream> Conn;
  SharedRegion Region;
  Stats Counters;
  bool LogRejections = false;
  bool LogChecks = false;
  bool PartialMode = false;
  std::vector<LeftOutRecord> LastPartial;
  bool PiecesMode = false;
  std::vector<LeftOutRecord> LastPieces;
  /// The header the latest plan is for, and the copy it chose.
  std::optional<std::pair<std::string, CacheEntryRef>> Planned;
  /// The versions of its copies' sections, which came with the copies, by
  /// the name they are stored under: asked for, they need no request.
  llvm::StringMap<std::vector<CacheEntryRef>> PlannedVersions;
  /// What the chosen copy's declarations found when it was planned.
  std::vector<std::pair<const NamedDecl *, const Decl *>> PlannedDecls;
  /// CLANG_HEADER_CACHE_PIECES: by canonical path, the name each file a
  /// served copy holds has in this compile.
  llvm::StringMap<std::string> ServedNames;
  unsigned LastCandidates = 0;
  MismatchReason LastRejection = MismatchReason::None;
  std::vector<bool> LastSubmodules;
  std::vector<std::string> LastSubmoduleNames, LastSubmodulePaths;
  std::optional<MetadataView> LastMetadata;
  std::vector<NamedDecl *> MatchedDecls;
  /// The ask-list as last parsed, and the version it was parsed from.
  llvm::StringSet<> AskList;
  uint64_t AskListVersion = 0;
  /// See setBuild. The searches' outcomes this compile found, by where the
  /// copy's metadata is and the search key, and those not yet sent to the
  /// daemon.
  uint64_t Build = 0;
  llvm::DenseMap<std::pair<uint64_t, uint64_t>, uint8_t> Outcomes;
  /// The copies used since the last message, by metadata offset.
  std::vector<uint64_t> Used;

  /// The files this compile has entered, by file name, as far as
  /// OpenSeenUpTo of the source manager's offsets; for SearchMemo::FindOpen.
  llvm::StringMap<llvm::SmallVector<FileEntryRef, 1>> OpenByName;
  llvm::DenseSet<const FileEntry *> OpenSeen;
  unsigned OpenSeenUpTo = 0;
  std::vector<LearnedSearch> Learned;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHECLIENT_H
