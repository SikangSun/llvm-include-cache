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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
  Counter,
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
  /// The fingerprint of each live declaration the compile's checks have
  /// computed, with the latest redeclaration it covered and whether the
  /// struct, union or enum it describes was complete: one computed again
  /// only where either changed. Every copy of a header names much the same
  /// outside declarations, and each copy is checked against them. Kept
  /// apart by what is in them: 1, a struct's layout (LayoutPart); 2, a
  /// function's masked hash.
  struct Print {
    const Decl *Latest = nullptr;
    bool Complete = false;
    std::optional<std::string> Fingerprint;
  };
  llvm::DenseMap<std::pair<const NamedDecl *, unsigned>, Print> *Prints =
      nullptr;
  /// The names the compile made from __COUNTER__ values, by its own text
  /// or in the copies it loaded (declarationFingerprint).
  const llvm::DenseSet<const IdentifierInfo *> *CounterNames = nullptr;
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
///
/// \p Others, when non-null with \p Definedness, for measurement: every
/// expansion, plain-text name, type name and outside declaration that
/// differs is added to it too, as "E", "N", "Y" or "D" and its index in its
/// list, and the check goes on; the result is still the first of them.
///
/// \p Tolerated, when non-null (CLANG_HEADER_CACHE_GUARD_PLUS=1): a test
/// that finds read a header the copy holds (HeldHeader) -- its guard
/// defined, or its #pragma once file read -- does not fail the check where
/// the compile read the header as the copy holds it and nothing the header
/// read changed since. It receives how many such headers the copy holds a
/// second time.
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
                                    *Prior = nullptr,
                                std::vector<std::string> *Others = nullptr,
                                unsigned *Tolerated = nullptr);

class CacheClient {
public:
  /// Connects, asks the daemon which region file it writes, and attaches to
  /// that file. A non-empty \p RegionPath is attached instead, for a test
  /// that wants to name it. Either way the file must be the daemon's own
  /// instance (HelloReply), or nothing the daemon says about offsets can be
  /// trusted and connecting fails. Fails too if no daemon is running, which
  /// is an ordinary condition: the compiler then behaves exactly as it would
  /// without a cache. A non-empty \p Compile names the compile, the same in
  /// every build: the daemon then says which headers it read without asking
  /// last time (notListedBefore), and keeps what this compile reports.
  /// \p Build is the build the compile belongs to (setBuild), 0 for none.
  static llvm::Expected<std::unique_ptr<CacheClient>>
  connect(llvm::StringRef SocketPath = DefaultSocketPath,
          llvm::StringRef RegionPath = {}, llvm::StringRef Compile = {},
          uint64_t Build = 0);

  /// Tells the daemon at \p SocketPath that a link of \p Output succeeded
  /// (BuildHint), for its offline step. False if no daemon took it, which
  /// is ordinary: a build with no daemon running.
  static bool sendBuildHint(llvm::StringRef SocketPath,
                            llvm::StringRef Output);

  ~CacheClient();

  /// The decisions file the daemon named when this client connected
  /// (HelloReply::DecisionsPath), or empty.
  llvm::StringRef decisionsPath() const { return DecisionsPath; }
  /// Where the daemon asked this compile to record (HelloReply::RecordDir),
  /// or empty.
  llvm::StringRef recordDir() const { return RecordDir; }
  /// Whether the daemon picks headers to merge, so that the timing report
  /// lists the compile's requests (HelloReply::WantAsks).
  bool wantAsks() const { return WantAsks; }

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

  /// The stored copy of \p Filename whose module is \p ModuleName and whose
  /// metadata hashes to \p Hash, and its metadata in \p Meta, with no
  /// check: a copy that links it (CLANG_HEADER_CACHE_LINK) was checked for
  /// both. Empty if the daemon holds none.
  llvm::StringRef fetch(llvm::StringRef Filename, llvm::StringRef ModuleName,
                        uint64_t Hash, std::optional<MetadataView> &Meta);

  /// CLANG_HEADER_CACHE_SECOND_MISS: whether a compile was about to write
  /// the copy of \p Filename named \p ModuleName before; the daemon notes
  /// this one. True if the daemon cannot say, so that the copy is written.
  bool seenBefore(llvm::StringRef Filename, llvm::StringRef ModuleName);

  /// Stores a newly produced entry. Blocks until the daemon acknowledges,
  /// unless CLANG_HEADER_CACHE_SEND_THREAD: then the encoded copy goes to a
  /// thread of the compile's own, which sends it on a connection of its
  /// own, and the compile goes on.
  bool push(const PushRequest &Req);

  /// CLANG_HEADER_CACHE_WAIT: tells the daemon, from the sending thread and
  /// without waiting, that this compile missed \p Filename and is writing
  /// its copy; with \p Ended, that it stopped without one.
  void noteWriting(llvm::StringRef Filename, bool Ended);

  /// CLANG_HEADER_CACHE_CLAIM: asks to be the compile that writes the copy
  /// of \p Filename named \p ModuleName; false if another compile claimed
  /// or stored it. True if the daemon cannot say, so that the copy is
  /// written. With \p Release, gives the claim up.
  bool claim(llvm::StringRef Filename, llvm::StringRef ModuleName,
             bool Release = false);

  /// Waits until every copy given to the sending thread is sent, and ends
  /// the thread. A compile calls it before it exits.
  void finishSending();

  /// CLANG_HEADER_CACHE_CLAIM=check: compares \p R, a copy whose claim was
  /// refused, with the stored copy of the same name, and counts whether the
  /// two are identical.
  void compareStored(const PushRequest &R);

  /// Which of the concurrency switches this compile runs with.
  bool sendThread() const { return SendThread; }
  bool waitMode() const { return WaitMode; }
  /// "1" claims; "check" claims, and where refused writes the copy anyway,
  /// unsent, to compare with the stored one of the same name.
  llvm::StringRef claimMode() const { return ClaimMode; }
  /// Whether the latest lookup's reply was held for a copy being written.
  bool lastWaited() const { return LastWaited; }

  /// Whether \p Path is on the daemon's ask-list, the headers some compile
  /// found slow enough to be worth a saved copy (DECISIONS.md 2.14). Read
  /// from the shared region, with no socket traffic; the parsed list is kept
  /// until the daemon publishes a new one.
  bool isListed(llvm::StringRef Path);
  /// Whether this compile (connect's \p Compile) read \p Path in its last
  /// build without asking for a copy, because the ask-list did not have it
  /// then. Its copies are of the contexts it asked for, so it asks for the
  /// same headers again (DECISIONS.md 2.28).
  bool notListedBefore(llvm::StringRef Path) const {
    return NotListedBefore.contains(Path);
  }

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
    /// Lookups and fetches answered from the region's index, with no
    /// request to the daemon.
    unsigned IndexReads = 0;
    unsigned CandidatesRejected = 0;
    /// Hits on a copy whose __COUNTER__ began elsewhere, served by
    /// CLANG_HEADER_CACHE_COUNTER=names or =any.
    unsigned OtherStartHits = 0;
    /// CLANG_HEADER_CACHE_GUARD_PLUS=1: hits on a copy holding headers the
    /// compile read already, and how many such headers they held.
    unsigned GuardPlusHits = 0, HeldAgain = 0;
    unsigned RejectedFor[14] = {};
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
    /// CLANG_HEADER_CACHE_WAIT: lookups the daemon held, and how many of
    /// them ended in a hit.
    unsigned Waited = 0, WaitedHits = 0;
    /// CLANG_HEADER_CACHE_CLAIM: claims refused; with "check", of the copies
    /// refused, those identical to the stored copy of the same name, those
    /// that differ, and those whose namesake was not stored yet.
    unsigned ClaimsRefused = 0, ClaimSame = 0, ClaimDiffers = 0,
             ClaimNotStored = 0;
  };
  const Stats &stats() const { return Counters; }

  /// What the latest lookup found: how many saved PCMs the daemon returned,
  /// and why the first rejected one was rejected (None if none was).
  unsigned lastCandidates() const { return LastCandidates; }
  MismatchReason lastRejection() const { return LastRejection; }
  /// CLANG_HEADER_CACHE_COUNTER_PROBE: of the latest lookup's copies whose
  /// header expanded __COUNTER__ and which fit otherwise, the one needing
  /// least: 2 begun at the same value, 1 differing but renamable (its values
  /// landed only in names and strings), 0 neither; -1 for none.
  int lastCounterFit() const { return LastCounterFit; }

  /// The names this compile made from __COUNTER__ values: its own text's,
  /// which the exporter adds, and those each copy it loads lists.
  llvm::DenseSet<const IdentifierInfo *> &counterNames() {
    return CounterNames;
  }
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
  /// The latest lookup's match's record in the region's index, or null if
  /// the daemon's reply gave it.
  const IndexedCopy *lastRecord() const { return Region.copyAt(LastRecord); }
  uint64_t lastRecordOffset() const { return LastRecord; }
  /// CLANG_HEADER_CACHE_GUARD_PLUS=1: how many headers the latest lookup's
  /// match holds that the compile read already.
  unsigned lastHeldAgain() const { return LastHeldAgain; }

  /// CLANG_HEADER_CACHE_MERGE: the hit being loaded loads the combination
  /// \p Combo of copies (MergeRecord::Combo); the timing report says so.
  void noteCombo(uint64_t Combo) { LastCombo = Combo; }
  /// CLANG_HEADER_CACHE_MERGE=auto: the latest hit, if it loaded links,
  /// took \p Instructions from its lookup to the end of its replay.
  void noteHitInstructions(uint64_t Instructions) {
    if (LastHitCombo && !Combos.empty())
      Combos.back().Instructions = Instructions;
    LastHitCombo = false;
  }
  /// The record of the combination \p Combo of \p Copy; null for none.
  const MergeRecord *comboOf(const IndexedCopy &Copy, uint64_t Combo) const {
    return Region.comboOf(Copy, Combo);
  }

  /// CLANG_HEADER_CACHE_MERGE: in a process forked from the compile, a
  /// connection of its own, every message sent on it and waited for. The
  /// compile's connection and sending thread are left alone; this process
  /// must end with _exit. False if the daemon cannot be reached.
  bool reconnectAfterFork();

  /// CLANG_HEADER_CACHE_COUNTER=unique or =ahead: a hit moves the counter
  /// past the values its copy holds.
  bool counterAhead() const {
    return CounterOtherStart == OtherStart::Unique ||
           CounterOtherStart == OtherStart::Ahead;
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
  std::string SocketPath, DecisionsPath, RecordDir;
  bool WantAsks = false;
  /// Tells this compile's writing apart from other compiles' at the daemon.
  uint64_t ClientId = 0;
  bool SendThread = false, WaitMode = false, LastWaited = false;
  std::string ClaimMode;
  /// CLANG_HEADER_CACHE_COUNTER=1: a copy whose header expanded __COUNTER__
  /// is served where the counter starts at the value it started at. =unique
  /// also serves one whose values only set names apart where the counter
  /// has not yet reached that value, the counter then moving past the
  /// copy's values, so that none repeats (the user's rule, 2026-10-27).
  bool CounterMode = false;
  /// For measuring only, a copy served wherever the counter starts, keeping
  /// the values it was written with: =names, one whose values landed only in
  /// names and strings; =any, every one, as a module is; =ahead, every one
  /// where the counter has not yet reached the value it began at, the
  /// counter then moving past the copy's values, so that none repeats (sound
  /// where each value only has to differ from the others, as the kernel's
  /// and qemu's do).
  enum class OtherStart { Never, Unique, Names, Ahead, Any } CounterOtherStart =
      OtherStart::Never;
  /// The sending thread: messages it sends in order on a connection of its
  /// own, so that they never interleave with the compile's lookups.
  void sendLoop();
  void enqueue(llvm::SmallVector<char, 0> Msg);
  std::thread Sender;
  std::mutex QueueLock;
  std::condition_variable QueueReady;
  std::deque<llvm::SmallVector<char, 0>> Queue;
  bool Finishing = false;
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
  int LastCounterFit = -1;
  std::vector<bool> LastSubmodules;
  std::vector<std::string> LastSubmoduleNames, LastSubmodulePaths;
  std::optional<MetadataView> LastMetadata;
  uint64_t LastRecord = 0, LastCombo = 0;
  unsigned LastHeldAgain = 0;
  /// CLANG_HEADER_CACHE_GUARD_PLUS=1 (matchesLiveState's Tolerated).
  bool GuardPlus = false;
  /// For the timing report: each hit's copy and combination (noteCombo),
  /// and its instructions (noteHitInstructions).
  std::vector<TimingReport::ComboHit> Combos;
  bool LastHitCombo = false;
  std::vector<NamedDecl *> MatchedDecls;
  /// The ask-list as last parsed, and the version it was parsed from.
  llvm::StringSet<> AskList;
  uint64_t AskListVersion = 0;
  /// See connect and notListedBefore.
  std::string Compile;
  llvm::StringSet<> NotListedBefore;
  /// See setBuild. The searches' outcomes this compile found, by where the
  /// copy's metadata is and the search key, and those not yet sent to the
  /// daemon.
  uint64_t Build = 0;
  llvm::DenseMap<std::pair<uint64_t, uint64_t>, uint8_t> Outcomes;
  /// Live declarations' fingerprints, for every check (SearchMemo::Prints).
  llvm::DenseMap<std::pair<const NamedDecl *, unsigned>, SearchMemo::Print>
      Prints;
  /// See SearchMemo::CounterNames.
  llvm::DenseSet<const IdentifierInfo *> CounterNames;
  /// The copies used since the last message, by metadata offset.
  std::vector<uint64_t> Used;

  /// Copies are found in the region's index, with no request to the
  /// daemon (CLANG_HEADER_CACHE_REGION_INDEX=0 asks instead). Not with
  /// CLANG_HEADER_CACHE_WAIT, whose reply waits for a copy being written,
  /// nor with CLANG_HEADER_CACHE_PIECES, whose reply holds the versions of
  /// the copies' sections.
  bool RegionIndex = true;
  /// Per header, the records of its copies the index listed so far, the
  /// oldest first, and the newest of them.
  struct IndexedCopies {
    uint64_t Newest = 0;
    std::vector<const IndexedCopy *> Copies;
  };
  llvm::StringMap<IndexedCopies> Indexed;
  /// The records of \p Filename's copies, with those stored since the
  /// last call added.
  IndexedCopies &indexedCopies(llvm::StringRef Filename);
  /// The copies of \p Filename in the index, as the daemon's reply to a
  /// lookup gives them: the most recently used first.
  std::vector<CacheEntryRef> indexedEntries(llvm::StringRef Filename);

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
