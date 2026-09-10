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
// output from it: the tracker writes it when recording a value dependency,
// and the client writes it again for the live macro when checking whether a
// stored entry still applies. If those two ever drifted, every value
// dependency would compare unequal and nothing would ever hit -- a silent
// failure that looks like the cache simply not helping.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_MACROTOKENS_H
#define LLVM_CLANG_ASTCACHE_MACROTOKENS_H

#include <string>

namespace clang {
class MacroInfo;

namespace ast_cache {

/// A stable spelling of \p MI's replacement list: each token's kind, then its
/// identifier or literal text, separated by NULs. NUL-separated rather than
/// space-joined so two different token sequences cannot collide into one
/// string -- which is why every consumer must treat this as opaque bytes and
/// never as a C string.
std::string serializeMacroTokens(const MacroInfo &MI);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_MACROTOKENS_H
