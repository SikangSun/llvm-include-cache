//===- DependencySerialization.cpp ------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/DependencySerialization.h"
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

/// A byte that must be at most \p Max; a larger one fails the read rather
/// than standing for a value this build does not know.
uint8_t upTo(ByteReader &R, uint8_t Max) {
  uint8_t V = R.u8();
  if (V > Max)
    R.fail();
  return V;
}

void encodeRange(ByteWriter &W, const SavedDiagnostic::Range &R) {
  W.u64(R.Begin);
  W.u64(R.End);
  W.u8(R.TokenRange ? 1 : 0);
}

SavedDiagnostic::Range decodeRange(ByteReader &R) {
  SavedDiagnostic::Range Out;
  Out.Begin = R.u64();
  Out.End = R.u64();
  Out.TokenRange = upTo(R, 1) != 0;
  return Out;
}

} // namespace

void clang::ast_cache::encodeMetadata(const HeaderDependencySet &Deps,
                                      llvm::StringRef ModuleName,
                                      llvm::SmallVectorImpl<char> &Out) {
  size_t Base = Out.size();
  ByteWriter W(Out);
  W.u32(DependencyEncodingVersion);
  size_t Table = Out.size();
  for (unsigned I = 0; I != MetadataView::NumSections; ++I)
    W.u32(0);
  encodeSnapshot(W, Deps.Entry);
  encodeSnapshot(W, Deps.Exit);
  W.u64(Deps.WarningState);
  W.str(ModuleName);
  W.str(Deps.HeaderName);

  // Notes where section S starts and writes its count.
  auto Begin = [&](MetadataView::Section S, size_t Count) {
    llvm::support::endian::write32le(Out.data() + Table + 4 * S,
                                     static_cast<uint32_t>(Out.size() - Base));
    W.u32(static_cast<uint32_t>(Count));
  };
  using SD = SyntacticDependency;
  auto Requires = [&](SD::State S) {
    return llvm::count_if(Deps.Syntactic,
                          [&](const SD &D) { return D.Required == S; });
  };

  Begin(MetadataView::Conditional, Deps.Conditional.size());
  for (const ConditionalDependency &D : Deps.Conditional) {
    W.str(D.MacroName);
    W.u8(D.WasDefined ? 1 : 0);
  }

  Begin(MetadataView::ExpandsTo, Requires(SD::State::ExpandsTo));
  for (const SD &D : Deps.Syntactic)
    if (D.Required == SD::State::ExpandsTo) {
      W.str(D.Name);
      W.str(D.TokenSequence);
    }

  Begin(MetadataView::MacroStacks, Deps.MacroStacks.size());
  for (const MacroStackEffect &E : Deps.MacroStacks) {
    W.str(E.Name);
    W.u32(static_cast<uint32_t>(E.Popped.size()));
    for (const std::optional<std::string> &D : E.Popped) {
      W.u8(D ? 1 : 0);
      if (D)
        W.str(*D);
    }
    W.u32(E.Pushes);
  }

  Begin(MetadataView::Inclusion, Deps.Inclusion.size());
  for (const InclusionDependency &D : Deps.Inclusion) {
    W.str(D.HeaderPath);
    W.u8(static_cast<uint8_t>(D.Why));
  }

  Begin(MetadataView::NotAMacro, Requires(SD::State::NotAMacro));
  for (const SD &D : Deps.Syntactic)
    if (D.Required == SD::State::NotAMacro)
      W.str(D.Name);

  Begin(MetadataView::Symbol, Deps.Symbol.size());
  for (const SymbolDependency &D : Deps.Symbol) {
    W.str(D.Name);
    W.u8(static_cast<uint8_t>(D.Classification));
    W.str(D.ResolvedTypeName);
  }

  Begin(MetadataView::Files, Deps.Files.size());
  for (const FileStamp &F : Deps.Files) {
    W.str(F.Path);
    W.u64(F.Size);
    W.u64(static_cast<uint64_t>(F.ModTime));
  }

  Begin(MetadataView::Includes, Deps.Includes.size());
  for (const IncludeDependency &D : Deps.Includes) {
    W.u8(static_cast<uint8_t>(D.K));
    W.u8(D.Angled ? 1 : 0);
    W.str(D.Spelled);
    W.u32(static_cast<uint32_t>(D.Includer));
    W.str(D.Found);
    W.str(D.Name);
  }

  Begin(MetadataView::FileNames, Deps.FileNames.size());
  for (const FileNameDependency &D : Deps.FileNames) {
    W.u8(static_cast<uint8_t>(D.Which));
    W.u32(static_cast<uint32_t>(D.File));
    W.str(D.Text);
  }

  Begin(MetadataView::Declarations, Deps.Declarations.size());
  for (const DeclarationDependency &D : Deps.Declarations) {
    W.u8(static_cast<uint8_t>(D.In));
    W.str(D.Name);
    W.str(D.Fingerprint);
  }

  Begin(MetadataView::MacroEffects, Deps.MacroEffects.size());
  for (const MacroEffect &E : Deps.MacroEffects) {
    W.str(E.Name);
    W.u8(E.Defined ? 1 : 0);
  }

  Begin(MetadataView::Diagnostics, Deps.Diagnostics.size());
  for (const SavedDiagnostic &D : Deps.Diagnostics) {
    W.u8(D.Level);
    W.u32(D.ID);
    W.u64(D.Loc);
    W.str(D.Message);
    W.u32(static_cast<uint32_t>(D.Ranges.size()));
    for (const SavedDiagnostic::Range &R : D.Ranges)
      encodeRange(W, R);
    W.u32(static_cast<uint32_t>(D.FixIts.size()));
    for (const SavedDiagnostic::FixIt &F : D.FixIts) {
      encodeRange(W, F.Remove);
      W.str(F.Insert);
      W.u8(F.BeforePreviousInsertions ? 1 : 0);
    }
  }

  Begin(MetadataView::WarningPragmas, Deps.WarningPragmas.size());
  for (const WarningPragma &P : Deps.WarningPragmas) {
    W.u8(P.Severity);
    W.str(P.Option);
  }
}

//===----------------------------------------------------------------------===//
// Elements
//===----------------------------------------------------------------------===//

ConditionalView ConditionalView::read(ByteReader &R) {
  ConditionalView D;
  D.MacroName = R.str();
  D.WasDefined = upTo(R, 1) != 0;
  return D;
}

ExpandsToView ExpandsToView::read(ByteReader &R) {
  ExpandsToView D;
  D.Name = R.str();
  D.TokenSequence = R.str();
  return D;
}

NameView NameView::read(ByteReader &R) { return {R.str()}; }

SymbolView SymbolView::read(ByteReader &R) {
  SymbolView D;
  D.Name = R.str();
  D.Classification = static_cast<SymbolClassification>(
      upTo(R, static_cast<uint8_t>(SymbolClassification::Template)));
  D.ResolvedTypeName = R.str();
  return D;
}

InclusionView InclusionView::read(ByteReader &R) {
  InclusionView D;
  D.HeaderPath = R.str();
  D.Why = static_cast<InclusionDependency::Reason>(
      upTo(R, static_cast<uint8_t>(InclusionDependency::Reason::Import)));
  return D;
}

FileStampView FileStampView::read(ByteReader &R) {
  FileStampView F;
  F.Path = R.str();
  F.Size = R.u64();
  F.ModTime = static_cast<int64_t>(R.u64());
  return F;
}

IncludeView IncludeView::read(ByteReader &R) {
  IncludeView D;
  D.K = static_cast<IncludeDependency::Kind>(
      upTo(R, static_cast<uint8_t>(IncludeDependency::Kind::HasIncludeNext)));
  D.Angled = upTo(R, 1) != 0;
  D.Spelled = R.str();
  D.Includer = static_cast<int32_t>(R.u32());
  D.Found = R.str();
  D.Name = R.str();
  return D;
}

FileNameView FileNameView::read(ByteReader &R) {
  FileNameView D;
  D.Which = static_cast<FileNameDependency::Macro>(
      upTo(R, static_cast<uint8_t>(FileNameDependency::Macro::FileName)));
  D.File = static_cast<int32_t>(R.u32());
  D.Text = R.str();
  return D;
}

DeclarationView DeclarationView::read(ByteReader &R) {
  DeclarationView D;
  D.In = static_cast<DeclarationDependency::Space>(
      upTo(R, static_cast<uint8_t>(DeclarationDependency::Space::Tag)));
  D.Name = R.str();
  D.Fingerprint = R.str();
  return D;
}

MacroEffectView MacroEffectView::read(ByteReader &R) {
  MacroEffectView E;
  E.Name = R.str();
  E.Defined = upTo(R, 1) != 0;
  return E;
}

MacroStackView MacroStackView::read(ByteReader &R) {
  MacroStackView Out;
  MacroStackEffect &E = Out.Effect;
  E.Name = R.str().str();
  for (uint32_t J = 0, M = R.count(); J < M && R.ok(); ++J) {
    if (upTo(R, 1))
      E.Popped.push_back(R.str().str());
    else
      E.Popped.push_back(std::nullopt);
  }
  E.Pushes = R.u32();
  return Out;
}

DiagnosticView DiagnosticView::read(ByteReader &R) {
  DiagnosticView Out;
  SavedDiagnostic &D = Out.Diagnostic;
  D.Level = R.u8();
  D.ID = R.u32();
  D.Loc = R.u64();
  D.Message = R.str().str();
  for (uint32_t J = 0, M = R.count(); J < M && R.ok(); ++J)
    D.Ranges.push_back(decodeRange(R));
  for (uint32_t J = 0, M = R.count(); J < M && R.ok(); ++J) {
    SavedDiagnostic::FixIt F;
    F.Remove = decodeRange(R);
    F.Insert = R.str().str();
    F.BeforePreviousInsertions = upTo(R, 1) != 0;
    D.FixIts.push_back(std::move(F));
  }
  return Out;
}

WarningPragmaView WarningPragmaView::read(ByteReader &R) {
  WarningPragmaView Out;
  Out.Pragma.Severity = R.u8();
  Out.Pragma.Option = R.str().str();
  return Out;
}

//===----------------------------------------------------------------------===//
// The view
//===----------------------------------------------------------------------===//

std::optional<MetadataView> MetadataView::open(llvm::StringRef Bytes) {
  ByteReader R(Bytes);
  if (R.u32() != DependencyEncodingVersion)
    return std::nullopt;
  MetadataView V;
  V.Bytes = Bytes;
  for (unsigned I = 0; I != NumSections; ++I)
    V.Starts[I] = R.u32();
  if (!R.ok() || Bytes.size() > UINT32_MAX)
    return std::nullopt;
  V.Starts[NumSections] = static_cast<uint32_t>(Bytes.size());
  // Each section holds at least its count and ends where the next starts.
  for (unsigned I = 0; I != NumSections; ++I)
    if (V.Starts[I] > V.Starts[I + 1] || V.Starts[I + 1] - V.Starts[I] < 4)
      return std::nullopt;
  // The fixed values fill what lies between the table and the first section.
  ByteReader Fixed(Bytes.slice(4 + 4 * NumSections, V.Starts[0]));
  V.Entry = decodeSnapshot(Fixed);
  V.Exit = decodeSnapshot(Fixed);
  V.WarningState = Fixed.u64();
  V.ModuleName = Fixed.str();
  V.HeaderName = Fixed.str();
  if (!Fixed.atEnd())
    return std::nullopt;
  return V;
}

bool MetadataView::validate() const {
  if (!conditional().valid() || !expandsTo().valid() ||
      !macroStacks().valid() || !inclusion().valid() || !notAMacro().valid() ||
      !symbol().valid() || !files().valid() || !includes().valid() ||
      !fileNames().valid() || !declarations().valid() ||
      !macroEffects().valid() || !diagnostics().valid() ||
      !warningPragmas().valid())
    return false;
  // A name the check looks up must be one: an empty one would pass as
  // "not a macro" whatever the compile defines.
  if (llvm::any_of(expandsTo(),
                   [](const ExpandsToView &D) { return D.Name.empty(); }) ||
      llvm::any_of(notAMacro(),
                   [](const NameView &N) { return N.Name.empty(); }) ||
      llvm::any_of(macroEffects(),
                   [](const MacroEffectView &E) { return E.Name.empty(); }))
    return false;
  // An #include names its includer, and a __FILE__ its file, by the index
  // of an #include before it; anything else would send a hit's lookups to
  // the wrong place.
  int32_t I = 0;
  for (const IncludeView &D : includes()) {
    if (D.Includer < -1 || D.Includer >= I)
      return false;
    ++I;
  }
  return llvm::all_of(fileNames(), [&](const FileNameView &D) {
    return D.File >= -1 && D.File < I;
  });
}

std::optional<HeaderDependencySet>
clang::ast_cache::decodeMetadata(llvm::StringRef Bytes,
                                 std::string *ModuleName) {
  std::optional<MetadataView> V = MetadataView::open(Bytes);
  if (!V || !V->validate())
    return std::nullopt;
  HeaderDependencySet Deps;
  for (const ConditionalView &D : V->conditional())
    Deps.Conditional.push_back({D.MacroName.str(), D.WasDefined});
  for (const ExpandsToView &D : V->expandsTo())
    Deps.Syntactic.push_back({D.Name.str(),
                              SyntacticDependency::State::ExpandsTo,
                              D.TokenSequence.str()});
  for (const NameView &N : V->notAMacro())
    Deps.Syntactic.push_back(
        {N.Name.str(), SyntacticDependency::State::NotAMacro, ""});
  for (const MacroStackView &E : V->macroStacks())
    Deps.MacroStacks.push_back(E.Effect);
  for (const InclusionView &D : V->inclusion())
    Deps.Inclusion.push_back({D.HeaderPath.str(), D.Why});
  for (const SymbolView &D : V->symbol())
    Deps.Symbol.push_back(
        {D.Name.str(), D.Classification, D.ResolvedTypeName.str()});
  for (const FileStampView &F : V->files())
    Deps.Files.push_back({F.Path.str(), F.Size, F.ModTime});
  for (const IncludeView &D : V->includes())
    Deps.Includes.push_back({D.K, D.Angled, D.Spelled.str(), D.Includer,
                             D.Found.str(), D.Name.str()});
  for (const FileNameView &D : V->fileNames())
    Deps.FileNames.push_back({D.Which, D.File, D.Text.str()});
  for (const DeclarationView &D : V->declarations())
    Deps.Declarations.push_back({D.In, D.Name.str(), D.Fingerprint.str()});
  for (const MacroEffectView &E : V->macroEffects())
    Deps.MacroEffects.push_back({E.Name.str(), E.Defined});
  for (const DiagnosticView &D : V->diagnostics())
    Deps.Diagnostics.push_back(D.Diagnostic);
  for (const WarningPragmaView &P : V->warningPragmas())
    Deps.WarningPragmas.push_back(P.Pragma);
  Deps.HeaderName = V->headerName().str();
  Deps.Entry = V->entry();
  Deps.Exit = V->exit();
  Deps.WarningState = V->warningState();
  if (ModuleName)
    *ModuleName = V->moduleName().str();
  return Deps;
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
