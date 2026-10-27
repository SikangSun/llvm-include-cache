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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
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
};

class CacheDaemon {
public:
  using Options = CacheDaemonOptions;

  static llvm::Expected<std::unique_ptr<CacheDaemon>>
  create(llvm::StringRef RegionPath = DefaultRegionPath,
         uint64_t RegionSize = DefaultRegionSize, Options Opts = Options());

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
  };
  Stats stats() const;

  /// For measuring: one line per header and per copy stored -- the copy's
  /// size, the compile that pushed it, how often and by how many compiles
  /// it was used, its metadata's sizes and definedness tests. Compiles are
  /// counted by their timing reports, one at a compile's end.
  void dumpEntries(llvm::raw_ostream &OS) const;

private:
  CacheDaemon() = default;

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
  llvm::SmallVector<char, 0> handleHello();

  /// Keeps what a compile of \p Build found its searches to find, and which
  /// copies it used. Mutex held.
  void learnLocked(uint64_t Build, const std::vector<LearnedSearch> &Learned,
                   const std::vector<uint64_t> &Used);
  /// When each copy, by metadata offset, was last used, in messages
  /// received: copies are offered most recently used first (REPORT.md I15).
  llvm::DenseMap<uint64_t, uint64_t> LastUsed;
  uint64_t UseClock = 0;
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
  llvm::StringSet<> PushedNames;

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

  std::atomic<bool> Stopping{false};
  /// Owned by run(); shutdown() on it makes a blocked accept return.
  void *Listener = nullptr;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHEDAEMON_H
