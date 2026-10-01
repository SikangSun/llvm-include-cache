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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace clang {
class IdentifierInfo;
class MacroInfo;
class Preprocessor;

namespace ast_cache {

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
  void Defined(const Token &MacroNameTok, const MacroDefinition &MD,
               SourceRange Range) override;

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

  /// The same record, reached from an actual expansion. Every expansion is
  /// preceded by an expansionCandidate for the same token, so this only
  /// matters if some path reaches HandleIdentifier without one -- in which
  /// case recording here keeps the dependency rather than dropping it.
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

private:
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
    llvm::DenseSet<const FileEntry *> SeenSkipped;
    HeaderDependencySet Deps;
    /// Something happened in this header that this tracker cannot record
    /// soundly, so it must not be cached at all. Checked at frame close.
    bool Poisoned = false;
  };

  /// Records a definedness test into every open frame it is external to.
  void recordConditional(const Token &MacroNameTok, const MacroDefinition &MD);

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

  Preprocessor &PP;
  llvm::SmallVector<Frame, 8> Stack;
  std::vector<HeaderRecord> Completed;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_ASTCACHEDEPENDENCYTRACKER_H
