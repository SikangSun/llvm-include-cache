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
#include "clang/ASTCache/SharedRegion.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <optional>
#include <string>

namespace llvm {
class raw_socket_stream;
}

namespace clang {
class Preprocessor;
class Sema;

namespace ast_cache {

/// Why a candidate entry was rejected. Reported rather than swallowed: "the
/// cache never helps" and "the cache is checking the wrong thing" look
/// identical without this.
enum class MismatchReason {
  None,
  Conditional,
  Value,
  Symbol,
  Inclusion,
  PragmaState,
};

llvm::StringRef describe(MismatchReason R);

/// Whether \p Deps still holds in this compile. \p S must be non-null:
/// every record carries pragma state and only Sema holds the live value, so
/// a null Sema rejects every candidate rather than passing checks it cannot
/// run.
MismatchReason matchesLiveState(const HeaderDependencySet &Deps,
                                 Preprocessor &PP, Sema *S);

class CacheClient {
public:
  /// Connects and attaches to the shared region. Fails if no daemon is
  /// running, which is an ordinary condition: the compiler then behaves
  /// exactly as it would without a cache.
  static llvm::Expected<std::unique_ptr<CacheClient>>
  connect(llvm::StringRef SocketPath = DefaultSocketPath,
          llvm::StringRef RegionPath = DefaultRegionPath);

  ~CacheClient();

  /// The stored result for \p Filename that applies to this compile, or an
  /// empty StringRef if there is none. The returned bytes live in the shared
  /// region and stay valid for this process's lifetime -- the region is
  /// append-only, so nothing rewrites them.
  ///
  /// \p MatchedDeps, when non-null, receives the matched entry's record, whose
  /// Entry/Exit snapshots the caller needs to replay the header's pragma
  /// effect after loading.
  llvm::StringRef lookup(llvm::StringRef Filename, Preprocessor &PP, Sema *S,
                         HeaderDependencySet *MatchedDeps = nullptr);

  /// Stores a newly produced entry. Blocks until the daemon acknowledges, so
  /// a caller that does not want to wait should run this off the critical
  /// path.
  bool push(const PushRequest &Req);

  struct Stats {
    unsigned Lookups = 0;
    unsigned Hits = 0;
    unsigned CandidatesRejected = 0;
    unsigned RejectedFor[6] = {};
  };
  const Stats &stats() const { return Counters; }

private:
  CacheClient() = default;

  std::unique_ptr<llvm::raw_socket_stream> Conn;
  SharedRegion Region;
  Stats Counters;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHECLIENT_H
