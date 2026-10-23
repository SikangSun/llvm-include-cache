//===- DependencySerialization.h --------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A copy's metadata in the form it is stored in: the compile that writes the
// copy encodes it once, the daemon checks it once and puts it in the shared
// region beside the copy, and every compile that considers the copy reads it
// there in place (MetadataView), with nothing decoded or allocated.
//
// The bytes are a version, a table giving where each section starts, a few
// fixed values, then the sections. A section is a count and its elements,
// each read in order; a string is length-prefixed, never NUL-terminated,
// because SyntacticDependency::TokenSequence contains embedded NULs by
// construction -- they are its token separator. The sections come in the
// order the check reads them, so a candidate rejected early touches only the
// start of its metadata.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H
#define LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H

#include "clang/ASTCache/ByteStream.h"
#include "clang/ASTCache/DependencyRecord.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <iterator>
#include <optional>

namespace clang {
namespace ast_cache {

/// Bumped whenever the encoding below changes shape. A reader that sees a
/// version it does not know declines the whole metadata rather than
/// guessing: misread metadata is metadata with the wrong contents, which is
/// exactly the failure mode that validates where it must not.
constexpr uint32_t DependencyEncodingVersion = 17;

/// Writes \p Deps, with the module name of the copy they describe, in the
/// stored form.
void encodeMetadata(const HeaderDependencySet &Deps,
                    llvm::StringRef ModuleName,
                    llvm::SmallVectorImpl<char> &Out);

/// The elements of a MetadataView: the fields of the HeaderDependencySet
/// elements of the same name, with each string pointing into the metadata.
struct ConditionalView {
  llvm::StringRef MacroName;
  bool WasDefined = false;
  uint32_t Cond = 0;
  static ConditionalView read(ByteReader &R);
};
/// A SyntacticDependency that requires ExpandsTo. Those that require
/// NotAMacro are names alone (MetadataView::notAMacro).
struct ExpandsToView {
  llvm::StringRef Name;
  llvm::StringRef TokenSequence;
  uint32_t Cond = 0;
  static ExpandsToView read(ByteReader &R);
};
struct NameView {
  llvm::StringRef Name;
  uint32_t Cond = 0;
  static NameView read(ByteReader &R);
};
struct SymbolView {
  llvm::StringRef Name;
  SymbolClassification Classification = SymbolClassification::Unknown;
  llvm::StringRef ResolvedTypeName;
  uint32_t Cond = 0;
  static SymbolView read(ByteReader &R);
};
struct InclusionView {
  llvm::StringRef HeaderPath;
  InclusionDependency::Reason Why = InclusionDependency::Reason::PragmaOnce;
  uint32_t Cond = 0;
  static InclusionView read(ByteReader &R);
};
struct FileStampView {
  llvm::StringRef Path;
  uint64_t Size = 0;
  int64_t ModTime = 0;
  uint32_t Cond = 0;
  static FileStampView read(ByteReader &R);
};
struct IncludeView {
  IncludeDependency::Kind K = IncludeDependency::Kind::Include;
  bool Angled = false;
  llvm::StringRef Spelled;
  int32_t Includer = -1;
  llvm::StringRef Found;
  llvm::StringRef Name;
  uint32_t Cond = 0;
  static IncludeView read(ByteReader &R);
};
struct FileNameView {
  FileNameDependency::Macro Which = FileNameDependency::Macro::File;
  int32_t File = -1;
  llvm::StringRef Text;
  uint32_t Cond = 0;
  static FileNameView read(ByteReader &R);
};
struct DeclarationView {
  DeclarationDependency::Space In = DeclarationDependency::Space::Ordinary;
  llvm::StringRef Name;
  llvm::StringRef Fingerprint;
  uint32_t Cond = 0;
  static DeclarationView read(ByteReader &R);
};
struct ProvidedView {
  DeclarationDependency::Space In = DeclarationDependency::Space::Ordinary;
  llvm::StringRef Name;
  llvm::StringRef Fingerprint;
  uint32_t Cond = 0;
  struct Use {
    uint32_t Index;
    ProvidedDeclaration::How Of;
    llvm::StringRef Member;
  };
  llvm::SmallVector<Use, 2> Uses;
  static ProvidedView read(ByteReader &R);
};
struct MacroEffectView {
  llvm::StringRef Name;
  bool Defined = false;
  uint32_t Cond = 0;
  static MacroEffectView read(ByteReader &R);
};
/// Elements read only where a copy is used, and rarely there at all, are
/// read whole.
struct MacroStackView {
  MacroStackEffect Effect;
  static MacroStackView read(ByteReader &R);
};
struct DiagnosticView {
  SavedDiagnostic Diagnostic;
  static DiagnosticView read(ByteReader &R);
};
struct WarningPragmaView {
  WarningPragma Pragma;
  static WarningPragmaView read(ByteReader &R);
};
/// REPORT.md T17.
struct SubmoduleView {
  llvm::StringRef Name;
  int32_t Parent = -1;
  llvm::StringRef Guard, Path, Spelled;
  bool Angled = false;
  uint32_t At = 0;
  static SubmoduleView read(ByteReader &R);
};
struct ConditionView {
  int32_t Owner = -1, Provider = -1, LeftOut = -1;
  static ConditionView read(ByteReader &R);
};
/// CLANG_HEADER_CACHE_PARTIAL.
struct LeftOutView {
  llvm::StringRef Guard, Path, Spelled;
  bool Angled = false;
  uint32_t At = 0;
  static LeftOutView read(ByteReader &R);
};
/// CLANG_HEADER_CACHE_LINK.
struct LinkView {
  llvm::StringRef Path, Module, Guard;
  bool OnGuard = false;
  llvm::SmallVector<llvm::StringRef, 4> Names;
  uint64_t Hash = 0;
  static LinkView read(ByteReader &R);
};

/// One section of a MetadataView: its elements, read in order as the loop
/// advances.
template <typename T> class MetadataSection {
public:
  using value_type = T;
  MetadataSection() = default;
  explicit MetadataSection(llvm::StringRef Bytes) : Bytes(Bytes) {}
  /// \p Count elements in \p Bytes, with no count before them: one group
  /// of a grouped section (MetadataView::group).
  MetadataSection(llvm::StringRef Bytes, uint32_t Count)
      : Bytes(Bytes), Fixed(Count) {}

  class iterator {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T *;
    using reference = const T &;

    iterator(ByteReader R, uint32_t Left) : R(R), Left(Left) {
      if (Left)
        Cur = T::read(this->R);
    }
    const T &operator*() const { return Cur; }
    const T *operator->() const { return &Cur; }
    iterator &operator++() {
      if (--Left)
        Cur = T::read(R);
      return *this;
    }
    bool operator==(const iterator &O) const { return Left == O.Left; }
    bool operator!=(const iterator &O) const { return Left != O.Left; }

  private:
    ByteReader R;
    uint32_t Left;
    T Cur;
  };

  iterator begin() const {
    ByteReader R(Bytes);
    uint32_t N = Fixed != NoCount ? Fixed : R.count();
    return iterator(R, N);
  }
  iterator end() const { return iterator(ByteReader({}), 0); }
  uint32_t size() const {
    if (Fixed != NoCount)
      return Fixed;
    ByteReader R(Bytes);
    return R.count();
  }
  bool empty() const { return size() == 0; }

  /// Reads the whole section: true if every element is in range and the
  /// section ends where its last element does.
  bool valid() const {
    ByteReader R(Bytes);
    for (uint32_t I = 0, N = Fixed != NoCount ? Fixed : R.count();
         I < N && R.ok(); ++I)
      T::read(R);
    return R.atEnd();
  }

private:
  static constexpr uint32_t NoCount = UINT32_MAX;
  llvm::StringRef Bytes;
  uint32_t Fixed = NoCount;
};

/// A copy's metadata, read in place from the bytes encodeMetadata wrote.
/// Copying a view copies a few offsets, never the metadata.
class MetadataView {
public:
  /// An empty view, for a caller to assign a found one to.
  MetadataView() = default;

  /// Reads the version, the section table and the fixed values. Nothing in
  /// the sections is read: the daemon checks every element once, when the
  /// copy is pushed (validate), and every compile after reads them as
  /// stored, as it reads the copy's PCM bytes. ByteReader still keeps every
  /// read inside \p Bytes.
  static std::optional<MetadataView> open(llvm::StringRef Bytes);

  /// Reads every section through: each element in range, each index naming
  /// an earlier #include, no name empty, nothing left over.
  bool validate() const;

  llvm::StringRef bytes() const { return Bytes; }
  llvm::StringRef moduleName() const { return ModuleName; }
  llvm::StringRef headerName() const { return HeaderName; }
  const PragmaSnapshot &entry() const { return Entry; }
  const PragmaSnapshot &exit() const { return Exit; }
  uint64_t warningState() const { return WarningState; }

  /// The sections, in the order they are stored and checked.
  MetadataSection<ConditionalView> conditional() const {
    return section<ConditionalView>(Conditional);
  }
  MetadataSection<ExpandsToView> expandsTo() const {
    return section<ExpandsToView>(ExpandsTo);
  }
  MetadataSection<MacroStackView> macroStacks() const {
    return section<MacroStackView>(MacroStacks);
  }
  MetadataSection<InclusionView> inclusion() const {
    return section<InclusionView>(Inclusion);
  }
  /// The names the SyntacticDependency elements require NotAMacro of:
  /// several hundred for a system header.
  MetadataSection<NameView> notAMacro() const {
    return section<NameView>(NotAMacro);
  }
  MetadataSection<SymbolView> symbol() const {
    return section<SymbolView>(Symbol);
  }
  MetadataSection<FileStampView> files() const {
    return section<FileStampView>(Files);
  }
  MetadataSection<IncludeView> includes() const {
    return section<IncludeView>(Includes);
  }
  MetadataSection<FileNameView> fileNames() const {
    return section<FileNameView>(FileNames);
  }
  MetadataSection<DeclarationView> declarations() const {
    return section<DeclarationView>(Declarations);
  }
  MetadataSection<MacroEffectView> macroEffects() const {
    return section<MacroEffectView>(MacroEffects);
  }
  MetadataSection<DiagnosticView> diagnostics() const {
    return section<DiagnosticView>(Diagnostics);
  }
  MetadataSection<WarningPragmaView> warningPragmas() const {
    return section<WarningPragmaView>(WarningPragmas);
  }
  MetadataSection<SubmoduleView> submodules() const {
    return section<SubmoduleView>(Submodules);
  }
  MetadataSection<ConditionView> conditions() const {
    return section<ConditionView>(Conditions);
  }
  MetadataSection<ProvidedView> provided() const {
    return section<ProvidedView>(Provided);
  }
  MetadataSection<LinkView> links() const { return section<LinkView>(Links); }
  MetadataSection<NameView> declares() const {
    return section<NameView>(Declares);
  }
  MetadataSection<LeftOutView> leftOut() const {
    return section<LeftOutView>(LeftOut);
  }

  enum Section : unsigned {
    Conditional,
    ExpandsTo,
    MacroStacks,
    Inclusion,
    NotAMacro,
    Symbol,
    Files,
    Includes,
    FileNames,
    Declarations,
    MacroEffects,
    Diagnostics,
    WarningPragmas,
    Submodules,
    Conditions,
    Provided,
    Links,
    Declares,
    LeftOut,
    Groups,
    NumSections
  };
  /// The sections a grouped copy keeps in groups, in the order their marks
  /// are stored.
  static constexpr Section GroupedSections[] = {
      Conditional, ExpandsTo, Inclusion, NotAMacro, Symbol,
      Files,       Includes,  FileNames, Declarations};
  static int groupedIndex(Section S) {
    for (unsigned I = 0; I != std::size(GroupedSections); ++I)
      if (GroupedSections[I] == S)
        return I;
    return -1;
  }

  /// CLANG_HEADER_CACHE_PIECES (HeaderDependencySet::Grouped): how many
  /// groups each grouped section is in -- the base's, then one per
  /// submodule -- or 0 for a copy that is not grouped, whose sections are
  /// read whole.
  uint32_t groups() const { return NumGroups; }
  /// Group \p G of section \p S, one of the grouped sections
  /// (GroupedSections).
  template <typename T> MetadataSection<T> group(Section S, uint32_t G) const {
    auto [Offset, Count] = groupMark(S, G);
    llvm::StringRef Elements = Bytes.slice(Starts[S] + 4, Starts[S + 1]);
    uint32_t End = G + 1 < groups() ? groupMark(S, G + 1).first
                                    : static_cast<uint32_t>(Elements.size());
    return MetadataSection<T>(Elements.slice(Offset, End), Count);
  }

private:
  /// Where group \p G of grouped section \p S starts, in bytes from its
  /// first element, and how many elements it holds.
  std::pair<uint32_t, uint32_t> groupMark(Section S, uint32_t G) const;
  template <typename T> MetadataSection<T> section(Section S) const {
    return MetadataSection<T>(Bytes.slice(Starts[S], Starts[S + 1]));
  }

  llvm::StringRef Bytes;
  /// Where each section starts; the last is where the bytes end.
  uint32_t Starts[NumSections + 1] = {};
  llvm::StringRef ModuleName, HeaderName;
  PragmaSnapshot Entry, Exit;
  uint64_t WarningState = 0;
  uint32_t NumGroups = 0;
};

/// The metadata in \p Bytes as a HeaderDependencySet, and its module name in
/// \p ModuleName when non-null. std::nullopt where validate fails: never a
/// partly filled set, whose fewer constraints would match compiles the
/// whole one does not. For tests and tools; compiles read a view.
std::optional<HeaderDependencySet>
decodeMetadata(llvm::StringRef Bytes, std::string *ModuleName = nullptr);

/// The names in a list of NUL-terminated names, such as the ask-list.
/// std::nullopt for a list that does not end in a NUL or holds an empty
/// name, so a damaged list cannot pass as a shorter one.
std::optional<llvm::SmallVector<llvm::StringRef, 0>>
splitNameList(llvm::StringRef List);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DEPENDENCYSERIALIZATION_H
