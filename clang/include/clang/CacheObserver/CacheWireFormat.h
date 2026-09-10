//===- CacheWireFormat.h ---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-independent, serializable representation of the data the
// TokenStreamCacheObserver captures for a single header. This is the only
// artifact an external caching daemon needs to understand -- it has no
// dependency on a live SourceManager/Preprocessor.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CACHEOBSERVER_CACHEWIREFORMAT_H
#define LLVM_CLANG_CACHEOBSERVER_CACHEWIREFORMAT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MD5.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace cache_observer {

/// A single spelled token, stripped of any dependency on a live
/// SourceManager so it can be serialized and later reconstructed by an
/// out-of-process daemon.
struct WireToken {
  uint16_t Kind = 0; // clang::tok::TokenKind
  uint32_t Offset = 0; // byte offset into the header's contents
  std::string Spelling;
};

enum class WireConditionKind : uint8_t {
  If = 0,
  Ifdef = 1,
  Ifndef = 2,
  Elif = 3,
  Elifdef = 4,
  Elifndef = 5,
  Else = 6,
};

struct WireSkippedRegion {
  uint32_t BeginOffset = 0;
  uint32_t EndOffset = 0;
  WireConditionKind ConditionKind = WireConditionKind::If;
  /// Empty unless the region was gated by a single #ifdef/#ifndef/#elifdef/
  /// #elifndef macro name (or a #else inheriting one from such a guard).
  std::string GuardMacroName;
};

struct HeaderCachePayload {
  static constexpr uint32_t CurrentWireVersion = 1;

  uint32_t WireVersion = CurrentWireVersion;
  std::string HeaderPath;
  /// A cheap, non-cryptographic combination of the header's on-disk size
  /// and modification time. This is a staleness check on the header's own
  /// content, orthogonal to MacroDigestOnEntry (which tracks the macro
  /// environment the header was processed under).
  uint64_t ContentHash = 0;
  llvm::MD5::MD5Result MacroDigestOnEntry = {};
  std::vector<WireToken> SpelledTokens;
  std::vector<WireSkippedRegion> SkippedRegions;
};

/// Serializes \p Payload into a flat, self-contained byte buffer using a
/// stable, versioned binary format.
llvm::SmallVector<char, 0> serialize(const HeaderCachePayload &Payload);

/// Reconstructs a HeaderCachePayload previously produced by serialize().
/// Returns std::nullopt if \p Bytes is truncated, malformed, or was
/// written by an incompatible WireVersion.
std::optional<HeaderCachePayload> deserialize(llvm::ArrayRef<char> Bytes);

} // namespace cache_observer
} // namespace clang

#endif // LLVM_CLANG_CACHEOBSERVER_CACHEWIREFORMAT_H
