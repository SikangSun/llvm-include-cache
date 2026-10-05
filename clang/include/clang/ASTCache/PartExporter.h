//===- PartExporter.h ------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Saves a header's own part of the real compile, at a miss (DECISIONS.md
// 2.17): the declarations and macros the header's text produced, the
// outside declarations those refer to, and the metadata a later hit checks.
// Replaces the separate producer compile.
//
// The part refers to an outside declaration by name, and the loading
// compile resolves the name to its own declaration. CLANG_HEADER_CACHE_OUTSIDE
// =copy writes a deep copy of each instead, which the loading compile merges
// with its own. A part with a declaration it cannot name, such as a field of
// a variable's unnamed struct, is written with copies too.
//
// The part is written at the first top-level declaration the parser finishes
// after the header ends, not when the header's file ends: the parser reads
// one token ahead, so the header's last declaration may still be open then.
// The notification comes from an AST consumer registered as a frontend
// plugin, which every compile gets and which does nothing unless this
// compile has an exporter.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_PARTEXPORTER_H
#define LLVM_CLANG_ASTCACHE_PARTEXPORTER_H

#include "clang/ASTCache/CacheProtocol.h"
#include "clang/Basic/FileEntry.h"
#include <functional>
#include <string>
#include <vector>

namespace clang {
class CompilerInstance;

namespace ast_cache {

class PartExporter {
public:
  struct Stats {
    unsigned Exported = 0;
    unsigned Declined = 0;
    /// Exported parts written with deep copies, because some outside
    /// declaration could not be named, and why.
    unsigned Copied = 0;
    std::vector<std::string> Copies;
    /// "<header>: <why>", one per declined part.
    std::vector<std::string> Declines;
    /// "<header>: <bytes> bytes in <us> us, <n> by name", one per exported
    /// part.
    std::vector<std::string> Exports;
  };

  /// Installs an exporter on \p CI's preprocessor, which owns it. \p Deliver
  /// receives each part, ready to push.
  static PartExporter &install(CompilerInstance &CI,
                               std::function<void(PushRequest &&)> Deliver);

  virtual ~PartExporter();

  /// The compile is about to enter \p File at an #include where a saved copy
  /// could have stood in, and none fit: export the header.
  virtual void exportNext(FileEntryRef File) = 0;

  /// A saved copy stood in for an #include. A header open now is not
  /// exported: its part would hold the import, not the declarations. The
  /// copy may be one the compile loaded before, which loads no module file.
  virtual void served() = 0;

  /// Export the header whose absolute path is \p Path the first time it is
  /// entered, whatever the cache says. For CLANG_HEADER_CACHE_EXPORT.
  virtual void exportWhenEntered(std::string Path) = 0;

  virtual const Stats &stats() const = 0;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_PARTEXPORTER_H
