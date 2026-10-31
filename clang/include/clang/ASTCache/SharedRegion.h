//===- SharedRegion.h ------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The append-only region that holds every stored parsed header, shared between
// the daemon and every connected compiler by mmap.
//
// Append-only is what buys the concurrency property. A written entry is never
// modified or moved, so any number of clients can read existing entries with
// no coordination at all, and the daemon can append without disturbing anyone
// mid-read. The socket reply carrying an offset is also the synchronisation
// point: a client only learns of an entry after the daemon finished writing it.
//
// The size is fixed once, generously, at creation. Growing a mapping in place
// can require moving it, which would invalidate every offset already handed
// out and every client's existing mapping. Reserving address space costs
// nothing until written -- the backing file is sparse -- so there is no reason
// to start small.
//
// Only the daemon appends. Clients map the same file for writing, because a
// read-only mapping in LLVM is MAP_PRIVATE and would not reliably show later
// appends, but a client never writes.
//
// The region also holds an index of the stored copies: a table, by the
// hash of a header's name, of the newest copy's record, and each record
// names the copy stored before it. A compile reads the copies of a header
// there, with no request to the daemon.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_SHAREDREGION_H
#define LLVM_CLANG_ASTCACHE_SHAREDREGION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace ast_cache {

/// One stored copy in the region's index of copies: where its metadata and
/// its PCM bytes are. The daemon writes it whole before it publishes it,
/// so a compile finds the copies of a header with no request. Only Stamp,
/// Searches, Serves and Merge change later, and only the daemon changes
/// them.
struct IndexedCopy {
  /// The copy of the same header stored before this one; 0 for none.
  uint64_t Next;
  uint64_t MetadataOffset, MetadataLength, Offset, Length;
  /// The xxh3 of the metadata (CacheEntryRef::MetadataHash).
  uint64_t MetadataHash;
  /// When a compile last used the copy, on the daemon's clock; 0 for never.
  /// A compile checks the most recently used first (REPORT.md I15).
  std::atomic<uint64_t> Stamp;
  /// Where what its searches found in one build is (searchesAt); 0 for
  /// none (REPORT.md I9).
  std::atomic<uint64_t> Searches;
  /// How many hits started from the copy, by the compiles' reports.
  std::atomic<uint64_t> Serves;
  /// CLANG_HEADER_CACHE_MERGE: where the newest MergeRecord of the copy is;
  /// 0 for none.
  std::atomic<uint64_t> Merge;
};

/// CLANG_HEADER_CACHE_MERGE: one combination of copies that hits starting
/// from a copy with links loaded -- the copy, and each link loaded or
/// skipped by its include guard -- with how many hits loaded it, and the
/// one copy it was merged into. Only the daemon writes it.
struct MergeRecord {
  /// The copy's combination recorded before this one; 0 for none.
  uint64_t Next;
  /// The xxh3 of the module names loaded, in load order.
  uint64_t Combo;
  std::atomic<uint64_t> Serves;
  /// 0 while no compile merges it; 1 while one does, or after one failed;
  /// then the record of the merged copy, which a compile checks just before
  /// the copy the hits started from.
  std::atomic<uint64_t> Merged;
  /// The instructions the hits counted in Serves took, from the lookup to
  /// the end of the replay, summed; 0 for a hit that did not count them.
  std::atomic<uint64_t> Instructions;
};

/// What a copy's #include searches found in one build: the build, then
/// each outcome as two words, its search key and the outcome.
struct StoredSearches {
  uint64_t Build = 0;
  llvm::ArrayRef<uint64_t> Pairs;
};

/// Where the daemon listens, and where the region is backed. Fixed paths: for
/// a prototype the compiler and the daemon simply agree on them at build time,
/// with no discovery step, config file, or environment lookup.
constexpr const char *DefaultSocketPath = "/tmp/clang-header-cache.sock";
constexpr const char *DefaultRegionPath = "/dev/shm/clang-header-cache.region";

/// 4 GiB of address space, sparse until written.
constexpr uint64_t DefaultRegionSize = 4ull * 1024 * 1024 * 1024;

class SharedRegion {
public:
  /// An unattached region: empty, appends fail, reads return nothing. Exists
  /// so an owner can be constructed before its region is.
  SharedRegion() = default;

  /// Daemon side. Removes any existing region first, so a restart cannot
  /// append over data a surviving client still has mapped -- with the old
  /// file unlinked, that client's mapping stays valid and simply becomes
  /// orphaned instead of being overwritten.
  static llvm::Expected<SharedRegion> create(llvm::StringRef Path,
                                              uint64_t Size);

  /// Client side. Fails if no daemon has created the region.
  static llvm::Expected<SharedRegion> attach(llvm::StringRef Path);

  SharedRegion(SharedRegion &&) = default;
  SharedRegion &operator=(SharedRegion &&) = default;

  /// Copies \p Data to the end of the region and returns its offset. Returns
  /// std::nullopt when the region is full. Daemon only, and single-threaded
  /// by virtue of the lookup-table lock the caller already holds.
  std::optional<uint64_t> append(llvm::StringRef Data);

  /// The bytes at \p Offset, or an empty StringRef if the range falls outside
  /// what has actually been written. Bounds are checked because the offset
  /// arrived over a socket: an out-of-range one must read as nothing rather
  /// than as whatever happens to be mapped there.
  llvm::StringRef read(uint64_t Offset, uint64_t Length) const;

  /// Identifies the daemon instance that created this region. A client
  /// compares it against the value the daemon reports, so a mapping left over
  /// from a previous instance is detected rather than silently misread.
  uint64_t instanceId() const;

  /// Daemon side. Makes \p List -- names in splitNameList's form -- the list
  /// of headers clients ask about (DECISIONS.md 2.14). The list is appended
  /// like any entry and then published by storing its offset in a fixed slot
  /// of the region's header, so a reader sees either the previous list or the
  /// whole new one. Returns false when the region is full.
  bool publishAskList(llvm::StringRef List);

  /// Client side. The current list, or an empty StringRef if none has been
  /// published. \p Version, when non-null, receives a value that changes
  /// exactly when a new list is published, so a caller can keep a parsed copy
  /// until then.
  llvm::StringRef askList(uint64_t *Version = nullptr) const;

  /// Daemon side. Adds \p Copy, a copy of the header stored under \p Name,
  /// to the index, as the newest copy of the header; its Next, Stamp and
  /// Searches are set here. Returns where its record is, or std::nullopt
  /// when the region or the index is full.
  std::optional<uint64_t> indexCopy(llvm::StringRef Name,
                                    const IndexedCopy &Copy);

  /// Daemon side. A compile used the copy whose record is at \p Record:
  /// at \p Stamp on the daemon's clock.
  void stampCopy(uint64_t Record, uint64_t Stamp);

  /// Daemon side. Makes \p Pairs (key, outcome, key, outcome, ...) what the
  /// copy at \p Record found in build \p Build. False when the region is
  /// full.
  bool publishSearches(uint64_t Record, uint64_t Build,
                       llvm::ArrayRef<uint64_t> Pairs);

  /// Daemon side. A hit started from the copy at \p Record.
  void countServe(uint64_t Record);

  /// Daemon side. A hit started from the copy at \p Record and loaded
  /// the combination \p Combo, taking \p Instructions; its record is made
  /// if it is the first. Returns where the combination's record is, or
  /// std::nullopt if the region is full.
  std::optional<uint64_t> countCombo(uint64_t Record, uint64_t Combo,
                                     uint64_t Serves = 1,
                                     uint64_t Instructions = 0);

  /// Daemon side. Marks the combination \p Combo of the copy at \p Record
  /// as being merged. Returns where its record is, or std::nullopt if a
  /// compile merges it already, or has.
  std::optional<uint64_t> claimMerge(uint64_t Record, uint64_t Combo);

  /// Daemon side. The combination whose record is at \p At is merged into
  /// the copy at \p Merged.
  void setMerged(uint64_t At, uint64_t Merged);

  /// Client side. The record of the combination \p Combo of \p Copy, or
  /// null for none; and the record at \p At.
  const MergeRecord *comboOf(const IndexedCopy &Copy, uint64_t Combo) const;
  const MergeRecord *mergeAt(uint64_t At) const;

  /// Where \p Record, a record this region holds, is.
  uint64_t offsetOf(const void *Record) const;

  /// Client side. The record of the newest copy stored under \p Name, 0
  /// for none; each record names the one stored before it.
  uint64_t newestCopy(llvm::StringRef Name) const;

  /// Client side. The record at \p Record, or null if that is not one
  /// written.
  const IndexedCopy *copyAt(uint64_t Record) const;

  /// Client side. The search outcomes at \p Offset (IndexedCopy::Searches).
  std::optional<StoredSearches> searchesAt(uint64_t Offset) const;

  uint64_t bytesUsed() const;
  uint64_t capacity() const { return Mapping.size(); }

  /// False for a default-constructed region, which is mapped to nothing.
  bool valid() const { return Mapping.size() != 0; }

private:
  llvm::sys::fs::mapped_file_region Mapping;
  std::string Path;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_SHAREDREGION_H
