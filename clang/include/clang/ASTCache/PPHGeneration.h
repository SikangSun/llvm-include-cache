//===- PPHGeneration.h - Rules for generating a pre-parsed header -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A compile that generates one header's pre-parsed image (CLANG_PPH_GEN,
// PPH_PLAN.md section 4) refuses, as GCC's -fpph-gen did, a header that has
// no include guard or runs #include_next, and says why.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_PPHGENERATION_H
#define LLVM_CLANG_ASTCACHE_PPHGENERATION_H

#include "llvm/ADT/StringRef.h"

namespace clang {

class CompilerInstance;

namespace ast_cache {

class CacheClient;

/// Watches \p CI's preprocessor for \p Target, the absolute name of the header
/// being generated, and has \p Client refuse its image when it breaks a rule.
/// Also gives \p Client the header's end state (CacheClient::endState).
void installPPHGenerationRules(CompilerInstance &CI, CacheClient &Client,
                               llvm::StringRef Target);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_PPHGENERATION_H
