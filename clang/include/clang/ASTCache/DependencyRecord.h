//===- DependencyRecord.h --------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The data model for a header's AST-cache dependency set: the pieces of
// external state a cached AST for that header actually depends on.
//
// A macro (or, for SymbolDependency, an ordinary identifier) is only a
// dependency at all if its meaning could vary depending on state outside
// the header's own transitive-include closure -- something defined and
// used entirely within the header's own (stable, unchanging-during-a-
// compile) text is never a dependency, no matter how "structurally
// significant" the position it's used in looks. See
// ASTCacheDependencyTracker.h for how these are actually collected.
//
// Deliberately has no dependency on clang::Sema/clang::NameClassification
// (a heavy header) -- SymbolDependency::Classification mirrors
// clang::NameClassificationKind's small integer values without including
// Sema.h; the one place that needs the real enum casts at the point of use.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_DEPENDENCYRECORD_H
#define LLVM_CLANG_ASTCACHE_DEPENDENCYRECORD_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace ast_cache {

/// A macro's *definedness* gates a branch (#ifdef/#ifndef/#if defined()).
/// A hit requires the live macro's defined-ness to match WasDefined.
struct ConditionalDependency {
  std::string MacroName;
  bool WasDefined = false;
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// An identifier the preprocessor considered for macro expansion while
/// processing the header, and the macro state it had the first time. Any
/// identifier-shaped token can be turned into something else by an earlier
/// #define -- keywords included -- so each one the header's text produced is
/// a dependency on the includer's macro table.
///
/// Recorded only when that state could vary: a name first seen as a macro
/// defined inside the header's own closure has no requirement, since the
/// header's text pins it. See ASTCacheDependencyTracker for where the names
/// come from.
struct SyntacticDependency {
  enum class State : uint8_t {
    /// The name was not a macro. A hit requires that it is not one now.
    NotAMacro,
    /// The name was a macro defined outside the header. A hit requires that
    /// it is one now, with the exact same replacement tokens.
    ExpandsTo,
  };

  std::string Name;
  State Required = State::NotAMacro;
  /// For ExpandsTo: the replacement-list tokens' spellings, concatenated with
  /// a separator that cannot appear in a single spelling, so two different
  /// token sequences never collide into the same recorded string.
  std::string TokenSequence;
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// An identifier's grammar-ambiguity classification (is it a type-name,
/// and if so which one) was relied on while parsing this header. Mirrors
/// clang::NameClassificationKind's cases relevant to this check; see
/// ASTCacheSymbolDependencyTracker.h for the real enum this is cast from.
enum class SymbolClassification : uint8_t {
  Unknown = 0,
  Type = 1,
  NonType = 2,
  Template = 3,
};

struct SymbolDependency {
  std::string Name;
  SymbolClassification Classification = SymbolClassification::Unknown;
  /// For Classification == Type, a stable textual spelling of the
  /// resolved type (e.g. its canonical name), so a hit can check "still
  /// resolves to the same type," not just "still resolves to some type."
  std::string ResolvedTypeName;
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// An #include inside the header was suppressed by Clang's multiple-include
/// optimization, so the file was never opened and none of its own directives
/// fired. Where the suppression came from a guard macro it reduces to an
/// ordinary ConditionalDependency on that macro, and is recorded as one --
/// skipping is semantically just "entered the file and its #ifndef was
/// immediately false." #pragma once and #import have no controlling macro, so
/// there is no macro state to compare and the dependency is on file identity
/// instead: a hit requires that this file has likewise already been included.
struct InclusionDependency {
  std::string HeaderPath;
  enum class Reason : uint8_t { PragmaOnce, Import };
  Reason Why = Reason::PragmaOnce;
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// An #include, #include_next, __has_include or __has_include_next inside
/// the header's region, and the file it found. The file depends on the
/// compile's include path: qemu compiles some files once per target CPU, and
/// include/system/kvm.h's `#include "cpu.h"` finds target/arm/cpu.h under
/// -I../target/arm and target/i386/cpu.h under -I../target/i386. A hit redoes
/// each lookup in the loading compile and requires the same file.
///
/// Recorded in the order the directives ran, so an entry's includer is always
/// an earlier entry, or the header itself.
struct IncludeDependency {
  enum class Kind : uint8_t {
    Include,
    IncludeNext,
    HasInclude,
    HasIncludeNext
  };
  Kind K = Kind::Include;
  bool Angled = false;
  /// The name between the quotes or angle brackets, after macro expansion.
  std::string Spelled;
  /// Where the directive is written: -1 for the header itself, otherwise the
  /// index of the entry that found the file holding it.
  int32_t Includer = -1;
  /// The real path of the file found. Empty when __has_include found nothing.
  std::string Found;
  /// The name the lookup found the file by, "./ae.h" or
  /// "/usr/include/stdio.h". A part exported from the real compile holds its
  /// files under these names, which __FILE__ and debug information repeat, so
  /// a hit requires the same names. Empty for a copy the separate producer
  /// built, whose names are its own.
  std::string Name;

  friend bool operator==(const IncludeDependency &A,
                         const IncludeDependency &B) {
    return A.K == B.K && A.Angled == B.Angled && A.Spelled == B.Spelled &&
           A.Includer == B.Includer && A.Found == B.Found && A.Name == B.Name;
  }
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// A __FILE__ or __FILE_NAME__ expanded in the header or in a file the header
/// reached, and the text it produced. __FILE__ names the file by the path the
/// compile opened it with, not by the file itself: redis's ae.c includes
/// ae_epoll.c, found beside it, and a compile run from src/ calls it
/// "./ae_epoll.c" while one that opened it by absolute path calls it
/// "/.../src/ae_epoll.c". The text is in the parsed header, so a hit requires
/// this compile to produce the same text. A #line in the file's own text
/// names the file the same way in every compile, so a use after one is not
/// recorded.
struct FileNameDependency {
  enum class Macro : uint8_t { File, FileName };
  Macro Which = Macro::File;
  /// The file the macro was expanded in: -1 for the header itself, otherwise
  /// the index of the IncludeDependency that found it.
  int32_t File = -1;
  /// What the macro produced, without its quotes: after -fmacro-prefix-map,
  /// and for __FILE_NAME__ only the last path component.
  std::string Text;

  friend bool operator==(const FileNameDependency &A,
                         const FileNameDependency &B) {
    return A.Which == B.Which && A.File == B.File && A.Text == B.Text;
  }
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// A declaration from outside the header that the header's part carries a
/// copy of, because the part's own declarations refer to it: anet.h declares
/// functions taking uint32_t, so anet.h's part holds stdint.h's typedef of
/// uint32_t. Loading the part merges each copy with this compile's own
/// declaration of the name, so a hit requires one, with the same fingerprint
/// (declarationFingerprint): a struct fd_set with other members, or an
/// fscanf with another asm label, cannot merge, and would stop the compile.
struct DeclarationDependency {
  /// Which C name space the name is looked up in: struct, union and enum
  /// tags are one, everything else the other.
  enum class Space : uint8_t { Ordinary, Tag };
  Space In = Space::Ordinary;
  std::string Name;
  std::string Fingerprint;

  friend bool operator==(const DeclarationDependency &A,
                         const DeclarationDependency &B) {
    return A.In == B.In && A.Name == B.Name && A.Fingerprint == B.Fingerprint;
  }
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// A file whose text a part holds the parse of, with its size and
/// modification time when the part was saved. A plain compile reads every
/// file it includes, so it always uses the current text; a hit uses the text
/// the part was saved from, so it requires each file unchanged. Clang's own
/// modules check their input files the same way. An edit within the same
/// second that keeps the size is not seen, as with Clang's modules.
struct FileStamp {
  /// The real path.
  std::string Path;
  uint64_t Size = 0;
  /// Seconds since the epoch, as FileEntry::getModificationTime gives it.
  int64_t ModTime = 0;

  friend bool operator==(const FileStamp &A, const FileStamp &B) {
    return A.Path == B.Path && A.Size == B.Size && A.ModTime == B.ModTime;
  }
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// The pragma state that was active at a single point, reduced to the values
/// that actually reach a Decl. Only the small-value pragmas are represented:
/// a header reached under one of the others (#pragma clang attribute push,
/// #pragma GCC visibility) is declined outright rather than described here,
/// since their state is a parsed attribute or a private stack type and is
/// neither comparable nor replayable.
struct PragmaSnapshot {
  /// Sema::AlignPackInfo::getRawEncoding of the current #pragma pack/align
  /// value. Zero is not the default -- the default is whatever the invocation
  /// established -- so this is only ever compared, never interpreted.
  uint32_t AlignPackRaw = 0;
  /// FPOptionsOverride::getAsOpaqueInt of the current #pragma float_control
  /// state.
  uint64_t FpPragmaRaw = 0;

  friend bool operator==(const PragmaSnapshot &A, const PragmaSnapshot &B) {
    return A.AlignPackRaw == B.AlignPackRaw && A.FpPragmaRaw == B.FpPragmaRaw;
  }
  friend bool operator!=(const PragmaSnapshot &A, const PragmaSnapshot &B) {
    return !(A == B);
  }
};

/// A macro the header's own text defined or undefined, and whether it was
/// defined when the header ended. This is an *effect*, not a dependency:
/// included as text, the header leaves these macros in this state, and a
/// loaded result must leave them the same way. Module macro visibility alone
/// does not: a module's #undef of a macro the includer defined does not hide
/// the includer's definition, and two loaded results that define one name
/// differently leave it ambiguous rather than set to the later one.
struct MacroEffect {
  std::string Name;
  bool Defined = false;
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// What the header's #pragma push_macro and pop_macro did to one name's
/// stack of saved definitions, seen from outside the header (REPORT.md Q11).
/// A push and its pop inside the header leave nothing.
///
/// Popped is a dependency: the definitions the header's pops took from below
/// it, the top of the stack first, each as macro tokens, or none where the
/// name was not a macro. A hit requires the same at the top of the loading
/// compile's stack. Pops and Pushes are the effect: a hit pops as many, then
/// pushes as many of the definition that leaves, as the header's text would.
struct MacroStackEffect {
  std::string Name;
  std::vector<std::optional<std::string>> Popped;
  uint32_t Pushes = 0;
};

/// A #pragma GCC diagnostic or #pragma clang diagnostic: the severity it
/// sets (diag::Severity) for an option such as "-Wsign-compare".
struct WarningPragma {
  uint8_t Severity = 0;
  std::string Option;

  friend bool operator==(const WarningPragma &A, const WarningPragma &B) {
    return A.Severity == B.Severity && A.Option == B.Option;
  }
};

/// A diagnostic Clang printed while the header was open, in the compile that
/// wrote the copy. A hit prints it again: the header's text, skipped, would
/// have printed it (REPORT.md Q34). Each location is in the copy's own
/// encoding (ASTWriter::getRawSourceLocationEncoding), which the loading
/// compile's reader translates as it translates any location in the copy.
struct SavedDiagnostic {
  /// DiagnosticsEngine::Level: a warning, an error -Werror made of one, a
  /// remark, or a note on the diagnostic before it.
  uint8_t Level = 0;
  uint32_t ID = 0;
  uint64_t Loc = 0;
  std::string Message;
  struct Range {
    uint64_t Begin = 0, End = 0;
    bool TokenRange = false;
  };
  std::vector<Range> Ranges;
  struct FixIt {
    Range Remove;
    std::string Insert;
    bool BeforePreviousInsertions = false;
  };
  std::vector<FixIt> FixIts;
  /// When it applies (HeaderDependencySet::Conditions): 0 always, else the
  /// index of its condition plus one (REPORT.md T17).
  uint32_t Cond = 0;
};

/// A header inside a copy held as a submodule of the copy's module
/// (REPORT.md T17): git's refs.h's copy holds object.h as one. Where the
/// compile has read object.h already -- its guard is defined -- the
/// submodule stays hidden and its dependencies are not checked; elsewhere
/// they are, and it is made visible.
struct SubmoduleRecord {
  /// The submodule's name in the copy's module.
  std::string Name;
  /// The submodule holding this one, by index; -1 for the copy's module.
  int32_t Parent = -1;
  /// The header's include guard; empty for #pragma once, with Path the file.
  std::string Guard;
  std::string Path;
};

/// When a dependency applies: where its submodule (Owner) is loaded, and,
/// for one taken from a submodule by the lines outside it (Provider), where
/// that submodule is skipped. -1 for none.
struct DependencyCondition {
  int32_t Owner = -1;
  int32_t Provider = -1;
};

/// What a part's lines outside a submodule take from it (REPORT.md T17):
/// refs.h's lines use object.h's struct object. Where the submodule is
/// skipped, the compile's own declaration of the name, with the same
/// fingerprint, stands for each of the part's declarations in Uses, which
/// the reader then never reads.
struct ProvidedDeclaration {
  DeclarationDependency Decl;
  /// How a declaration of the part is found from the compile's one.
  enum class How : uint8_t { Self, TagOfTypedef, Member };
  struct Use {
    /// The part's local declaration index.
    uint32_t Index = 0;
    How Of = How::Self;
    /// For How::Member, the member of the struct or union (the typedef's,
    /// for a typedef) by name.
    std::string Member;
    friend bool operator==(const Use &A, const Use &B) {
      return A.Index == B.Index && A.Of == B.Of && A.Member == B.Member;
    }
  };
  std::vector<Use> Uses;
  friend bool operator==(const ProvidedDeclaration &A,
                         const ProvidedDeclaration &B) {
    return A.Decl == B.Decl && A.Decl.Cond == B.Decl.Cond && A.Uses == B.Uses;
  }
};

struct HeaderDependencySet {
  std::vector<ConditionalDependency> Conditional;
  std::vector<SyntacticDependency> Syntactic;
  std::vector<SymbolDependency> Symbol;
  std::vector<InclusionDependency> Inclusion;
  std::vector<IncludeDependency> Includes;
  std::vector<FileNameDependency> FileNames;
  std::vector<DeclarationDependency> Declarations;
  /// Each file the part holds the parse of: the header and every file
  /// entered while it was open. Empty for a copy the separate producer built.
  std::vector<FileStamp> Files;

  /// The name the compile that exported the part opened the header by
  /// ("./anet.h"); a hit requires the same. Empty for a copy the separate
  /// producer built.
  std::string HeaderName;

  /// Pragma state where the header was reached. This is an *input* to the
  /// header's compilation -- "#pragma pack(1)" before an #include changes the
  /// layout of every record inside it, with no macro tested and none expanded
  /// -- so it is part of what a hit must match.
  PragmaSnapshot Entry;

  /// Pragma state the header left behind. This is an *output*: included
  /// textually, a header that sets pack without popping leaves its includer
  /// packed, and loading a stored result does not replay that. The client
  /// applies the difference from Entry after loading.
  PragmaSnapshot Exit;

  /// Every macro the header defined or undefined, each once, in the order
  /// first defined or undefined; less those its push_macro and pop_macro
  /// leave as they were (MacroStacks).
  std::vector<MacroEffect> MacroEffects;

  /// What decided which diagnostics the header printed where it was
  /// included: the compile's warning flags and the warning pragmas in force
  /// there, hashed (warningState). A hit requires the same.
  uint64_t WarningState = 0;

  /// The diagnostics the header's text printed, in order.
  std::vector<SavedDiagnostic> Diagnostics;

  /// The warning pragmas the header left in force for its includer, in
  /// order. A hit applies them again where the header is included.
  std::vector<WarningPragma> WarningPragmas;

  /// The header's pushes and pops that reach outside it, one per name. A hit
  /// checks Popped and replays the rest before MacroEffects.
  std::vector<MacroStackEffect> MacroStacks;
  /// REPORT.md T17: the headers held as submodules, the conditions the
  /// elements above refer to, and the declarations the copy's lines take
  /// from a submodule, checked against the compile's own where it is
  /// skipped.
  std::vector<SubmoduleRecord> Submodules;
  std::vector<DependencyCondition> Conditions;
  std::vector<ProvidedDeclaration> Provided;
};

/// One header's dependency set, as observed over the region between entering
/// that header and leaving it.
struct HeaderRecord {
  std::string HeaderPath;
  HeaderDependencySet Deps;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_DEPENDENCYRECORD_H
