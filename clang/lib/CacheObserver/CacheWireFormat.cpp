//===- CacheWireFormat.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CacheObserver/CacheWireFormat.h"
#include "llvm/ADT/StringRef.h"
#include <cstring>

using namespace clang;
using namespace clang::cache_observer;

namespace {

void appendU32(llvm::SmallVectorImpl<char> &Out, uint32_t V) {
  char Bytes[4];
  memcpy(Bytes, &V, sizeof(Bytes));
  Out.append(Bytes, Bytes + sizeof(Bytes));
}

void appendU64(llvm::SmallVectorImpl<char> &Out, uint64_t V) {
  char Bytes[8];
  memcpy(Bytes, &V, sizeof(Bytes));
  Out.append(Bytes, Bytes + sizeof(Bytes));
}

void appendU16(llvm::SmallVectorImpl<char> &Out, uint16_t V) {
  char Bytes[2];
  memcpy(Bytes, &V, sizeof(Bytes));
  Out.append(Bytes, Bytes + sizeof(Bytes));
}

void appendString(llvm::SmallVectorImpl<char> &Out, llvm::StringRef S) {
  appendU32(Out, static_cast<uint32_t>(S.size()));
  Out.append(S.begin(), S.end());
}

/// A tiny bounds-checked cursor over a byte buffer. Every read either
/// succeeds or marks the cursor permanently failed; callers check ok()
/// once at the end instead of threading an error through every call.
class Reader {
public:
  explicit Reader(llvm::ArrayRef<char> Bytes) : Bytes(Bytes) {}

  bool ok() const { return !Failed; }

  uint16_t readU16() {
    if (!require(2))
      return 0;
    uint16_t V;
    memcpy(&V, Bytes.data() + Pos, 2);
    Pos += 2;
    return V;
  }

  uint32_t readU32() {
    if (!require(4))
      return 0;
    uint32_t V;
    memcpy(&V, Bytes.data() + Pos, 4);
    Pos += 4;
    return V;
  }

  uint64_t readU64() {
    if (!require(8))
      return 0;
    uint64_t V;
    memcpy(&V, Bytes.data() + Pos, 8);
    Pos += 8;
    return V;
  }

  std::string readString() {
    uint32_t Len = readU32();
    if (!require(Len))
      return {};
    std::string S(Bytes.data() + Pos, Len);
    Pos += Len;
    return S;
  }

  void readBytes(void *Dest, size_t Len) {
    if (!require(Len))
      return;
    memcpy(Dest, Bytes.data() + Pos, Len);
    Pos += Len;
  }

private:
  bool require(size_t N) {
    if (Failed || Pos + N > Bytes.size()) {
      Failed = true;
      return false;
    }
    return true;
  }

  llvm::ArrayRef<char> Bytes;
  size_t Pos = 0;
  bool Failed = false;
};

} // namespace

llvm::SmallVector<char, 0> clang::cache_observer::serialize(
    const HeaderCachePayload &Payload) {
  llvm::SmallVector<char, 0> Out;

  appendU32(Out, Payload.WireVersion);
  appendString(Out, Payload.HeaderPath);
  appendU64(Out, Payload.ContentHash);
  Out.append(reinterpret_cast<const char *>(Payload.MacroDigestOnEntry.data()),
             reinterpret_cast<const char *>(Payload.MacroDigestOnEntry.data()) +
                 Payload.MacroDigestOnEntry.size());

  appendU32(Out, static_cast<uint32_t>(Payload.SpelledTokens.size()));
  for (const WireToken &Tok : Payload.SpelledTokens) {
    appendU16(Out, Tok.Kind);
    appendU32(Out, Tok.Offset);
    appendString(Out, Tok.Spelling);
  }

  appendU32(Out, static_cast<uint32_t>(Payload.SkippedRegions.size()));
  for (const WireSkippedRegion &Region : Payload.SkippedRegions) {
    appendU32(Out, Region.BeginOffset);
    appendU32(Out, Region.EndOffset);
    Out.push_back(static_cast<char>(Region.ConditionKind));
    appendString(Out, Region.GuardMacroName);
  }

  return Out;
}

std::optional<HeaderCachePayload>
clang::cache_observer::deserialize(llvm::ArrayRef<char> Bytes) {
  Reader R(Bytes);
  HeaderCachePayload Payload;

  Payload.WireVersion = R.readU32();
  if (Payload.WireVersion != HeaderCachePayload::CurrentWireVersion)
    return std::nullopt;

  Payload.HeaderPath = R.readString();
  Payload.ContentHash = R.readU64();
  R.readBytes(Payload.MacroDigestOnEntry.data(),
              Payload.MacroDigestOnEntry.size());

  uint32_t NumTokens = R.readU32();
  Payload.SpelledTokens.reserve(NumTokens);
  for (uint32_t I = 0; I < NumTokens && R.ok(); ++I) {
    WireToken Tok;
    Tok.Kind = R.readU16();
    Tok.Offset = R.readU32();
    Tok.Spelling = R.readString();
    Payload.SpelledTokens.push_back(std::move(Tok));
  }

  uint32_t NumRegions = R.readU32();
  Payload.SkippedRegions.reserve(NumRegions);
  for (uint32_t I = 0; I < NumRegions && R.ok(); ++I) {
    WireSkippedRegion Region;
    Region.BeginOffset = R.readU32();
    Region.EndOffset = R.readU32();
    uint16_t Kind = 0;
    R.readBytes(&Kind, 1);
    Region.ConditionKind = static_cast<WireConditionKind>(Kind & 0xFF);
    Region.GuardMacroName = R.readString();
    Payload.SkippedRegions.push_back(std::move(Region));
  }

  if (!R.ok())
    return std::nullopt;

  return Payload;
}
