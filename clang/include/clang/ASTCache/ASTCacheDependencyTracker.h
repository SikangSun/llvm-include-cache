//===- ASTCacheDependencyTracker.h -----------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A PPCallbacks observer, driven during AST-cache population, that collects
// one dependency set per header entered during a real compile (see
// DependencyRecord.h).
//
// Headers nest, so this tracks a *stack* of in-flight frames rather than one
// TU-wide set: a frame is pushed when a real file is entered and closed when
// it is exited, and each frame carries its own transitive-include closure and
// its own dependency set. That nesting is what makes the provenance check
// mean the right thing at every level. A macro defined in a.h and used in the
// b.h it includes is external to b.h's frame (a genuine dependency of b.h)
// and internal to a.h's frame (not a dependency of a.h, since a.h's own text
// pins it). One MacroExpands event is therefore asked the question once per
// open frame, and can answer differently at each.
//
// Besides being attached with addPPCallbacks, the tracker must be registered
// with Preprocessor::setExpansionCandidateObserver: identifiers that are not
// macros reach no PPCallbacks event, and those are most syntactic
// dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_ASTCACHEDEPENDENCYTRACKER_H
#define LLVM_CLANG_ASTCACHE_ASTCACHEDEPENDENCYTRACKER_H

#include "clang/ASTCache/DependencyRecord.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/ExpansionCandidateObserver.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Sema/NameClassificationObserver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace clang {
class IdentifierInfo;
class MacroInfo;
class Preprocessor;

namespace ast_cache {
class MetadataView;

/// Collects one HeaderDependencySet per header processed by the Preprocessor
/// it is attached to. See DependencyRecord.h for what each dependency
/// category means.
class ASTCacheDependencyTracker : public PPCallbacks,
                                  public ExpansionCandidateObserver,
                                  public NameClassificationObserver {
public:
  explicit ASTCacheDependencyTracker(Preprocessor &PP);

  /// Pushes a frame on EnterFile and closes it on ExitFile. Every real file
  /// entered is added to the closure of *every* open frame, so each frame's
  /// closure is exactly "this file plus everything entered beneath it."
  ///
  /// Synthetic buffers (predefines/"<built-in>", -D and -include command-line
  /// buffers) have no backing FileEntry and are deliberately excluded: they
  /// carry externally supplied state, so absorbing them into a closure would
  /// misclassify every command-line macro as internal.
  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                    SrcMgr::CharacteristicKind FileType,
                    FileID PrevFID) override;

  /// An #include suppressed by Clang's multiple-include optimization. The
  /// file is never opened, so none of its own directives fire -- in
  /// particular the #ifndef of its include guard never reaches Ifndef below.
  /// The suppression is nonetheless a real dependency: the same #include
  /// would have been *entered* in a translation unit where the guard was not
  /// yet defined, giving the header a different closure and different
  /// content. Recorded here so it is not silently invisible.
  void FileSkipped(const FileEntryRef &SkippedFile, const Token &FilenameTok,
                    SrcMgr::CharacteristicKind FileType) override;

  /// Records the lookup in every open frame whose region holds the
  /// directive (IncludeDependency), and remembers which entry the file about
  /// to be entered came from.
  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef RelativePath, const Module *SuggestedModule,
                          bool ModuleImported,
                          SrcMgr::CharacteristicKind FileType) override;

  /// The same for __has_include and __has_include_next, whose answer also
  /// depends on the include path.
  void HasInclude(SourceLocation Loc, StringRef FileName, bool IsAngled,
                  OptionalFileEntryRef File,
                  SrcMgr::CharacteristicKind FileType) override;

  /// Conditional dependencies: a definedness test, recorded in each open
  /// frame the first time that frame sees the name -- unless the state tested
  /// was set inside that frame's closure. Dependencies face strictly outward:
  /// redis's fmacros.h defines _BSD_SOURCE and then includes <features.h>,
  /// which tests it. For features.h the definition came from outside, so it
  /// is a dependency; for fmacros.h its own text set it, and recording "must
  /// be defined" would demand of every includer a macro none defines before
  /// including fmacros.h. The state comes from the active #define, or from
  /// the #undef that removed it; a name never defined, or defined on the
  /// command line, is always external.
  void Ifdef(SourceLocation Loc, const Token &MacroNameTok,
             const MacroDefinition &MD) override;
  void Ifndef(SourceLocation Loc, const Token &MacroNameTok,
              const MacroDefinition &MD) override;

  /// A defined() test is held until its #if or #elif is evaluated, and
  /// recorded only if DefinedOperatorsNeeded then says the result needed it.
  /// Clang's __stddef_null.h tests
  ///   #if !defined(NULL) || !__building_module(_Builtin_stddef)
  /// whose right side is true in every compile that is not building Clang's
  /// own stddef module: the lines are kept whatever NULL is, so recording
  /// "NULL was defined" would only reject the header where NULL is not.
  ///
  /// A test the result did not need is dropped without counting as the
  /// frame's first test of the name, so a later test of it is still
  /// recorded. Only defined() answers are dropped: whether each other name in
  /// the expression is a macro, and what it expands to, decides the
  /// expression's tokens, and stays recorded through expansionCandidate and
  /// UndefinedIdentifierInDirective.
  void Defined(const Token &MacroNameTok, const MacroDefinition &MD,
               SourceRange Range) override;
  void DefinedOperatorsNeeded(bool Known,
                              ArrayRef<SourceLocation> Needed) override;

  /// Held tests left over when the #if or #elif's outcome is reported --
  /// which would mean the evaluator returned without naming what it needed
  /// -- are all recorded.
  void If(SourceLocation Loc, SourceRange ConditionRange,
          ConditionValueKind ConditionValue) override;
  void Elif(SourceLocation Loc, SourceRange ConditionRange,
            ConditionValueKind ConditionValue, SourceLocation IfLoc) override;

  /// #elifdef/#elifndef are definedness tests exactly like #ifdef/#ifndef and
  /// are recorded the same way.
  ///
  /// Each has two overloads: one carrying the macro name, fired when the
  /// branch is actually evaluated, and one carrying only a source range,
  /// fired when an earlier branch already won and this one is skipped. Both
  /// are overridden because overriding either alone would hide the other by
  /// ordinary C++ name hiding, silently losing the callback. The skipped form
  /// records nothing: the branch was never tested, so its macro did not
  /// influence the outcome, and whatever made the earlier branch win is
  /// already recorded as a dependency in its own right.
  void Elifdef(SourceLocation Loc, const Token &MacroNameTok,
               const MacroDefinition &MD) override;
  void Elifdef(SourceLocation Loc, SourceRange ConditionRange,
               SourceLocation IfLoc) override {}
  void Elifndef(SourceLocation Loc, const Token &MacroNameTok,
                const MacroDefinition &MD) override;
  void Elifndef(SourceLocation Loc, SourceRange ConditionRange,
                SourceLocation IfLoc) override {}

  /// An identifier in an #if/#elif expression that had no macro definition
  /// and was taken as 0. This is the same kind of dependency as any other
  /// definedness test -- the branch was chosen because the macro was absent,
  /// and a compile that defines it must not reuse this artifact -- but no
  /// other callback reports it, so without this the dependency would simply
  /// be missing, and a missing dependency matches every compile.
  void UndefinedIdentifierInDirective(const Token &IdentifierTok) override;

  /// Syntactic dependencies: every identifier that reaches the
  /// macro-expansion decision, recorded in each open frame with the state it
  /// has the first time that frame sees it --
  ///   - not a macro: NotAMacro;
  ///   - a macro whose active #define lies outside that frame's closure:
  ///     ExpandsTo, with its replacement tokens;
  ///   - a macro defined inside that frame's closure: nothing, since the
  ///     header's own text pins it.
  /// Only the first sighting counts. A header that uses BUF and later
  /// #defines it depended on BUF not being a macro at that first use; a
  /// header that #defines BUF and then uses it did not depend on the
  /// includer at all.
  void expansionCandidate(const Token &Identifier) override;

  /// A #pragma pop_macro that takes what was pushed before a frame's header
  /// leaves the name at the definition it took. The part's metadata checks
  /// that definition (MacroStackEffect), so the frame records nothing more
  /// for the name; without this, its first use after the pop would be
  /// checked against the definition at the #include (REPORT.md Q11).
  void PragmaPushMacro(SourceLocation Loc, const IdentifierInfo *II,
                       const MacroInfo *Saved) override;
  void PragmaPopMacro(SourceLocation Loc, const IdentifierInfo *II,
                      const MacroInfo *Taken) override;

  /// The same record, reached from an actual expansion. Every expansion is
  /// preceded by an expansionCandidate for the same token, so this only
  /// matters if some path reaches HandleIdentifier without one -- in which
  /// case recording here keeps the dependency rather than dropping it.
  ///
  /// Also where a __FILE__ or __FILE_NAME__ is seen, whose text is recorded,
  /// and where __has_include_next is told apart from __has_include.
  void MacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                     SourceRange Range, const MacroArgs *Args) override;

  /// Symbol dependencies -- the grammar-ambiguity resolutions this header
  /// relied on. Filed into every open frame whose closure contains the query
  /// but *not* the declaration that answered it: a declaration inside a
  /// frame's own closure is pinned by that header's text and cannot vary,
  /// exactly as for a macro definition. A name that resolved to nothing is
  /// always a dependency, since a translation unit that declares it parses
  /// differently.
  void recordTypeNameQuery(const IdentifierInfo *Name, bool IsType,
                            const QueryContext &Ctx) override;
  void recordClassification(const IdentifierInfo *Name,
                             const Sema::NameClassification &Result,
                             const QueryContext &Ctx) override;

  /// C++-only queries. This tracker is scoped to C, whose only
  /// symbol-dependent ambiguity is whether an identifier names a type; both
  /// of these sit after the C early-exit in
  /// Parser::TryAnnotateTypeOrScopeTokenAfterScopeSpec and are unreachable
  /// while compiling C. Rather than record something unvalidated, reaching
  /// either poisons every open frame so no entry is emitted for them: an
  /// untracked construct must cost a cache miss, not a wrong hit.
  void recordTemplateNameQuery(const IdentifierInfo *Name,
                                TemplateNameKind Kind) override;
  void recordDeductionGuideNameQuery(const IdentifierInfo *Name,
                                      bool IsDeductionGuide) override;

  /// Passed on to OnDeclarationUse (CLANG_HEADER_CACHE_CLAIM=deps).
  void recordDeclarationUse(const NamedDecl *D, SourceLocation Loc) override {
    if (DumpSites)
      declarationSite(D, Loc);
    if (OnDeclarationUse)
      OnDeclarationUse(D, Loc);
  }
  std::function<void(const NamedDecl *, SourceLocation)> OnDeclarationUse;

  /// Closes every frame still open, emitting a record for each as if it had
  /// been exited. The header being populated is the main file of its own
  /// sub-compile, so it never receives an ExitFile and its frame would
  /// otherwise never be emitted at all. Sound to call only once the
  /// translation unit is over, since a frame's dependency set is complete
  /// exactly when nothing more can be added to it. Idempotent.
  void finalize();

  /// Records for headers whose region has closed, in closing order
  /// (innermost first). Frames still open -- notably the main source file --
  /// are not included; the main file is never a cache target. Poisoned
  /// frames are omitted entirely rather than emitted with partial data.
  llvm::ArrayRef<HeaderRecord> completed() const { return Completed; }

  /// The dependency set recorded for the closed header whose path ends with
  /// \p PathSuffix, or nullptr if there is no such record. Suffix matching
  /// keeps callers independent of how the file was spelled on the command
  /// line or resolved through the search path.
  const HeaderDependencySet *findByPath(llvm::StringRef PathSuffix) const;

  /// The dependency set recorded for the file entered as \p FID, once it has
  /// been left; nullptr if it was not tracked, is still open, or was poisoned.
  const HeaderDependencySet *recordFor(FileID FID) const;

  /// Track only the files \p Track accepts. Every file entered still joins
  /// the closure of every open frame, so a tracked header's record is the
  /// same; untracked files only get no record of their own. A compile that
  /// exports a few headers tracks just those, and pays almost nothing
  /// elsewhere: with no frame open, every hook returns at once.
  void trackOnly(std::function<bool(FileID)> Track) {
    TrackOnly = std::move(Track);
  }

  /// For measuring which headers inside a written one are independent of it
  /// (REPORT.md T15): per header, the files outside it whose state it read
  /// and whose state it changed. Kept only after measureIndependence().
  struct Independence {
    FileID FID;
    std::string Path;
    /// This file plus every real file entered beneath it.
    std::vector<FileID> Closure;
    /// The files outside the header whose #define or #undef gave a state the
    /// header read: a definedness test, a macro expanded, a name read as
    /// plain text after an #undef, a type name, a guarded #include skipped.
    /// Each with the first such read, for the log: "tests FOO".
    llvm::DenseMap<FileID, std::string> ReadFrom;
    /// The files outside the header whose #define or #undef the header
    /// replaced with another state, with the first: "undefines FOO". A
    /// redefinition with the same tokens changes nothing and is not counted.
    llvm::DenseMap<FileID, std::string> AltersFrom;
    bool Poisoned = false;
  };
  void measureIndependence() { Measuring = true; }

  /// CLANG_HEADER_CACHE_FOLD_SERVED: a saved copy stood in for the #include
  /// at \p Loc, and the headers open hold its declarations as their own.
  /// Its module file's source-location entries, \p NumIDs of them from ID
  /// \p FirstID, count as inside each, and its metadata \p Deps is theirs
  /// where it reads a state set outside them (the header boundary
  /// principle). \p Resolve gives where the declaration a name finds here
  /// is. A copy with submodules, warning pragmas or push_macro stacks
  /// poisons them instead.
  /// \p Linked: the copy is linked (CLANG_HEADER_CACHE_LINK), and replays
  /// its warning pragmas itself.
  void foldServed(SourceLocation Loc, int FirstID, unsigned NumIDs,
                  const MetadataView &Deps,
                  llvm::function_ref<SourceLocation(llvm::StringRef)> Resolve,
                  bool Linked = false);

  /// REPORT.md T17: what each file read inside a header being exported read
  /// with its own text -- a definedness test, a name expanded or read as
  /// plain text, a type name, a #pragma once #include skipped -- each with
  /// the file whose #define, #undef or declaration gave the state read
  /// (invalid for none: never defined, the command line). A name is
  /// recorded again in a file when its state there has changed since, so
  /// the dependencies of any part of the header, with some of the headers
  /// inside it held apart as submodules, can be put together afterwards.
  template <typename T> struct Sourced {
    T Dep;
    FileID Origin;
    uint64_t Seq = 0;
  };
  struct FileReads {
    FileID FID;
    std::string Path;
    /// When the file was entered and left, on the same clock as Seq.
    uint64_t Enter = 0, Exit = 0;
    std::vector<Sourced<ConditionalDependency>> Conditional;
    std::vector<Sourced<SyntacticDependency>> Syntactic;
    std::vector<Sourced<SymbolDependency>> Symbol;
    std::vector<Sourced<InclusionDependency>> Inclusion;
    /// Each macro the file's text defined or undefined, with the file whose
    /// directive the change replaced (invalid for none).
    struct Change {
      const IdentifierInfo *II;
      FileID Replaced;
      uint64_t Seq;
    };
    std::vector<Change> Changes;
    /// CLANG_HEADER_CACHE_PARTIAL: each #include the file's text reached
    /// that was skipped by the header's include guard: the header, by
    /// canonical path, its guard, the file whose #define gave the guard, how
    /// the #include spelled it ("<stdint.h>", "\"hash.h\""), and where.
    struct Skip {
      std::string Path;
      std::string Guard;
      FileID Origin;
      std::string Spelled;
      SourceLocation Loc;
      uint64_t Seq;
    };
    std::vector<Skip> Skips;
    /// The last state recorded per name, to record a name again when its
    /// state changes. Held apart so that the vector of FileReads grows by
    /// moving them: DenseMap's move may throw, and std::vector copies an
    /// element whose move may.
    struct SeenState {
      llvm::DenseMap<const IdentifierInfo *, const MacroInfo *> Macro;
      llvm::DenseMap<const IdentifierInfo *, std::pair<bool, FileID>> Test;
      llvm::DenseMap<const IdentifierInfo *, std::pair<unsigned, FileID>>
          Type;
      llvm::DenseSet<const FileEntry *> Once;
    };
    std::unique_ptr<SeenState> Seen = std::make_unique<SeenState>();
  };
  /// Keep a FileReads for each file read inside a header being exported:
  /// the files TrackOnly accepts and every file entered while one is open.
  void recordFiles() { RecordFiles = true; }

  /// CLANG_HEADER_CACHE_PIECES: the headers \p IsBase accepts are bases.
  /// Each dependency a base's frame records is marked, as it is recorded,
  /// with the piece it was read in -- a header the base's own lines include,
  /// with everything below it -- or the base's own lines; one read in two
  /// of them is the base's.
  void piecesFor(std::function<bool(FileID)> IsBase) {
    PiecesBase = std::move(IsBase);
  }
  /// What a base's frame recorded about its pieces. Each dependency of its
  /// record has its piece in Cond: 0 the base's own lines, K + 1 piece K.
  struct PiecesRecord {
    struct Piece {
      FileID FID;
      /// Read a state the base's lines or another piece set, changed one
      /// they set, or changed one they read before it: the base's.
      bool Bad = false;
    };
    std::vector<Piece> Pieces;
    /// The piece of each file entered under the base, -1 for the base's.
    llvm::DenseMap<FileID, int32_t> PieceOf;
    /// Per macro the base's lines or pieces changed, the group that changed
    /// it last (-1 the base's lines).
    llvm::DenseMap<const IdentifierInfo *, int32_t> ChangedBy;
    /// For each element of the record's Conditional, Syntactic, Symbol and
    /// Inclusion: the file whose directive or declaration gave the state
    /// read, invalid for none.
    std::vector<FileID> Origins[4];
    /// A state a piece set that the base's lines read: checked where the
    /// piece is skipped or another version of it is read.
    struct Crossing {
      int32_t Provider;
      std::optional<ConditionalDependency> Cond;
      std::optional<SyntacticDependency> Syn;
      std::optional<SymbolDependency> Sym;
    };
    std::vector<Crossing> Crossings;
    /// Each #include in the base's own lines that an include guard defined
    /// before the base skipped: a header the copy leaves out. Before: the
    /// record's Conditional, Syntactic, Symbol and Inclusion sizes there.
    /// Clean: the base's lines changed no macro before it but their guard.
    struct Skip {
      std::string Path, Guard, Spelled;
      FileID Origin;
      SourceLocation Loc;
      size_t Before[4];
      bool Clean;
    };
    std::vector<Skip> Skips;
  };
  const PiecesRecord *piecesRecordFor(FileID FID) const {
    for (size_t I = 0; I != CompletedFIDs.size(); ++I)
      if (CompletedFIDs[I] == FID)
        return CompletedPieces[I].get();
    return nullptr;
  }
  /// Every FileReads kept, in the order the files were entered.
  llvm::ArrayRef<FileReads> allFileReads() const { return Files; }
  /// The FileReads of the file entered as \p FID, or nullptr.
  const FileReads *fileReads(FileID FID) const {
    auto It = FileIndex.find(FID);
    return It == FileIndex.end() ? nullptr : &Files[It->second];
  }
  /// For the header entered as \p FID, once left: the file holding each of
  /// its record's #includes and __FILE__s, in order.
  struct Sites {
    std::vector<FileID> Includes, FileNames;
    /// CLANG_HEADER_CACHE_DUMP_DEPS, for measurement: where the header's
    /// text read each state its record holds, how it used it, and each
    /// #include it reached, in the order they happened (Seq).
    struct Read {
      /// 'c' conditional, 's' syntactic, 'y' symbol, 'i' inclusion, 'd' an
      /// outside declaration, 'n' an #include or __has_include.
      char Kind;
      /// "test", "guard", "value", "expand", "plain", "type", "decl",
      /// "once", "fold", "include".
      const char *Use;
      std::string Name;
      SourceLocation Loc;
      uint64_t Seq;
      /// The file whose directive or declaration gave the state read.
      FileID Origin;
    };
    std::vector<Read> Reads;
  };
  const Sites *sitesFor(FileID FID) const {
    for (size_t I = 0; I != CompletedFIDs.size(); ++I)
      if (CompletedFIDs[I] == FID)
        return &CompletedSites[I];
    return nullptr;
  }
  bool measuringIndependence() const { return Measuring; }
  /// What was measured for the header entered as \p FID, once left.
  const Independence *independenceFor(FileID FID) const;

  /// Notes changes to macros, for measureIndependence.
  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *MD) override;
  void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &MD,
                      const MacroDirective *Undef) override;

private:
  /// CLANG_HEADER_CACHE_PIECES: per name a base's lines or pieces read or
  /// changed, which group read it first (-1 the base's lines, or more than
  /// one group; -2 none) and which group changed it last (-3 none). Read for
  /// every identifier, so small.
  struct NameState {
    int16_t Cond = -2, Syn = -2, Sym = -2, Changer = -3;
  };
  struct PiecesState {
    PiecesRecord Rec;
    /// How deep the file being read is below the base's, and its piece.
    int32_t Depth = 0, Cur = -1;
    llvm::DenseMap<const IdentifierInfo *, NameState> Names;
    /// Per name, the dependency its first reading recorded, by kind
    /// (definedness, syntactic, symbol): made the base's when another group
    /// reads the name too.
    llvm::DenseMap<const IdentifierInfo *, std::array<int32_t, 3>> DepOf;
    int32_t &depOf(const IdentifierInfo *II, unsigned Kind) {
      auto [It, New] = DepOf.try_emplace(II);
      if (New)
        It->second.fill(-1);
      return It->second[Kind];
    }
    /// Per #pragma once file skipped, the group that first skipped it and
    /// its dependency.
    llvm::DenseMap<const FileEntry *, std::pair<int16_t, int32_t>> Skipped;
    /// The crossings recorded: name, piece, and the state read.
    llvm::DenseSet<std::tuple<const IdentifierInfo *, int32_t, uintptr_t>>
        Crossed;
    /// Macros the base's lines and pieces changed, and the first the base's
    /// own lines tested: its include guard, whose #define is not counted.
    unsigned Changes = 0;
    const IdentifierInfo *FirstRootTest = nullptr;
    /// A #pragma push_macro or pop_macro was read: no piece.
    bool NoPieces = false;
    /// A piece's group as it stands: a piece that turned out not to be one
    /// is the base's.
    int32_t eff(int32_t G) const {
      return G >= 0 && Rec.Pieces[G].Bad ? -1 : G;
    }
    void bad(int32_t K) {
      if (K < 0 || Rec.Pieces[K].Bad)
        return;
      Rec.Pieces[K].Bad = true;
      if (Cur == K)
        Cur = -1;
    }
  };

  /// One header's in-flight state, live between its EnterFile and ExitFile.
  struct Frame {
    FileID FID;
    std::string Path;
    /// This file plus every real file entered beneath it.
    llvm::DenseSet<FileID> Closure;
    /// The same files by identity, for a #pragma once skip, which names a
    /// file rather than a FileID.
    llvm::DenseSet<const FileEntry *> ClosureFiles;
    llvm::DenseSet<const IdentifierInfo *> SeenConditional;
    /// Names this frame has already classified, whatever the outcome. Also
    /// a subset relation: anything the innermost frame has seen, every frame
    /// enclosing it has seen too, since both were open at the time.
    llvm::DenseSet<const IdentifierInfo *> SeenSyntactic;
    llvm::DenseSet<const IdentifierInfo *> SeenSymbol;
    /// CLANG_HEADER_CACHE_DUMP_DEPS: outside declarations already used.
    llvm::DenseSet<const NamedDecl *> SeenDeclarationUse;
    /// CLANG_HEADER_CACHE_DUMP_DEPS, in a base: the kinds and names read.
    llvm::StringSet<> SiteNames;
    /// Per name, the #pragma push_macro made in this frame and not popped.
    llvm::DenseMap<const IdentifierInfo *, unsigned> MacroPushes;
    llvm::DenseSet<const FileEntry *> SeenSkipped;
    llvm::StringSet<> SeenFileNames;
    /// For each file entered under this frame, the index of the
    /// IncludeDependency that found it.
    llvm::DenseMap<FileID, int32_t> EntryOf;
    HeaderDependencySet Deps;
    /// For measureIndependence.
    llvm::DenseMap<FileID, std::string> ReadFrom, AltersFrom;
    /// For recordFiles: the file holding each #include and __FILE__ of Deps.
    Sites At;
    /// Something happened in this header that this tracker cannot record
    /// soundly, so it must not be cached at all. Checked at frame close.
    bool Poisoned = false;
    /// CLANG_HEADER_CACHE_PIECES, for a base.
    std::unique_ptr<PiecesState> P;
    /// Copies served inside this header and folded into it (foldServed):
    /// the first ID of each one's source-location entries, and how many.
    SmallVector<std::pair<int, unsigned>, 2> Folded;
    /// Whether \p FID is in this header: in its closure or in a copy folded
    /// into it.
    bool holds(FileID FID) const {
      if (Closure.contains(FID))
        return true;
      int ID = static_cast<int>(FID.getHashValue());
      for (auto [First, N] : Folded)
        if (ID >= First && ID < First + int(N))
          return true;
      return false;
    }
  };

  /// CLANG_HEADER_CACHE_PIECES: a base's frame reads \p II, changes it, or
  /// skips a header (see PiecesRecord).
  void piecesConditional(Frame &F, const IdentifierInfo *II, bool WasDefined,
                         FileID Origin);
  void piecesSyntactic(Frame &F, const IdentifierInfo *II, const MacroInfo *MI,
                       FileID DefFID, std::optional<std::string> &Sequence,
                       bool &Built);
  void piecesSymbol(Frame &F, const IdentifierInfo *Name,
                    SymbolClassification Classification, FileID QueryFID,
                    FileID ResolvedFID, bool HaveResolved,
                    llvm::StringRef ResolvedTypeName);
  void piecesChange(Frame &F, const IdentifierInfo *II);
  void piecesSkip(Frame &F, const FileEntryRef &SkippedFile,
                  const Token &FilenameTok, const IdentifierInfo *Guard,
                  FileID GuardOrigin);
  /// The state \p II has in \p F was set inside it: by the group reading
  /// it, by another piece (a crossing, which the base's lines read), or by
  /// the base's lines (which no piece may read).
  void piecesInternal(Frame &F, const IdentifierInfo *II,
                      PiecesRecord::Crossing C);

  /// Records a definedness test into every open frame it is external to.
  void recordConditional(const Token &MacroNameTok, const MacroDefinition &MD);
  /// The same, for \p II found defined or not, in a state set by a directive
  /// in \p Origin (see stateOrigin).
  void recordConditional(const IdentifierInfo *II, bool WasDefined,
                         FileID Origin);

  /// Records every held defined() test.
  void recordHeldTests();

  /// The file whose directive gave \p II the state just observed: the
  /// #define behind \p MD or, when \p II is not defined, the #undef that
  /// removed it. Invalid when no file set it -- a name never defined, a -D on
  /// the command line, a builtin -- which only the outside can change.
  FileID stateOrigin(const IdentifierInfo *II, const MacroDefinition &MD) const;

  /// Shared body of expansionCandidate and MacroExpands. \p MI is the name's
  /// active macro, or null if it is not one.
  void recordSyntactic(const IdentifierInfo *II, const MacroInfo *MI);

  /// Shared body of the two symbol-query hooks.
  void recordSymbol(const IdentifierInfo *Name,
                     SymbolClassification Classification,
                     const QueryContext &Ctx);

  /// Moves \p F's record into Completed unless it was poisoned, in which case
  /// it is dropped: emitting a partial record would produce an entry with
  /// fewer constraints than the compile it came from.
  void closeFrame(Frame &F);

  /// Marks every open frame uncacheable.
  void poisonOpenFrames();

  /// For measureIndependence: \p F read a state that \p Origin set, \p What
  /// naming the read.
  void noteRead(Frame &F, FileID Origin, const llvm::Twine &What);

  /// CLANG_HEADER_CACHE_DUMP_DEPS: \p F's text read \p Name at SiteLoc.
  void site(Frame &F, char Kind, const char *Use, llvm::StringRef Name,
            FileID Origin);
  /// The first use in each open frame of an outside declaration.
  void declarationSite(const NamedDecl *D, SourceLocation Loc);
  const bool DumpSites = std::getenv("CLANG_HEADER_CACHE_DUMP_DEPS");
  /// Where the read being recorded is, and, for a conditional, how it was
  /// used; set by each callback before it records.
  SourceLocation SiteLoc;
  const char *SiteUse = "test";
  /// In foldServed: what a served copy read, as read at its #include.
  bool Folding = false;

  /// Records the text of a __FILE__ (or, with \p LastComponentOnly,
  /// __FILE_NAME__) whose name token is at \p Loc.
  void recordFileName(SourceLocation Loc, bool LastComponentOnly);

  /// Appends one lookup to every open frame whose region holds the
  /// directive at \p Loc, and leaves the entries made in PendingEntries.
  void recordLookup(SourceLocation Loc, IncludeDependency::Kind K, bool Angled,
                    StringRef Spelled, OptionalFileEntryRef Found);

  Preprocessor &PP;
  llvm::SmallVector<Frame, 8> Stack;
  std::vector<HeaderRecord> Completed;
  /// The file each Completed record is for, by the same index.
  std::vector<FileID> CompletedFIDs;
  std::function<bool(FileID)> TrackOnly;
  bool Measuring = false;
  /// See recordFiles.
  bool RecordFiles = false;
  std::vector<FileReads> Files;
  llvm::DenseMap<FileID, size_t> FileIndex;
  /// The FileReads of the files open, innermost last.
  llvm::SmallVector<size_t, 8> OpenFiles;
  uint64_t Clock = 0;
  std::vector<Sites> CompletedSites;
  std::vector<std::unique_ptr<PiecesRecord>> CompletedPieces;
  std::function<bool(FileID)> PiecesBase;
  FileReads *currentFile() {
    return OpenFiles.empty() ? nullptr : &Files[OpenFiles.back()];
  }
  /// Records a read in the file holding it, if it is recorded and the state
  /// read came from another file. Returns whether it recorded one.
  void fileRead(FileReads *F, const IdentifierInfo *II, const MacroInfo *MI,
                FileID Origin);
  std::vector<Independence> Measured;
  /// Where each file was first entered, for a #pragma once skip.
  llvm::DenseMap<const FileEntry *, FileID> FirstEntered;
  /// The entries InclusionDirective just made, as (frame, index), waiting
  /// for the file they found to be entered.
  const FileEntry *PendingFile = nullptr;
  llvm::SmallVector<std::pair<size_t, int32_t>, 4> PendingEntries;
  /// Set by MacroExpands for __has_include_next, read by the HasInclude call
  /// that follows it.
  bool NextHasIncludeIsNext = false;
  /// The defined() tests of the expression being evaluated, waiting for
  /// DefinedOperatorsNeeded.
  struct HeldTest {
    /// The location of the 'defined' token, which names the test in
    /// DefinedOperatorsNeeded.
    SourceLocation Loc;
    const IdentifierInfo *II;
    bool WasDefined;
    FileID Origin;
  };
  llvm::SmallVector<HeldTest, 4> Held;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_ASTCACHEDEPENDENCYTRACKER_H
