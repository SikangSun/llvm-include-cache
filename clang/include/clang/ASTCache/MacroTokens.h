//===- MacroTokens.h -------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One definition of what a macro's replacement list looks like as a string.
//
// It lives on its own because two sides depend on producing byte-identical
// output from it: the tracker writes it when recording an ExpandsTo
// syntactic dependency, and the client writes it again for the live macro
// when checking whether a stored entry still applies. If those two ever
// drifted, every ExpandsTo dependency would compare unequal and nothing would
// ever hit -- a silent failure that looks like the cache simply not helping.
// What __FILE__ expands to is here for the same reason.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_MACROTOKENS_H
#define LLVM_CLANG_ASTCACHE_MACROTOKENS_H

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace clang {
class LangOptions;
class MacroInfo;
class SourceManager;
class TargetInfo;

namespace ast_cache {

/// A stable spelling of \p MI's replacement list: each token's kind, then its
/// identifier or literal text, separated by NULs. NUL-separated rather than
/// space-joined so two different token sequences cannot collide into one
/// string -- which is why every consumer must treat this as opaque bytes and
/// never as a C string.
///
/// A literal's text is read from where the token was written, not from the
/// token: a macro loaded from a saved copy carries no literal text, so
/// `#define __WORDSIZE 64` would otherwise read as "some number" and match
/// `#define __WORDSIZE 32`. std::nullopt when that text cannot be read; such
/// a definition must neither be recorded nor be found to match.
std::optional<std::string> serializeMacroTokens(const MacroInfo &MI,
                                                const SourceManager &SM,
                                                const LangOptions &LangOpts);

/// What __FILE__ expands to, without its quotes, in a file the compile names
/// \p Name; with \p LastComponentOnly, what __FILE_NAME__ expands to. The same
/// steps as Preprocessor::ExpandBuiltinMacro, here for the same reason as
/// serializeMacroTokens: the tracker records this text and the client
/// computes it again for the live compile, and the two must not drift.
std::string fileMacroText(llvm::StringRef Name, bool LastComponentOnly,
                          const LangOptions &LangOpts, const TargetInfo &TI);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_MACROTOKENS_H
