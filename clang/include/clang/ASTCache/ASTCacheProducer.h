//===- ASTCacheProducer.h --------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Builds one header's saved copy: the parsed header, plus the dependency
// record describing what that parse relied on.
//
// The header is compiled in a separate compile rather than sliced out of the
// build that needed it, because include guards make a slice depend on include
// order. In a translation unit that reads bar.h before foo.h, foo.h's own
// #include of bar.h is a no-op, so bar.h's declarations sit outside foo.h's
// slice while foo.h's declarations reference them; reverse the order and
// bar.h lands inside. A separate compile of foo.h fires that #include the same
// way every time.
//
// The separate compile is given the context the real build had: the files the
// real build had already included when it reached the header, in order, and
// the real build's own flags. Every header in a working build compiles in that
// context, so every one is cacheable. Nothing about the context is chosen
// here -- a request carries exactly what the compile that missed had.
//
// The saved copy is a module with two submodules, built from a generated
// module map:
//
//   module __hc_<hash> {
//     explicit module context { header "<wrapper including each context file>" }
//     module target { header "<wrapper including the header>" export * }
//   }
//
// The context has to be compiled into the saved copy, because the header's
// parse needs it. But a loading compile makes only the "target" submodule
// visible. A textual #include of foo.h adds foo.h's declarations to what is
// visible and nothing else; if the context's declarations became visible too,
// loading foo.h's saved copy into a file that never included <stdio.h> would
// declare everything in <stdio.h> there. foo.h's own references into its
// context are covered by foo.h's dependency record, which the loading compile
// checks against its own declarations and macros.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_ASTCACHEPRODUCER_H
#define LLVM_CLANG_ASTCACHE_ASTCACHEPRODUCER_H

#include "clang/ASTCache/CacheProtocol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <string>
#include <vector>

namespace clang {
namespace ast_cache {

/// The submodule of a saved copy that a loading compile makes visible. The
/// saved copy's other submodule, "context", is never made visible.
constexpr const char *TargetSubmoduleName = "target";

/// One #include as the compile that missed performed it. Example: anet.c
/// writes `#include <stdint.h>` and the compile opens
/// /usr/lib/clang/22/include/stdint.h; Spelled is `<stdint.h>` and Path is
/// that file.
struct IncludeRef {
  /// The text the replay writes after `#include`, brackets or quotes
  /// included: `<stdint.h>`, `"hiredis.h"`. Empty means the quoted Path.
  ///
  /// A path alone is not enough, because of #include_next. Clang's
  /// stdint.h says `#include_next <stdint.h>`: search the include path
  /// again, starting after the directory this stdint.h was found in. That is
  /// how Clang's stdint.h reaches glibc's. A file opened by absolute path was
  /// found in no directory, so its #include_next searches from the start,
  /// finds Clang's stdint.h again (already guarded, so empty), and glibc's
  /// stdint.h -- where uint32_t is -- is never read.
  ///
  /// So: if the compile found the file by searching the include path,
  /// Spelled is the text as written, and the replay searches the same path
  /// (Args carries the same -I flags). If the compile found the file without
  /// searching -- next to the .c file, or by an absolute path -- Spelled is
  /// empty: the quoted absolute path is also found without searching, so
  /// #include_next inside the file starts from the top in both compiles.
  std::string Spelled;

  /// The file the compile opened. The producer declines if the replay opens
  /// a different one. Required for the header. For a context entry, empty
  /// means not checked: the populate tool's `-context '<stdint.h>'` form,
  /// where nothing resolved the spelling.
  std::string Path;

  friend bool operator==(const IncludeRef &A, const IncludeRef &B) {
    return A.Spelled == B.Spelled && A.Path == B.Path;
  }
};

/// Everything needed to build one header's saved copy in a separate process.
/// A compile that misses on a header records exactly this, so the same struct
/// is the population job format (see writeJobFile).
struct ProduceRequest {
  /// The header to save.
  IncludeRef Header;

  /// The missing compile's flags that shape parsing: -I, -D, -U, -std, and
  /// the like. Copied so the saved copy is built under the same configuration
  /// the loading compile will check it against.
  std::vector<std::string> Args;

  /// Includes to replay, in order, in front of the header: the files the
  /// missing compile had already included directly from its .c file when it
  /// reached the header.
  ///
  /// Only whole files belong here, which is why only headers included
  /// directly by a .c file are populated. A header reached from inside
  /// another header -- b.h, included partway through a.h -- is saved as part
  /// of a.h's copy instead. Reproducing "partway through a.h" would mean
  /// including all of a.h, which compiles b.h inside the hidden context
  /// submodule and leaves the visible target submodule empty. produceHeader
  /// detects that and declines.
  std::vector<IncludeRef> Context;
};

struct ProduceResult {
  enum class Status {
    /// Push carries a complete entry.
    Produced,
    /// The header cannot be saved in this context. Expected and routine;
    /// Reason says why.
    Declined,
    /// Something went wrong that should not have.
    Failed,
  };

  Status Stat = Status::Failed;
  /// Human-readable, for the tool's report. "Not cached" is otherwise
  /// indistinguishable from a bug.
  std::string Reason;
  /// The saved copy's top-level module name. A loading compile makes
  /// "<ModuleName>.<TargetSubmoduleName>" visible.
  std::string ModuleName;
  PushRequest Push;

  bool produced() const { return Stat == Status::Produced; }
};

ProduceResult produceHeader(const ProduceRequest &Req);

/// A header with no context in front of it.
inline ProduceResult produceHeader(llvm::StringRef HeaderPath,
                                   llvm::ArrayRef<std::string> Args) {
  ProduceRequest Req;
  Req.Header.Path = HeaderPath.str();
  Req.Args.assign(Args.begin(), Args.end());
  return produceHeader(Req);
}

/// The module name a saved copy is built under: "__hc_" followed by 16 hex
/// digits of an xxh3 hash of the request's header, flags, and context, taken
/// exactly as given. Two different requests get two different names, which
/// matters because two modules with the same name cannot both be loaded into
/// one compile. The same request always gets the same name, in any process.
std::string moduleNameFor(const ProduceRequest &Req);

//===----------------------------------------------------------------------===//
// Job files: how a compile hands its misses to the process that populates
// them. Plain text, one field per line:
//
//   clang-header-cache-jobs 2
//   job
//   header /abs/redis/src/anet.h
//   arg -I/abs/redis/deps/hiredis
//   arg -DMSG=hello world
//   context /abs/redis/src/fmacros.h
//   context <stdint.h> /usr/lib/clang/22/include/stdint.h
//   context "hiredis.h" /abs/redis/deps/hiredis/hiredis.h
//   end
//
// Everything after the first space of a field line is the value, spaces
// included. A header or context value is an IncludeRef: an optional spelling
// in <> or "", a space, then the path. A value containing a newline cannot be
// written and is rejected rather than silently split.
//===----------------------------------------------------------------------===//

llvm::Expected<std::string> encodeJobs(llvm::ArrayRef<ProduceRequest> Jobs);
llvm::Expected<std::vector<ProduceRequest>> decodeJobs(llvm::StringRef Text);

llvm::Error writeJobFile(llvm::StringRef Path,
                         llvm::ArrayRef<ProduceRequest> Jobs);
llvm::Expected<std::vector<ProduceRequest>> readJobFile(llvm::StringRef Path);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_ASTCACHEPRODUCER_H
