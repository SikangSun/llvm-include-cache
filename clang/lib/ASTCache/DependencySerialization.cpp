//===- DependencySerialization.cpp ------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/ByteStream.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

void encodeSnapshot(ByteWriter &W, const PragmaSnapshot &S) {
  W.u32(S.AlignPackRaw);
  W.u64(S.FpPragmaRaw);
}

PragmaSnapshot decodeSnapshot(ByteReader &R) {
  PragmaSnapshot S;
  S.AlignPackRaw = R.u32();
  S.FpPragmaRaw = R.u64();
  return S;
}

} // namespace

void clang::ast_cache::encodeDependencySet(const HeaderDependencySet &Deps,
                                           llvm::SmallVectorImpl<char> &Out) {
  ByteWriter W(Out);
  W.u32(DependencyEncodingVersion);

  W.u32(static_cast<uint32_t>(Deps.Conditional.size()));
  for (const ConditionalDependency &D : Deps.Conditional) {
    W.str(D.MacroName);
    W.u8(D.WasDefined ? 1 : 0);
  }

  W.u32(static_cast<uint32_t>(Deps.Value.size()));
  for (const ValueDependency &D : Deps.Value) {
    W.str(D.MacroName);
    W.str(D.TokenSequence);
  }

  W.u32(static_cast<uint32_t>(Deps.Symbol.size()));
  for (const SymbolDependency &D : Deps.Symbol) {
    W.str(D.Name);
    W.u8(static_cast<uint8_t>(D.Classification));
    W.str(D.ResolvedTypeName);
  }

  W.u32(static_cast<uint32_t>(Deps.Inclusion.size()));
  for (const InclusionDependency &D : Deps.Inclusion) {
    W.str(D.HeaderPath);
    W.u8(static_cast<uint8_t>(D.Why));
  }

  encodeSnapshot(W, Deps.Entry);
  encodeSnapshot(W, Deps.Exit);
}

std::optional<HeaderDependencySet>
clang::ast_cache::decodeDependencySet(llvm::StringRef Data) {
  ByteReader R(Data);
  if (R.u32() != DependencyEncodingVersion)
    return std::nullopt;

  HeaderDependencySet Deps;

  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    ConditionalDependency D;
    D.MacroName = R.str().str();
    D.WasDefined = R.u8() != 0;
    Deps.Conditional.push_back(std::move(D));
  }

  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    ValueDependency D;
    D.MacroName = R.str().str();
    D.TokenSequence = R.str().str();
    Deps.Value.push_back(std::move(D));
  }

  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    SymbolDependency D;
    D.Name = R.str().str();
    uint8_t Raw = R.u8();
    if (Raw > static_cast<uint8_t>(SymbolClassification::Template))
      return std::nullopt;
    D.Classification = static_cast<SymbolClassification>(Raw);
    D.ResolvedTypeName = R.str().str();
    Deps.Symbol.push_back(std::move(D));
  }

  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    InclusionDependency D;
    D.HeaderPath = R.str().str();
    uint8_t Raw = R.u8();
    if (Raw > static_cast<uint8_t>(InclusionDependency::Reason::Import))
      return std::nullopt;
    D.Why = static_cast<InclusionDependency::Reason>(Raw);
    Deps.Inclusion.push_back(std::move(D));
  }

  Deps.Entry = decodeSnapshot(R);
  Deps.Exit = decodeSnapshot(R);

  // Trailing bytes mean the writer and reader disagree about the shape even
  // though the version matched, so the decode is not trustworthy.
  if (!R.atEnd())
    return std::nullopt;

  return Deps;
}
