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

  // CLANG_HEADER_CACHE_PIECES: for each grouped section, where each group
  // -- the base's, then each submodule's -- starts, in bytes from the
  // section's first element, and how many elements it holds. Elements come
  // grouped (HeaderDependencySet::Grouped); one out of order leaves the copy
  // ungrouped, which a check reads whole.
  constexpr unsigned NG = std::size(MetadataView::GroupedSections);
  uint32_t G = Deps.Grouped ? Deps.Submodules.size() + 1 : 0;
  std::vector<std::pair<uint32_t, uint32_t>> Marks(G * NG, {UINT32_MAX, 0});
  uint32_t Ends[NG] = {};
  int Kind = -1;
  uint32_t Last = 0;
  size_t ElementsAt = 0;
  bool Unsorted = false;
  auto Mark = [&](uint32_t Cond) {
    if (!G || Kind < 0)
      return;
    uint32_t Group =
        Cond && Cond <= Deps.Conditions.size()
            ? static_cast<uint32_t>(Deps.Conditions[Cond - 1].Owner + 1)
            : 0;
    if (Group >= G || Group < Last) {
      Unsorted = true;
      return;
    }
    Last = Group;
    auto &M = Marks[Kind * G + Group];
    if (M.first == UINT32_MAX)
      M.first = static_cast<uint32_t>(Out.size() - ElementsAt);
    ++M.second;
  };

  // Notes where section S starts and writes its count.
  auto Begin = [&](MetadataView::Section S, size_t Count) {
    if (Kind >= 0)
      Ends[Kind] = static_cast<uint32_t>(Out.size() - ElementsAt);
    llvm::support::endian::write32le(Out.data() + Table + 4 * S,
                                     static_cast<uint32_t>(Out.size() - Base));
    W.u32(static_cast<uint32_t>(Count));
    Kind = MetadataView::groupedIndex(S);
    ElementsAt = Out.size();
    Last = 0;
  };
  using SD = SyntacticDependency;
  auto Requires = [&](SD::State S) {
    return llvm::count_if(Deps.Syntactic,
                          [&](const SD &D) { return D.Required == S; });
  };

  Begin(MetadataView::Conditional, Deps.Conditional.size());
  for (const ConditionalDependency &D : Deps.Conditional) {
    Mark(D.Cond);
    W.str(D.MacroName);
    W.u8(D.WasDefined ? 1 : 0);
    W.vu32(D.Cond);
  }

  Begin(MetadataView::ExpandsTo, Requires(SD::State::ExpandsTo));
  for (const SD &D : Deps.Syntactic)
    if (D.Required == SD::State::ExpandsTo) {
      Mark(D.Cond);
      W.str(D.Name);
      W.str(D.TokenSequence);
      W.vu32(D.Cond);
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
    Mark(D.Cond);
    W.str(D.HeaderPath);
    W.u8(static_cast<uint8_t>(D.Why));
    W.vu32(D.Cond);
  }

  Begin(MetadataView::NotAMacro, Requires(SD::State::NotAMacro));
  for (const SD &D : Deps.Syntactic)
    if (D.Required == SD::State::NotAMacro) {
      Mark(D.Cond);
      W.str(D.Name);
      W.vu32(D.Cond);
    }

  Begin(MetadataView::Symbol, Deps.Symbol.size());
  for (const SymbolDependency &D : Deps.Symbol) {
    Mark(D.Cond);
    W.str(D.Name);
    W.u8(static_cast<uint8_t>(D.Classification));
    W.str(D.ResolvedTypeName);
    W.vu32(D.Cond);
  }

  Begin(MetadataView::Files, Deps.Files.size());
  for (const FileStamp &F : Deps.Files) {
    Mark(F.Cond);
    W.str(F.Path);
    W.u64(F.Size);
    W.u64(static_cast<uint64_t>(F.ModTime));
    W.vu32(F.Cond);
  }

  Begin(MetadataView::Includes, Deps.Includes.size());
  for (const IncludeDependency &D : Deps.Includes) {
    Mark(D.Cond);
    W.u8(static_cast<uint8_t>(D.K));
    W.u8(D.Angled ? 1 : 0);
    W.str(D.Spelled);
    W.u32(static_cast<uint32_t>(D.Includer));
    W.str(D.Found);
    W.str(D.Name);
    W.vu32(D.Cond);
  }

  Begin(MetadataView::FileNames, Deps.FileNames.size());
  for (const FileNameDependency &D : Deps.FileNames) {
    Mark(D.Cond);
    W.u8(static_cast<uint8_t>(D.Which));
    W.u32(static_cast<uint32_t>(D.File));
    W.str(D.Text);
    W.vu32(D.Cond);
  }

  auto Declaration = [&](const DeclarationDependency &D) {
    W.u8(static_cast<uint8_t>(D.In));
    W.str(D.Name);
    W.str(D.Fingerprint);
    W.vu32(D.Cond);
  };
  Begin(MetadataView::Declarations, Deps.Declarations.size());
  for (const DeclarationDependency &D : Deps.Declarations) {
    Mark(D.Cond);
    Declaration(D);
  }

  Begin(MetadataView::MacroEffects, Deps.MacroEffects.size());
  for (const MacroEffect &E : Deps.MacroEffects) {
    W.str(E.Name);
    W.u8(E.Defined ? 1 : 0);
    W.vu32(E.Cond);
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
    W.vu32(D.Cond);
  }

  Begin(MetadataView::WarningPragmas, Deps.WarningPragmas.size());
  for (const WarningPragma &P : Deps.WarningPragmas) {
    W.u8(P.Severity);
    W.str(P.Option);
  }

  Begin(MetadataView::Submodules, Deps.Submodules.size());
  for (const SubmoduleRecord &S : Deps.Submodules) {
    W.str(S.Name);
    W.u32(static_cast<uint32_t>(S.Parent));
    W.str(S.Guard);
    W.str(S.Path);
    W.str(S.Spelled);
    W.u8(S.Angled ? 1 : 0);
    W.u32(S.At);
  }

  Begin(MetadataView::Conditions, Deps.Conditions.size());
  for (const DependencyCondition &C : Deps.Conditions) {
    W.u32(static_cast<uint32_t>(C.Owner));
    W.u32(static_cast<uint32_t>(C.Provider));
    W.u32(static_cast<uint32_t>(C.LeftOut));
  }

  Begin(MetadataView::Provided, Deps.Provided.size());
  for (const ProvidedDeclaration &P : Deps.Provided) {
    Declaration(P.Decl);
    W.vu32(P.Uses.size());
    for (const ProvidedDeclaration::Use &U : P.Uses) {
      W.vu32(U.Index);
      W.u8(static_cast<uint8_t>(U.Of));
      W.str(U.Member);
    }
  }

  Begin(MetadataView::Links, Deps.Links.size());
  for (const LinkRecord &L : Deps.Links) {
    W.str(L.Path);
    W.str(L.Module);
    W.str(L.Guard);
    W.u8(L.OnGuard ? 1 : 0);
    W.vu32(L.Names.size());
    for (const std::string &N : L.Names)
      W.str(N);
    W.u64(L.Hash);
  }

  Begin(MetadataView::Declares, Deps.Declares.size());
  for (const std::string &N : Deps.Declares) {
    W.str(N);
    W.vu32(0);
  }

  Begin(MetadataView::LeftOut, Deps.LeftOut.size());
  for (const LeftOutRecord &L : Deps.LeftOut) {
    W.str(L.Guard);
    W.str(L.Path);
    W.str(L.Spelled);
    W.u8(L.Angled ? 1 : 0);
    W.u32(L.At);
  }

  if (Unsorted)
    G = 0;
  Begin(MetadataView::Groups, G);
  for (unsigned K = 0; K != NG && G; ++K) {
    // A group with no elements starts where the next one does.
    uint32_t Next = Ends[K];
    for (uint32_t I = G; I-- > 0;) {
      auto &M = Marks[K * G + I];
      if (M.first == UINT32_MAX)
        M.first = Next;
      Next = M.first;
    }
    for (uint32_t I = 0; I != G; ++I) {
      W.u32(Marks[K * G + I].first);
      W.u32(Marks[K * G + I].second);
    }
  }
}

//===----------------------------------------------------------------------===//
// Elements
//===----------------------------------------------------------------------===//

ConditionalView ConditionalView::read(ByteReader &R) {
  ConditionalView D;
  D.MacroName = R.str();
  D.WasDefined = upTo(R, 1) != 0;
  D.Cond = R.vu32();
  return D;
}

ExpandsToView ExpandsToView::read(ByteReader &R) {
  ExpandsToView D;
  D.Name = R.str();
  D.TokenSequence = R.str();
  D.Cond = R.vu32();
  return D;
}

NameView NameView::read(ByteReader &R) {
  NameView N;
  N.Name = R.str();
  N.Cond = R.vu32();
  return N;
}

SymbolView SymbolView::read(ByteReader &R) {
  SymbolView D;
  D.Name = R.str();
  D.Classification = static_cast<SymbolClassification>(
      upTo(R, static_cast<uint8_t>(SymbolClassification::Template)));
  D.ResolvedTypeName = R.str();
  D.Cond = R.vu32();
  return D;
}

InclusionView InclusionView::read(ByteReader &R) {
  InclusionView D;
  D.HeaderPath = R.str();
  D.Why = static_cast<InclusionDependency::Reason>(
      upTo(R, static_cast<uint8_t>(InclusionDependency::Reason::Import)));
  D.Cond = R.vu32();
  return D;
}

FileStampView FileStampView::read(ByteReader &R) {
  FileStampView F;
  F.Path = R.str();
  F.Size = R.u64();
  F.ModTime = static_cast<int64_t>(R.u64());
  F.Cond = R.vu32();
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
  D.Cond = R.vu32();
  return D;
}

FileNameView FileNameView::read(ByteReader &R) {
  FileNameView D;
  D.Which = static_cast<FileNameDependency::Macro>(
      upTo(R, static_cast<uint8_t>(FileNameDependency::Macro::FileName)));
  D.File = static_cast<int32_t>(R.u32());
  D.Text = R.str();
  D.Cond = R.vu32();
  return D;
}

DeclarationView DeclarationView::read(ByteReader &R) {
  DeclarationView D;
  D.In = static_cast<DeclarationDependency::Space>(
      upTo(R, static_cast<uint8_t>(DeclarationDependency::Space::Tag)));
  D.Name = R.str();
  D.Fingerprint = R.str();
  D.Cond = R.vu32();
  return D;
}

ProvidedView ProvidedView::read(ByteReader &R) {
  ProvidedView P;
  DeclarationView D = DeclarationView::read(R);
  P.In = D.In;
  P.Name = D.Name;
  P.Fingerprint = D.Fingerprint;
  P.Cond = D.Cond;
  uint32_t N = R.vu32();
  for (uint32_t I = 0; I != N && R.ok(); ++I) {
    Use U;
    U.Index = R.vu32();
    U.Of = static_cast<ProvidedDeclaration::How>(
        upTo(R, static_cast<uint8_t>(ProvidedDeclaration::How::Member)));
    U.Member = R.str();
    P.Uses.push_back(U);
  }
  return P;
}

MacroEffectView MacroEffectView::read(ByteReader &R) {
  MacroEffectView E;
  E.Name = R.str();
  E.Defined = upTo(R, 1) != 0;
  E.Cond = R.vu32();
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
  D.Cond = R.vu32();
  return Out;
}

SubmoduleView SubmoduleView::read(ByteReader &R) {
  SubmoduleView S;
  S.Name = R.str();
  S.Parent = static_cast<int32_t>(R.u32());
  S.Guard = R.str();
  S.Path = R.str();
  S.Spelled = R.str();
  S.Angled = upTo(R, 1) != 0;
  S.At = R.u32();
  return S;
}

LinkView LinkView::read(ByteReader &R) {
  LinkView L;
  L.Path = R.str();
  L.Module = R.str();
  L.Guard = R.str();
  L.OnGuard = upTo(R, 1) != 0;
  for (uint32_t I = 0, N = R.vu32(); I != N && R.ok(); ++I)
    L.Names.push_back(R.str());
  L.Hash = R.u64();
  return L;
}

ConditionView ConditionView::read(ByteReader &R) {
  ConditionView C;
  C.Owner = static_cast<int32_t>(R.u32());
  C.Provider = static_cast<int32_t>(R.u32());
  C.LeftOut = static_cast<int32_t>(R.u32());
  return C;
}

LeftOutView LeftOutView::read(ByteReader &R) {
  LeftOutView L;
  L.Guard = R.str();
  L.Path = R.str();
  L.Spelled = R.str();
  L.Angled = upTo(R, 1) != 0;
  L.At = R.u32();
  return L;
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
  ByteReader Marks(Bytes.slice(V.Starts[Groups], V.Starts[Groups + 1]));
  V.NumGroups = Marks.count();
  // Each group of each grouped section has its mark.
  if (V.Starts[Groups + 1] - V.Starts[Groups] !=
      4 + 8 * uint64_t(V.NumGroups) * std::size(GroupedSections))
    return std::nullopt;
  return V;
}

std::pair<uint32_t, uint32_t> MetadataView::groupMark(Section S,
                                                      uint32_t G) const {
  ByteReader R(Bytes.slice(Starts[Groups] + 4 +
                               8 * (groupedIndex(S) * NumGroups + G),
                           Starts[Groups + 1]));
  uint32_t Offset = R.u32();
  uint32_t Count = R.u32();
  return {Offset, Count};
}

bool MetadataView::validate() const {
  if (!conditional().valid() || !expandsTo().valid() ||
      !macroStacks().valid() || !inclusion().valid() || !notAMacro().valid() ||
      !symbol().valid() || !files().valid() || !includes().valid() ||
      !fileNames().valid() || !declarations().valid() ||
      !macroEffects().valid() || !diagnostics().valid() ||
      !warningPragmas().valid() || !submodules().valid() ||
      !conditions().valid() || !provided().valid() || !links().valid() ||
      !declares().valid() || !leftOut().valid())
    return false;
  // A left-out header names its guard, which decides whether to plug it
  // in, and its file and spelling, which plug it in.
  if (llvm::any_of(leftOut(), [](const LeftOutView &L) {
        return L.Guard.empty() || L.Path.empty() || L.Spelled.empty();
      }))
    return false;
  // A link names the copy it loads; one skipped by its guard names that.
  if (llvm::any_of(links(), [](const LinkView &L) {
        return L.Path.empty() || L.Module.empty() ||
               (L.OnGuard && L.Guard.empty());
      }))
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
  if (!llvm::all_of(fileNames(), [&](const FileNameView &D) {
        return D.File >= -1 && D.File < I;
      }))
    return false;
  // REPORT.md T17: each submodule inside an earlier one or the copy's own,
  // named, with a guard or a file to tell whether the compile read it; each
  // condition naming submodules there are; each element's condition one
  // there is. Otherwise an element would be checked, or skipped, by a
  // submodule that does not exist.
  int32_t S = 0;
  for (const SubmoduleView &M : submodules()) {
    if (M.Name.empty() || M.Parent < -1 || M.Parent >= S ||
        (M.Guard.empty() && M.Path.empty()))
      return false;
    ++S;
  }
  int32_t NL = leftOut().size();
  if (!llvm::all_of(conditions(), [&](const ConditionView &C) {
        return C.Owner >= -1 && C.Owner < S && C.Provider >= -1 &&
               C.Provider < S && C.LeftOut >= -1 && C.LeftOut < NL;
      }))
    return false;
  uint32_t NC = conditions().size();
  auto Fits = [&](auto Section) {
    return llvm::all_of(Section, [&](const auto &E) { return E.Cond <= NC; });
  };
  // CLANG_HEADER_CACHE_PIECES: a grouped copy has a group per submodule
  // and the base's; in each grouped section the groups start in order,
  // hold every element between them, and each element is its group's.
  uint32_t G = groups();
  if (Starts[Groups + 1] - Starts[Groups] !=
      4 + 8 * G * std::size(GroupedSections))
    return false;
  if (G) {
    if (G != uint32_t(S) + 1)
      return false;
    std::vector<int32_t> OwnerOf{-1};
    for (const ConditionView &C : conditions())
      OwnerOf.push_back(C.Owner);
    auto Grouped = [&](auto Whole, Section Sec, auto *Tag) {
      using T = std::remove_pointer_t<decltype(Tag)>;
      uint32_t Total = 0, Prev = 0;
      uint32_t Size = Starts[Sec + 1] - Starts[Sec] - 4;
      for (uint32_t I = 0; I != G; ++I) {
        auto [Offset, Count] = groupMark(Sec, I);
        if (Offset < Prev || Offset > Size)
          return false;
        Prev = Offset;
        MetadataSection<T> Part = group<T>(Sec, I);
        if (!Part.valid())
          return false;
        for (const T &E : Part)
          if (E.Cond > NC || OwnerOf[E.Cond] + 1 != int32_t(I))
            return false;
        Total += Count;
      }
      return Total == Whole.size();
    };
    if (!Grouped(conditional(), Conditional, (ConditionalView *)nullptr) ||
        !Grouped(expandsTo(), ExpandsTo, (ExpandsToView *)nullptr) ||
        !Grouped(inclusion(), Inclusion, (InclusionView *)nullptr) ||
        !Grouped(notAMacro(), NotAMacro, (NameView *)nullptr) ||
        !Grouped(symbol(), Symbol, (SymbolView *)nullptr) ||
        !Grouped(files(), Files, (FileStampView *)nullptr) ||
        !Grouped(includes(), Includes, (IncludeView *)nullptr) ||
        !Grouped(fileNames(), FileNames, (FileNameView *)nullptr) ||
        !Grouped(declarations(), Declarations, (DeclarationView *)nullptr))
      return false;
  }
  return Fits(conditional()) && Fits(expandsTo()) && Fits(inclusion()) &&
         Fits(notAMacro()) && Fits(symbol()) && Fits(files()) &&
         Fits(includes()) && Fits(fileNames()) && Fits(declarations()) &&
         Fits(macroEffects()) && Fits(provided()) &&
         llvm::all_of(diagnostics(), [&](const DiagnosticView &D) {
           return D.Diagnostic.Cond <= NC;
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
    Deps.Conditional.push_back({D.MacroName.str(), D.WasDefined, D.Cond});
  for (const ExpandsToView &D : V->expandsTo())
    Deps.Syntactic.push_back({D.Name.str(),
                              SyntacticDependency::State::ExpandsTo,
                              D.TokenSequence.str(), D.Cond});
  for (const NameView &N : V->notAMacro())
    Deps.Syntactic.push_back(
        {N.Name.str(), SyntacticDependency::State::NotAMacro, "", N.Cond});
  for (const MacroStackView &E : V->macroStacks())
    Deps.MacroStacks.push_back(E.Effect);
  for (const InclusionView &D : V->inclusion())
    Deps.Inclusion.push_back({D.HeaderPath.str(), D.Why, D.Cond});
  for (const SymbolView &D : V->symbol())
    Deps.Symbol.push_back(
        {D.Name.str(), D.Classification, D.ResolvedTypeName.str(), D.Cond});
  for (const FileStampView &F : V->files())
    Deps.Files.push_back({F.Path.str(), F.Size, F.ModTime, F.Cond});
  for (const IncludeView &D : V->includes())
    Deps.Includes.push_back({D.K, D.Angled, D.Spelled.str(), D.Includer,
                             D.Found.str(), D.Name.str(), D.Cond});
  for (const FileNameView &D : V->fileNames())
    Deps.FileNames.push_back({D.Which, D.File, D.Text.str(), D.Cond});
  for (const DeclarationView &D : V->declarations())
    Deps.Declarations.push_back(
        {D.In, D.Name.str(), D.Fingerprint.str(), D.Cond});
  for (const MacroEffectView &E : V->macroEffects())
    Deps.MacroEffects.push_back({E.Name.str(), E.Defined, E.Cond});
  for (const DiagnosticView &D : V->diagnostics())
    Deps.Diagnostics.push_back(D.Diagnostic);
  for (const WarningPragmaView &P : V->warningPragmas())
    Deps.WarningPragmas.push_back(P.Pragma);
  for (const SubmoduleView &S : V->submodules())
    Deps.Submodules.push_back({S.Name.str(), S.Parent, S.Guard.str(),
                               S.Path.str(), S.Spelled.str(), S.Angled, S.At});
  for (const ConditionView &C : V->conditions())
    Deps.Conditions.push_back({C.Owner, C.Provider, C.LeftOut});
  for (const LeftOutView &L : V->leftOut())
    Deps.LeftOut.push_back(
        {L.Guard.str(), L.Path.str(), L.Spelled.str(), L.Angled, L.At});
  for (const ProvidedView &D : V->provided()) {
    ProvidedDeclaration P;
    P.Decl = {D.In, D.Name.str(), D.Fingerprint.str(), D.Cond};
    for (const ProvidedView::Use &U : D.Uses)
      P.Uses.push_back({U.Index, U.Of, U.Member.str()});
    Deps.Provided.push_back(std::move(P));
  }
  for (const NameView &N : V->declares())
    Deps.Declares.push_back(N.Name.str());
  for (const LinkView &L : V->links()) {
    LinkRecord R{L.Path.str(), L.Module.str(), L.Guard.str(), L.OnGuard, {},
                 L.Hash};
    for (llvm::StringRef N : L.Names)
      R.Names.push_back(N.str());
    Deps.Links.push_back(std::move(R));
  }
  Deps.Grouped = V->groups() != 0;
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
