//===- ASTCacheProducer.h --------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compiles one header on its own and produces the two things a cache entry
// needs: the parsed result, and the dependency record describing what that
// parse relied on.
//
// The header is compiled in isolation rather than sliced out of an ordinary
// compile, because include guards make a slice depend on include order. In a
// translation unit that reads bar.h before foo.h, foo.h's own #include of
// bar.h is a no-op, so bar.h's declarations sit outside foo.h's slice while
// foo.h's declarations reference them; reverse the order and bar.h lands
// inside. A separate compile fires that #include for real every time and
// produces the same result regardless of who was including it.
//
// The cost is that the header must compile on its own. One that assumes its
// includer already provided something fails here and is declined, which is a
// visible failure at population rather than a silent one at use.
//
// The result is module-shaped, built through a synthesized module map. A
// precompiled header is a translation-unit prefix and cannot be loaded
// partway through a file, which is exactly what an #include site needs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_ASTCACHEPRODUCER_H
#define LLVM_CLANG_ASTCACHE_ASTCACHEPRODUCER_H

#include "clang/ASTCache/CacheProtocol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace clang {
namespace ast_cache {

struct ProduceResult {
  enum class Status {
    /// Push carries a complete entry.
    Produced,
    /// The header is not cacheable. Expected and routine -- Reason says why.
    Declined,
    /// The header should have been cacheable but something went wrong.
    Failed,
  };

  Status Stat = Status::Failed;
  /// Human-readable, for the tool's report. "not cached" is otherwise
  /// indistinguishable from a bug.
  std::string Reason;
  PushRequest Push;

  bool produced() const { return Stat == Status::Produced; }
};

/// Compiles \p HeaderPath by itself. \p ExtraArgs are appended to the
/// invocation, so a caller can pass the -D and -I flags the real build uses.
ProduceResult produceHeader(llvm::StringRef HeaderPath,
                            llvm::ArrayRef<std::string> ExtraArgs);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_ASTCACHEPRODUCER_H
