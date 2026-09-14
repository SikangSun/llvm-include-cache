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

#include <memory>

namespace clang {
class CompilerInstance;

namespace ast_cache {
class CacheClient;

/// Installs the hit path when CLANG_HEADER_CACHE names a daemon socket, and
/// does nothing otherwise -- including when no daemon answers, which is an
/// ordinary condition rather than an error. With CLANG_HEADER_CACHE_STATS
/// set, lookup and hit counts are printed when the compile ends.
///
/// Signature matches CompilerInstance::PreprocessorCreatedCallback.
void installHeaderCacheHook(CompilerInstance &CI);

/// The same, with the client supplied rather than built from the
/// environment. For tests, which run their own daemon.
void installHeaderCacheHook(CompilerInstance &CI,
                            std::unique_ptr<CacheClient> Client,
                            bool ReportStats = false);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_HEADERCACHESERVER_H
