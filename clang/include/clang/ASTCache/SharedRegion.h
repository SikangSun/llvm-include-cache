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
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_SHAREDREGION_H
#define LLVM_CLANG_ASTCACHE_SHAREDREGION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace ast_cache {

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
