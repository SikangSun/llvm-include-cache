//===- CacheDaemon.h -------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The caching daemon: a lookup table from header filename to the entries
// stored for it, over an append-only shared region (SharedRegion.h). Each
// entry's metadata and saved copy are in the region; the table holds only
// where they are.
//
// The daemon holds no state across restarts -- it starts with an empty table
// and a fresh region, and loads nothing from disk. Its only cleanup duty is
// removing the socket file, so a later run can bind.
//
// Message handling is separated from the socket so it can be tested directly:
// handleMessage takes one message body and returns one reply, with no I/O.
//
// Threading is one thread per connection, blocking on a read with no timeout.
// The absence of a timeout is deliberate. After a client issues its lookups
// for a translation unit it moves on to code generation, which can run for a
// long time without touching the socket; that gap is ordinary rather than a
// hung connection, and a thread parked on a read costs nothing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_CACHEDAEMON_H
#define LLVM_CLANG_ASTCACHE_CACHEDAEMON_H

#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/SharedRegion.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace ast_cache {

/// The slowest header parse a saved copy is not worth loading instead of:
/// what loading one saved copy costs, 226 us on release clang 18
/// (DECISIONS.md 2.14). A header any compile took at least this long to read
/// is on the ask-list.
constexpr uint64_t DefaultAskCutoffMicros = 226;

/// What a loaded PCM costs the rest of a compile: in C, each name the parser
/// meets for the first time is searched for in every loaded PCM's name
/// table (ASTReader::get). Measured at 150-300 us per PCM for 4,200 new
/// names, and about 196 us per PCM across the redis compiles (DECISIONS.md
/// 2.20).
constexpr uint64_t DefaultLaterCostMicros = 200;

struct CacheDaemonOptions {
  uint64_t AskCutoffMicros = DefaultAskCutoffMicros;
  /// Also list every header the moment a copy of it is stored, timed or not.
  /// For the populate tool and tests, where no compile has reported a time
  /// yet; a build leaves it off.
  bool ListStored = false;
  /// Drop a header from the ask-list once its hits are measured to cost
  /// more than parsing it: when its first parse time is below its average
  /// hit cost plus LaterCostMicros (DECISIONS.md 2.20). Off: on redis it
  /// measured no faster than plain clang, where the fixed cutoff alone is
  /// 0.14 s faster.
  bool DropCostly = false;
  uint64_t LaterCostMicros = DefaultLaterCostMicros;
  /// Measurement: when set, the ask-list is exactly these headers from the
  /// start, and no timing changes it, so that runs compare on one list.
  std::optional<std::vector<std::string>> FixedAskList;
  /// The fold rule (-fold), in place of AskCutoffMicros: each header is
  /// judged as its own copy against being folded into the copy around it.
  /// It goes on the ask-list at its first read when its own time there,
  /// plus an ask for each header it includes directly, is at least an ask
  /// and a load (the floor; every header with FoldFloor off). It comes off
  /// for good once its gain so far is below -FoldBudget writes, where a
  /// hit gains its own time plus its children's asks less an ask and a
  /// load, a miss loses an ask and a write, and each load of its copy as a
  /// link of another header's hit loses a load. Costs in microseconds.
  bool Fold = false;
  bool FoldFloor = true;
  /// What the floor reads: the own time plus an ask per header included
  /// (OwnAndChildren), the own time alone (Own), or today's rule, the time
  /// with the headers inside it against AskCutoffMicros (Inclusive).
  enum class FoldListing { OwnAndChildren, Own, Inclusive };
  FoldListing FoldListBy = FoldListing::OwnAndChildren;
  double FoldBudget = 8;
  uint64_t FoldAskMicros = 92, FoldLoadMicros = 50, FoldWriteMicros = 519;
  /// What one load of a header's copy as another header's link costs it;
  /// 0 charges nothing for links.
  uint64_t FoldLinkMicros = 50;
  /// The measured cutoff (-measured-cutoff), in place of AskCutoffMicros,
  /// with no fixed number: a header is on the ask-list while what a hit on
  /// it saves is at least what the hit costs, both its own. The saving is
  /// the parse time of the lines a copy of it holds -- its own, and those
  /// of the headers inside it read without asking -- as the compile that
  /// read it last measured it. The cost is one request for it with its
  /// check, and one copy of it loaded, each the mean of its requests and
  /// loads so far. A cost not measured yet is the default: AskCutoffMicros
  /// for the load and nothing for the request, so a header no compile has
  /// asked for or loaded is asked for while its parse time is at least
  /// AskCutoffMicros (226 us). Every report judges every header again, so a
  /// header can leave the list and come back.
  bool MeasuredCutoff = false;
  /// The offline step between builds (-offline, user 2026-11-02). While
  /// OfflineDecisions does not exist, compiles record what the fold and
  /// merge rules read to OfflineRecord (HelloReply), and a hint that a link
  /// succeeded (BuildHint) runs OfflineCommand with OfflineRecord and
  /// OfflineDecisions as its last two arguments, its output to
  /// OfflineDecisions + ".log". A lookup while it runs stops it, since the
  /// build goes on; the next hint runs it again. The compiles the step runs
  /// itself to merge (CLANG_HEADER_CACHE_OFFLINE=1) neither stop it nor
  /// record, and take the decisions they are given. Once OfflineDecisions
  /// exists, compiles take their decisions from it, record nothing, and
  /// hints run nothing. An empty command runs nothing; OfflineDecisions
  /// alone hands an earlier build's decisions to compiles.
  std::vector<std::string> OfflineCommand;
  std::string OfflineRecord, OfflineDecisions;
  /// The five-counter merge pick (-merge-picks, user 2026-11-05). After each
  /// compile's report, the daemon computes five counters for every header
  /// asked for so far, each relative to the build so far:
  ///   - the share of the build's load time at hits that its hits took;
  ///   - copies loaded per hit, and bytes loaded per hit, each over the
  ///     median header's;
  ///   - the share of compiles that asked for it;
  ///   - the share of those compiles where it was the first header the .c
  ///     file asked for.
  /// The user's rule (2026-11-05): while the distributions are steady, a
  /// hit merges its header's tree when the header passes all five, each
  /// with a conservative threshold. Steady: each counter's top value moved
  /// PickDrift or less since the build had 10% fewer compiles, from the
  /// PickMinCompiles-th compile on (a tenth of the compiles is then at least
  /// one). Pass: the header is in the top PickTopPercent of the headers
  /// asked for so far. The numbers are the ones the first-build records
  /// chose, by the worst project (DECISIONS.md 2.26). After each report the
  /// daemon lists the headers that pass now, as decisions ("merge-root"
  /// lines), in PicksPath. HelloReply names the list to compiles that start
  /// later when there are no offline decisions, and each of their hits on a
  /// header in it merges the tree, as with CLANG_HEADER_CACHE_MERGE=decided.
  /// All of it in the first build only (PickBuild).
  bool MergePicks = false;
  double PickTopPercent = 2, PickDrift = 0.2;
  unsigned PickMinCompiles = 10;
  /// Measurement (user 2026-11-06: "Does redis do better with a single
  /// merge?"): a header merges its tree in one combination of its links per
  /// build. Once a merge of a header is granted, merges of its other
  /// combinations are refused, unless the granted one is given back.
  bool PickOneMerge = false;
  /// Where the list goes; empty: the region's path with ".picks" added.
  std::string PicksPath;
};

/// What the daemon's offline step did; shared with the thread that waits
/// for a run, which can outlive the daemon.
struct OfflineState;

class CacheDaemon {
public:
  using Options = CacheDaemonOptions;

  static llvm::Expected<std::unique_ptr<CacheDaemon>>
  create(llvm::StringRef RegionPath = DefaultRegionPath,
         uint64_t RegionSize = DefaultRegionSize, Options Opts = Options());

  /// The headers on the ask-list now, in the order they were listed.
  std::vector<std::string> askList() const;

  /// Handles one message body and returns the reply, or std::nullopt for a
  /// message that could not be understood. An unreadable message closes the
  /// connection rather than being guessed at.
  std::optional<llvm::SmallVector<char, 0>>
  handleMessage(llvm::StringRef Body) {
    return handleMessage(Body, /*Conn=*/0);
  }
  /// The same, for the connection numbered \p Conn (run() numbers them from
  /// 1); a compile's writing notices and claims end when it closes.
  std::optional<llvm::SmallVector<char, 0>>
  handleMessage(llvm::StringRef Body, uint64_t Conn);
  /// Connection \p Conn closed: what it was writing, and what it claimed,
  /// is given up.
  void connectionClosed(uint64_t Conn);

  /// Accepts connections until \p stop is called, handing each to its own
  /// thread. Removes the socket file on the way out.
  llvm::Error run(llvm::StringRef SocketPath = DefaultSocketPath);

  /// Stops the accept loop. Safe to call from another thread or a signal
  /// handler.
  void stop();

  /// Counters, for the tool's status line. Cheap enough to take the lock for.
  struct Stats {
    uint64_t Lookups = 0;
    uint64_t Hits = 0;      // lookups that returned at least one entry
    uint64_t Pushes = 0;
    uint64_t Duplicates = 0; // pushes discarded as already stored
    uint64_t Dropped = 0;    // pushes the full region had no room for
    uint64_t BytesStored = 0;
    /// BytesStored by kind: the parts' module files and their metadata.
    uint64_t PartBytes = 0, MetadataBytes = 0;
    uint64_t Entries = 0;
    uint64_t Headers = 0;
    uint64_t TimingReports = 0;
    uint64_t HeadersTimed = 0;
    uint64_t HeadersListed = 0;
    /// Headers dropped from the ask-list because their hits cost more than
    /// parsing them.
    uint64_t HeadersDropped = 0;
    /// The fold rule: headers below the floor at their first read, and
    /// headers taken off the ask-list once their losses passed the budget.
    uint64_t HeadersBelowFloor = 0, HeadersFolded = 0;
    /// The measured cutoff: the times a header left the ask-list.
    uint64_t HeadersUnlisted = 0;
    uint64_t HitsTimed = 0;
    /// CLANG_HEADER_CACHE_SECOND_MISS: copies asked about for the first time,
    /// and again.
    uint64_t SeenFirst = 0, SeenAgain = 0;
    /// CLANG_HEADER_CACHE_WAIT: lookups held for a copy being written; of
    /// them, those a new copy ended, those the time limit ended, and lookups
    /// not held because the header's copies seldom serve its requests.
    uint64_t Waits = 0, WaitsServed = 0, WaitsTimedOut = 0, WaitsGated = 0;
    uint64_t WaitMicros = 0;
    /// CLANG_HEADER_CACHE_CLAIM: claims granted, and refused because another
    /// compile claimed or pushed that copy.
    uint64_t ClaimsGranted = 0, ClaimsRefused = 0;
    /// CLANG_HEADER_CACHE_MERGE: merges granted, merged copies stored, and
    /// merges refused because their header merged once (PickOneMerge).
    uint64_t MergesClaimed = 0, MergesStored = 0, MergesRefusedOnce = 0;
    /// The offline step: hints received; runs started, finished (exit 0
    /// with the decisions written), failed, and stopped by a lookup or a
    /// later hint; the last finished run's wall time.
    uint64_t BuildHints = 0, OfflineStarted = 0, OfflineFinished = 0,
             OfflineFailed = 0, OfflineStopped = 0, OfflineMicros = 0;
    /// The five-counter merge pick: headers that were ever on the list, and
    /// the reports it judged with steady counters.
    uint64_t Picks = 0, PickSteadyReports = 0;
  };
  Stats stats() const;

  /// For measuring: one line per header and per copy stored -- the copy's
  /// size, the compile that pushed it, how often and by how many compiles
  /// it was used, its metadata's sizes and definedness tests. Compiles are
  /// counted by their timing reports, one at a compile's end.
  void dumpEntries(llvm::raw_ostream &OS) const;
  /// For measuring the fold rule: one line per header it judged -- its first
  /// read's own time and children, its hits, misses and link loads, its
  /// gain, and whether it is below the floor, folded or listed.
  void dumpFolds(llvm::raw_ostream &OS) const;

  /// An offline run still in flight goes on, and writes its decisions.
  ~CacheDaemon();

private:
  CacheDaemon();

  struct StoredEntry {
    /// The metadata's hash, so a push can be tested for duplication without
    /// comparing bytes against every entry. Two runs that produce the same
    /// metadata in a different order would fail the test and store a
    /// second, redundant entry -- which costs space, never correctness,
    /// since matching is semantic and happens on the client.
    uint64_t Hash = 0;
    /// Where the metadata and the copy's PCM bytes sit in the region.
    uint64_t MetadataOffset = 0;
    uint64_t MetadataLength = 0;
    uint64_t Offset = 0;
    uint64_t Length = 0;
    /// What compiles of build SearchBuild found the copy's searches to find,
    /// one per search key (REPORT.md I9). Another build starts afresh.
    uint64_t SearchBuild = 0;
    std::vector<SearchOutcome> Searches;
    /// Its record in the region's index (SharedRegion::indexCopy).
    uint64_t Record = 0;
  };

  llvm::SmallVector<char, 0> handleLookup(const LookupRequest &Req,
                                          uint64_t Conn);
  llvm::SmallVector<char, 0> handlePush(const ReceivedPush &Req,
                                        uint64_t Conn);
  llvm::SmallVector<char, 0> handleNotice(const WritingNotice &Notice,
                                          uint64_t Conn);
  llvm::SmallVector<char, 0> handleClaim(const ClaimRequest &Req,
                                         uint64_t Conn);
  llvm::SmallVector<char, 0> handleTiming(const TimingReport &Report);
  llvm::SmallVector<char, 0> handleHello(uint64_t Conn,
                                         const HelloRequest &Hello);
  llvm::SmallVector<char, 0> handleBuildHint(const BuildHint &Hint);
  /// Stops the offline run in flight, if any, because of \p Why.
  void stopOffline(llvm::StringRef Why);

  /// Keeps what a compile of \p Build found its searches to find, and which
  /// copies it used. Mutex held.
  void learnLocked(uint64_t Build, const std::vector<LearnedSearch> &Learned,
                   const std::vector<uint64_t> &Used);
  /// When each copy, by metadata offset, was last used, in messages
  /// received: copies are offered most recently used first (REPORT.md I15).
  llvm::DenseMap<uint64_t, uint64_t> LastUsed;
  uint64_t UseClock = 0;
  /// Each copy's record in the region's index, by metadata offset: a use
  /// stamps it there, for the compiles that read the index.
  llvm::DenseMap<uint64_t, uint64_t> RecordOf;
  /// For dumpEntries, per copy by metadata offset.
  struct Usage {
    uint64_t PushedAt = 0;
    unsigned Uses = 0, Compiles = 0;
    uint64_t First = UINT64_MAX, Last = 0, LastCompile = UINT64_MAX;
  };
  llvm::DenseMap<uint64_t, Usage> Usages;
  llvm::StringMap<unsigned> LookupsOf;
  /// Each copy a compile was about to write, by header and module name.
  llvm::StringSet<> SeenOnce;

  /// CLANG_HEADER_CACHE_WAIT: per header, the compiles writing a copy of it
  /// (WritingNotice), by connection and client, and the lookups waiting for
  /// one. An entry stays once made, so a waiting lookup can hold on to it.
  struct Writer {
    uint64_t Conn = 0, ClientId = 0;
    /// When its notice arrived: about when it began parsing the header.
    std::chrono::steady_clock::time_point Since;
  };
  struct Writers {
    std::vector<Writer> List;
    std::condition_variable Arrived;
  };
  llvm::StringMap<std::unique_ptr<Writers>> WritersOf;
  /// Holds \p Req's reply, while another compile writes a copy of its
  /// header, until a new copy is stored or the writers stop: at most until
  /// the earliest writer has had twice the header's parse time, enough to
  /// parse it and write the copy. Not once the waits for this header mostly
  /// ended in copies the waiting compile could not use. Mutex held.
  bool waitForWriterLocked(std::unique_lock<std::mutex> &Lock,
                           const LookupRequest &Req, uint64_t Conn);
  /// Per header, the waits that ended in a new copy and whether the waiting
  /// compile then used it (learned from its next message's Used): the gate.
  struct WaitHistory {
    unsigned Fit = 0, Missed = 0;
  };
  llvm::StringMap<WaitHistory> WaitsOf;
  /// Per connection, the header it last waited for and the copies that
  /// arrived meanwhile, until its next message says whether it used one.
  llvm::DenseMap<uint64_t, std::pair<std::string, std::vector<uint64_t>>>
      WaitedFor;
  /// Settles WaitedFor[\p Conn] with the copies \p Used. Mutex held.
  void settleWaitLocked(uint64_t Conn, const std::vector<uint64_t> &Used);
  /// Ends \p Conn's writing of \p Filename, or all of its writing if
  /// \p Filename is empty, and wakes the lookups waiting on it. Mutex held.
  void endWritingLocked(uint64_t Conn, llvm::StringRef Filename);
  /// CLANG_HEADER_CACHE_CLAIM: each copy claimed and not yet pushed, by
  /// module name, with the connection that claimed it; and the module names
  /// pushed.
  llvm::StringMap<uint64_t> Claimed;
  /// CLANG_HEADER_CACHE_MERGE: by connection, the record of the
  /// combination that compile merges, and the header's path; the copy of
  /// that header it pushes next is the merged one. A compile that merges in
  /// its own thread pushes copies of other headers too.
  llvm::DenseMap<uint64_t, std::pair<uint64_t, std::string>> MergeOf;
  /// Options::PickOneMerge: the headers a merge was granted for.
  llvm::StringSet<> MergedHeaders;
  llvm::StringSet<> PushedNames;

  /// The five-counter merge pick (Options::MergePicks): per header asked
  /// for, its requests, the compiles that made one, the compiles where the
  /// .c file asked for it first, its hits, the links, bytes and load time
  /// they loaded; the reports counted, and the build's links and load time
  /// at hits; each report's five top values, by reports counted; the
  /// headers that pass now, by path, and every header that ever did.
  struct PickCounts {
    uint64_t Asks = 0, Compiles = 0, First = 0, Hits = 0, Links = 0;
    uint64_t Bytes = 0, LoadMicros = 0;
  };
  llvm::StringMap<PickCounts> PickOf;
  uint64_t PickReports = 0, PickLinks = 0, PickLoadMicros = 0;
  std::vector<std::pair<uint64_t, std::array<double, 5>>> PickTops;
  std::vector<std::string> Passing;
  llvm::StringSet<> EverPassed;
  /// The build the pick runs in: the first a compile named when it
  /// connected (HelloRequest::Build). A compile of a later build neither
  /// lists its requests nor takes the list, and its report is not counted.
  uint64_t PickBuild = 0;
  /// Counts \p Report and lists the headers that pass now; true if the list
  /// changed. Mutex held.
  bool pickLocked(const TimingReport &Report);
  /// Writes the list to Options::PicksPath. Mutex held.
  void writePicksLocked();

  /// Adds \p Path to the ask-list; true if it was not on it. Mutex held.
  bool listLocked(llvm::StringRef Path);
  /// Drops \p Path from the ask-list if its hits cost more than parsing it;
  /// true if it was dropped. Mutex held.
  bool dropIfCostlyLocked(llvm::StringRef Path);
  /// Publishes the whole ask-list as a new version. Mutex held.
  void publishLocked();

  SharedRegion Region;
  /// Absolute, so a client in another directory can attach to it.
  std::string RegionPath;
  Options Opts;

  /// Guards Table and Counters. A lookup holds it just long enough to copy
  /// out one filename's entries; a push holds it to check for a duplicate and
  /// append. One lock for the whole table is a deliberate simplification --
  /// if it contends under a highly parallel build the table can be sharded by
  /// filename later, but that is not needed to get a prototype running.
  mutable std::mutex Mutex;
  llvm::StringMap<std::vector<StoredEntry>> Table;
  /// CLANG_HEADER_CACHE_PIECES: per header, the names its copies' section
  /// versions are stored under ("<header>#section:<section>").
  llvm::StringMap<std::vector<std::string>> VersionsOf;
  Stats Counters;
  /// The first time any compile reported for each header. The first is kept:
  /// it lands on the same side of the cutoff as later ones for 93.8% of
  /// parses (DECISIONS.md 2.14), and keeping it makes membership final.
  llvm::StringMap<uint64_t> FirstTiming;
  /// The ask-list, in the order headers joined it.
  std::vector<std::string> Listed;
  llvm::StringSet<> ListedSet;
  /// Each header's hits so far: their count and total cost.
  llvm::StringMap<std::pair<uint64_t, uint64_t>> HitCosts;
  /// Headers dropped from the ask-list. Dropping is final, as listing is.
  llvm::StringSet<> Dropped;
  /// For each compile (HelloRequest::Compile), the headers it read without
  /// asking in its last build because the ask-list did not have them; its
  /// next build gets them in the HelloReply (DECISIONS.md 2.28).
  llvm::StringMap<std::vector<std::string>> NotListedBy;
  /// The fold rule's record of each header: its first read's own time and
  /// children, its gain so far, and the reads that made it.
  struct FoldState {
    uint64_t Own = 0;
    uint32_t Children = 0;
    double Gain = 0;
    uint32_t Hits = 0, Misses = 0, Links = 0;
    bool BelowFloor = false, Folded = false;
  };
  llvm::StringMap<FoldState> Folds;
  /// Applies the fold rule to one compile's report. Mutex held; true if
  /// the ask-list changed.
  bool foldLocked(const TimingReport &Report);
  /// The measured cutoff, per header: its requests and their time, the
  /// copies of it loaded and their share of the load time, and the parse
  /// time of the lines a copy of it holds, from its latest report, in the
  /// order the headers were first reported.
  struct HeaderCosts {
    uint64_t Requests = 0, RequestMicros = 0, Loads = 0, LoadMicros = 0;
  };
  llvm::StringMap<HeaderCosts> CostsOf;
  llvm::StringMap<uint64_t> HeldParse;
  std::vector<std::string> HeldOrder;
  /// Applies the measured cutoff to one compile's report. Mutex held; true
  /// if the ask-list changed.
  bool cutoffLocked(const TimingReport &Report);
  /// Takes \p Path off the ask-list; true if it was on it. Mutex held.
  bool unlistLocked(llvm::StringRef Path);

  std::shared_ptr<OfflineState> Offline;
  /// The connections of compiles the offline step runs: their lookups do
  /// not stop it. Mutex held.
  llvm::DenseSet<uint64_t> OfflineConns;

  std::atomic<bool> Stopping{false};
  /// Owned by run(); shutdown() on it makes a blocked accept return.
  void *Listener = nullptr;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHEDAEMON_H
