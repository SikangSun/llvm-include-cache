//===- CacheDaemon.h -------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The caching daemon: a lookup table from header filename to the entries
// stored for it, over an append-only shared region (SharedRegion.h).
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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace ast_cache {

class CacheDaemon {
public:
  static llvm::Expected<std::unique_ptr<CacheDaemon>>
  create(llvm::StringRef RegionPath = DefaultRegionPath,
         uint64_t RegionSize = DefaultRegionSize);

  /// Handles one message body and returns the reply, or std::nullopt for a
  /// message that could not be understood. An unreadable message closes the
  /// connection rather than being guessed at.
  std::optional<llvm::SmallVector<char, 0>>
  handleMessage(llvm::StringRef Body);

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
    uint64_t Entries = 0;
    uint64_t Headers = 0;
  };
  Stats stats() const;

private:
  CacheDaemon() = default;

  struct StoredEntry {
    HeaderDependencySet Deps;
    /// The encoded record, kept so a push can be tested for duplication by
    /// byte comparison. Two runs that produce the same dependencies in a
    /// different order would fail this test and store a second, redundant
    /// entry -- which costs space, never correctness, since matching is
    /// semantic and happens on the client.
    std::string EncodedDeps;
    uint64_t Offset = 0;
    uint64_t Length = 0;
  };

  llvm::SmallVector<char, 0> handleLookup(const LookupRequest &Req);
  llvm::SmallVector<char, 0> handlePush(const PushRequest &Req);

  SharedRegion Region;

  /// Guards Table and Counters. A lookup holds it just long enough to copy
  /// out one filename's entries; a push holds it to check for a duplicate and
  /// append. One lock for the whole table is a deliberate simplification --
  /// if it contends under a highly parallel build the table can be sharded by
  /// filename later, but that is not needed to get a prototype running.
  mutable std::mutex Mutex;
  llvm::StringMap<std::vector<StoredEntry>> Table;
  Stats Counters;

  std::atomic<bool> Stopping{false};
  /// Owned by run(); shutdown() on it makes a blocked accept return.
  void *Listener = nullptr;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHEDAEMON_H
