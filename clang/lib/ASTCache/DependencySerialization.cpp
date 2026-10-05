//===- DependencySerialization.cpp ------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/ByteStream.h"
#include "llvm/ADT/STLExtras.h"

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

  W.u32(static_cast<uint32_t>(Deps.Syntactic.size()));
  for (const SyntacticDependency &D : Deps.Syntactic) {
    W.str(D.Name);
    W.u8(static_cast<uint8_t>(D.Required));
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

  W.u32(static_cast<uint32_t>(Deps.MacroEffects.size()));
  for (const MacroEffect &E : Deps.MacroEffects) {
    W.str(E.Name);
    W.u8(E.Defined ? 1 : 0);
  }

  W.u32(static_cast<uint32_t>(Deps.Includes.size()));
  for (const IncludeDependency &D : Deps.Includes) {
    W.u8(static_cast<uint8_t>(D.K));
    W.u8(D.Angled ? 1 : 0);
    W.str(D.Spelled);
    W.u32(static_cast<uint32_t>(D.Includer));
    W.str(D.Found);
  }

  W.u32(static_cast<uint32_t>(Deps.FileNames.size()));
  for (const FileNameDependency &D : Deps.FileNames) {
    W.u8(static_cast<uint8_t>(D.Which));
    W.u32(static_cast<uint32_t>(D.File));
    W.str(D.Text);
  }
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
    SyntacticDependency D;
    D.Name = R.str().str();
    uint8_t Raw = R.u8();
    if (Raw > static_cast<uint8_t>(SyntacticDependency::State::ExpandsTo))
      return std::nullopt;
    D.Required = static_cast<SyntacticDependency::State>(Raw);
    D.TokenSequence = R.str().str();
    Deps.Syntactic.push_back(std::move(D));
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

  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    MacroEffect E;
    E.Name = R.str().str();
    uint8_t Raw = R.u8();
    if (Raw > 1)
      return std::nullopt;
    E.Defined = Raw != 0;
    Deps.MacroEffects.push_back(std::move(E));
  }

  // An entry names its includer, and a __FILE__ its file, by the index of
  // an entry before it; anything else would send a hit's lookups to the
  // wrong place.
  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    IncludeDependency D;
    uint8_t Raw = R.u8();
    if (Raw > static_cast<uint8_t>(IncludeDependency::Kind::HasIncludeNext))
      return std::nullopt;
    D.K = static_cast<IncludeDependency::Kind>(Raw);
    Raw = R.u8();
    if (Raw > 1)
      return std::nullopt;
    D.Angled = Raw != 0;
    D.Spelled = R.str().str();
    D.Includer = static_cast<int32_t>(R.u32());
    if (D.Includer < -1 || D.Includer >= static_cast<int32_t>(I))
      return std::nullopt;
    D.Found = R.str().str();
    Deps.Includes.push_back(std::move(D));
  }

  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    FileNameDependency D;
    uint8_t Raw = R.u8();
    if (Raw > static_cast<uint8_t>(FileNameDependency::Macro::FileName))
      return std::nullopt;
    D.Which = static_cast<FileNameDependency::Macro>(Raw);
    D.File = static_cast<int32_t>(R.u32());
    if (D.File < -1 || D.File >= static_cast<int32_t>(Deps.Includes.size()))
      return std::nullopt;
    D.Text = R.str().str();
    Deps.FileNames.push_back(std::move(D));
  }

  // Trailing bytes mean the writer and reader disagree about the shape even
  // though the version matched, so the decode is not trustworthy.
  if (!R.atEnd())
    return std::nullopt;

  return Deps;
}

std::string clang::ast_cache::extractNotAMacroNames(HeaderDependencySet &Deps) {
  std::string List;
  llvm::erase_if(Deps.Syntactic, [&](const SyntacticDependency &D) {
    if (D.Required != SyntacticDependency::State::NotAMacro)
      return false;
    List += D.Name;
    List += '\0';
    return true;
  });
  return List;
}

std::optional<llvm::SmallVector<llvm::StringRef, 0>>
clang::ast_cache::splitNameList(llvm::StringRef List) {
  llvm::SmallVector<llvm::StringRef, 0> Names;
  if (List.empty())
    return Names;
  if (List.back() != '\0')
    return std::nullopt;
  List = List.drop_back();
  List.split(Names, '\0');
  for (llvm::StringRef N : Names)
    if (N.empty())
      return std::nullopt;
  return Names;
}

std::string clang::ast_cache::extractMacroEffects(HeaderDependencySet &Deps) {
  std::string List;
  for (const MacroEffect &E : Deps.MacroEffects) {
    List += E.Defined ? '+' : '-';
    List += E.Name;
    List += '\0';
  }
  Deps.MacroEffects.clear();
  return List;
}

std::optional<std::vector<MacroEffect>>
clang::ast_cache::decodeMacroEffects(llvm::StringRef List) {
  std::optional<llvm::SmallVector<llvm::StringRef, 0>> Entries =
      splitNameList(List);
  if (!Entries)
    return std::nullopt;
  std::vector<MacroEffect> Effects;
  Effects.reserve(Entries->size());
  for (llvm::StringRef Entry : *Entries) {
    if (Entry.size() < 2 || (Entry[0] != '+' && Entry[0] != '-'))
      return std::nullopt;
    Effects.push_back({Entry.drop_front().str(), Entry[0] == '+'});
  }
  return Effects;
}
