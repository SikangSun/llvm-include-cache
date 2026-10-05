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
#include "clang/Basic/FileEntry.h"
#include "clang/Lex/HeaderSearch.h"
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
};

/// Whether \p Deps still holds in this compile. \p S must be non-null:
/// every record carries pragma state and only Sema holds the live value, so
/// a null Sema rejects every candidate rather than passing checks it cannot
/// run.
///
/// \p NotAMacroNames is the list extractNotAMacroNames took out of a record
/// the daemon stored (see DependencySerialization.h). A record that never went
/// through the daemon still carries those names in Deps.Syntactic, and both
/// are checked.
///
/// \p Detail, when non-null, receives what failed, e.g. "LIMIT was not
/// defined, is".
///
/// \p Site is where the header is being included. Deps.Includes and
/// Deps.FileNames are checked from there, and metadata with either is
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
  unsigned FilesCompared = 0;
};

/// \p Cost, when non-null, is increased by the time each step takes.
MismatchReason matchesLiveState(const HeaderDependencySet &Deps,
                                Preprocessor &PP, Sema *S,
                                llvm::StringRef NotAMacroNames = {},
                                std::string *Detail = nullptr,
                                const IncludeSite *Site = nullptr,
                                CheckCost *Cost = nullptr);

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
  /// \p MatchedDeps, when non-null, receives the matched entry's record, whose
  /// Entry/Exit snapshots and macro effects the caller needs to replay the
  /// header's effect on the compile after loading.
  ///
  /// \p Use, when given, is offered each candidate that passes the check, in
  /// order, with its module name, and the first it accepts is the result. A
  /// candidate the check cannot rule out can still be unusable -- built with
  /// other compiler options, say -- and a later one may serve.
  ///
  /// \p Site is where the header is being included, as matchesLiveState
  /// takes it.
  llvm::StringRef lookup(llvm::StringRef Filename, Preprocessor &PP, Sema *S,
                         HeaderDependencySet *MatchedDeps = nullptr,
                         llvm::function_ref<bool(llvm::StringRef Bytes,
                                                 llvm::StringRef ModuleName)>
                             Use = nullptr,
                         const IncludeSite *Site = nullptr);

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
    unsigned CandidatesRejected = 0;
    unsigned RejectedFor[10] = {};
    /// With logRejections on: the header and what failed, one per rejected
    /// candidate, such as "/usr/include/stdio.h: __need_size_t was defined,
    /// is not".
    std::vector<std::string> Rejections;
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

  /// Keep a line per rejected candidate naming what failed. Off by default:
  /// it builds a string for every rejection.
  void logRejections(bool On) { LogRejections = On; }
  bool loggingRejections() const { return LogRejections; }

private:
  CacheClient() = default;

  std::unique_ptr<llvm::raw_socket_stream> Conn;
  SharedRegion Region;
  Stats Counters;
  bool LogRejections = false;
  /// The ask-list as last parsed, and the version it was parsed from.
  llvm::StringSet<> AskList;
  uint64_t AskListVersion = 0;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHECLIENT_H
