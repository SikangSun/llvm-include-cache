//===- HeaderCacheServer.h - Installing the hit path ------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Installs the compiler's side of the cache: the object that answers the
// preprocessor's question at an #include (clang/Lex/HeaderCacheHook.h) by
// asking the daemon, checking what comes back against this compile, and
// loading the winner.
//
// Installed from cc1 rather than from CompilerInstance, because this library
// already depends on clangFrontend and the dependency cannot run both ways.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_HEADERCACHESERVER_H
#define LLVM_CLANG_ASTCACHE_HEADERCACHESERVER_H

#include "llvm/ADT/StringRef.h"
#include <memory>

namespace clang {
class CompilerInstance;

namespace ast_cache {
class CacheClient;

/// Installs the hit path when CLANG_HEADER_CACHE names a daemon socket, and
/// does nothing otherwise -- including when no daemon answers, which is an
/// ordinary condition rather than an error. With CLANG_HEADER_CACHE_STATS
/// set, lookup and hit counts are printed when the compile ends. With
/// CLANG_HEADER_CACHE_JOBS naming a directory, the compile also writes there,
/// for each header it reads at a declaration boundary, the job that builds
/// that header's saved copy (ASTCacheProducer.h), less the driver flags.
///
/// Signature matches CompilerInstance::PreprocessorCreatedCallback.
void installHeaderCacheHook(CompilerInstance &CI);

/// Whether a compile exports the headers it reads (PartExporter).
enum class ExportMode {
  None,
  /// Serve what fits, and export each listed header no stored copy fits.
  /// With CLANG_HEADER_CACHE_OUTSIDE=copy, only until the compile loads its
  /// first copy: a deep copy cannot be of a loaded declaration.
  Misses,
  /// Serve nothing; export every listed header no stored copy fits.
  PopulateOnly,
};

/// The same, with the client supplied rather than built from the
/// environment. For tests, which run their own daemon. A non-empty \p JobsDir
/// records jobs there. cc1 exports misses unless
/// CLANG_HEADER_CACHE_EXPORT_PARTS is "0", and only populates if it is
/// "populate".
void installHeaderCacheHook(CompilerInstance &CI,
                            std::unique_ptr<CacheClient> Client,
                            bool ReportStats = false,
                            llvm::StringRef JobsDir = {},
                            ExportMode Export = ExportMode::None);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_HEADERCACHESERVER_H
