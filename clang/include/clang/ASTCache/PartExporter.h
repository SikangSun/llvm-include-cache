//===- PartExporter.h ------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Saves a header's own part of the real compile, at a miss (DECISIONS.md
// 2.17): the declarations and macros the header's text produced, the
// outside declarations those refer to, and the metadata a later hit checks.
// Replaces the separate producer compile.
//
// The part refers to an outside declaration by name, and the loading
// compile resolves the name to its own declaration. CLANG_HEADER_CACHE_OUTSIDE
// =copy writes a deep copy of each instead, which the loading compile merges
// with its own. A part with a declaration it cannot name, such as a field of
// a variable's unnamed struct, is written with copies too.
//
// The part is written at the first top-level declaration the parser finishes
// after the header ends, not when the header's file ends: the parser reads
// one token ahead, so the header's last declaration may still be open then.
// The notification comes from an AST consumer registered as a frontend
// plugin, which every compile gets and which does nothing unless this
// compile has an exporter.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_PARTEXPORTER_H
#define LLVM_CLANG_ASTCACHE_PARTEXPORTER_H

#include "clang/ASTCache/CacheProtocol.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileEntry.h"
#include <functional>
#include <string>
#include <vector>

namespace clang {
namespace serialization {
class ModuleFile;
}
class CompilerInstance;

namespace ast_cache {
class MetadataView;
class WarningPragmaTracker;

class PartExporter {
public:
  struct Stats {
    unsigned Exported = 0;
    unsigned Declined = 0;
    /// Exported parts written with deep copies, because some outside
    /// declaration could not be named, and why.
    unsigned Copied = 0;
    /// Headers held as submodules of the parts written (REPORT.md T17).
    unsigned Submodules = 0;
    /// Macros a submodule set that the part's other lines read, checked
    /// where the submodule is skipped.
    unsigned Crossing = 0;
    /// CLANG_HEADER_CACHE_LINK: links to copies served inside a header
    /// being written, and to copies of headers inside one written.
    unsigned LinksServed = 0, LinksWritten = 0;
    /// CLANG_HEADER_CACHE_SECOND_MISS: parts not written, the first time
    /// their copy would have been.
    unsigned HeldBack = 0;
    /// CLANG_HEADER_CACHE_FIRST_READ: headers recorded at their first read;
    /// of them, the ones written, and the ones read faster than the cutoff.
    unsigned FirstReads = 0, FirstReadsWritten = 0, FirstReadsFast = 0;
    /// CLANG_HEADER_CACHE_CLAIM: copies not written because another compile
    /// claimed or stored them.
    unsigned ClaimRefused = 0;
    std::vector<std::string> Copies;
    /// "<header>: <why>", one per declined part.
    std::vector<std::string> Declines;
    /// "<header> from <value>, <n> uses", one per part that expanded
    /// __COUNTER__: its value where the header began, and how often the
    /// header expanded it (DECISIONS.md 2.7).
    std::vector<std::string> Counters;
    /// "<header>: <bytes> bytes in <us> us, <n> by name", one per exported
    /// part.
    std::vector<std::string> Exports;
    /// With CLANG_HEADER_CACHE_INDEP=1: per part written, a line for each
    /// guarded header inside it saying whether it is independent of the
    /// part's lines before it (REPORT.md T15).
    std::vector<std::string> Independence;
    /// Wall time writing parts and pushing them to the daemon; of it, in
    /// Clang's writer and in compose (REPORT.md T17).
    uint64_t Micros = 0;
    uint64_t WriterMicros = 0;
    uint64_t ComposeMicros = 0;
    /// Of compose: choosing the submodules, tagging the dependencies.
    uint64_t ChooseMicros = 0, DepsMicros = 0;
  };

  /// Installs an exporter on \p CI's preprocessor, which owns it. \p Deliver
  /// receives each part, ready to push. \p Pragmas, when given, says where a
  /// warning pragma is in force; without it no part holds diagnostics.
  static PartExporter &install(CompilerInstance &CI,
                               std::function<void(PushRequest &&)> Deliver,
                               const WarningPragmaTracker *Pragmas = nullptr);

  virtual ~PartExporter();

  /// The compile is about to enter \p File at an #include where a saved copy
  /// could have stood in, and none fit: export the header. \p StoredAs, when
  /// given, is the name the daemon stores it under instead of its path: a
  /// version of a section of another header's copy (CLANG_HEADER_CACHE_PIECES).
  virtual void exportNext(FileEntryRef File, llvm::StringRef StoredAs = {}) = 0;

  /// The compile is about to enter \p File, a header no compile has listed
  /// yet (CLANG_HEADER_CACHE_FIRST_READ): record it as exportNext does, and
  /// write its copy only if reading it took at least \p CutoffMicros, the
  /// time that lists a header. The headers inside it are asked for as
  /// usual; one served inside it declines it, as it does any part.
  virtual void exportIfSlow(FileEntryRef File, uint64_t CutoffMicros) = 0;

  /// A saved copy stood in for an #include. A header open now is not
  /// exported: its part would hold the import, not the declarations. The
  /// copy may be one the compile loaded before, which loads no module file.
  /// With \p Fold, the copy's module file, the headers open hold its
  /// declarations as their own instead (CLANG_HEADER_CACHE_FOLD_SERVED), and
  /// its metadata \p Deps joins theirs; \p Loc is the #include, and
  /// \p Resolve gives where the declaration a name finds there is.
  virtual void
  served(serialization::ModuleFile *Fold = nullptr,
         const MetadataView *Deps = nullptr, SourceLocation Loc = {},
         llvm::function_ref<SourceLocation(llvm::StringRef)> Resolve =
             nullptr) = 0;

  /// CLANG_HEADER_CACHE_LINK: the copy of the header at \p Path, module
  /// \p Module and file \p MF, stood in for an #include at \p Loc inside
  /// the headers open, whose copies link it instead of holding it. \p Guard
  /// is the header's include guard. Its metadata \p Deps joins theirs, and
  /// \p Resolve gives where the declaration a name finds there is.
  /// \p FirstModule: how many module files the compile had loaded before
  /// the copy and its own links loaded.
  virtual void
  linked(llvm::StringRef Path, llvm::StringRef Module, llvm::StringRef Guard,
         serialization::ModuleFile *MF, const MetadataView &Deps,
         SourceLocation Loc,
         llvm::function_ref<SourceLocation(llvm::StringRef)> Resolve,
         unsigned FirstModule, uint64_t Hash) = 0;

  /// CLANG_HEADER_CACHE_SECOND_MISS: a part is written only if \p Seen
  /// says a compile was about to write the same copy (header, module name)
  /// before.
  virtual void
  writeOnlyIfSeen(std::function<bool(llvm::StringRef, llvm::StringRef)> Seen) = 0;

  /// CLANG_HEADER_CACHE_CLAIM: before Clang's writer runs, \p Claim (header,
  /// module name) says whether this compile may write the copy; a claimed
  /// copy then declined is given back with \p Release. With \p Check, a
  /// refused copy is written anyway and handed to \p Compare instead of
  /// being sent.
  virtual void
  writeOnlyIfClaimed(std::function<bool(llvm::StringRef, llvm::StringRef)> Claim,
                     std::function<void(llvm::StringRef, llvm::StringRef)> Release,
                     std::function<void(const PushRequest &)> Compare) = 0;

  /// Called with the stored name of each header whose copy is declined.
  virtual void whenDeclined(std::function<void(llvm::StringRef)> F) = 0;

  /// Called once the compile's last copy is written.
  virtual void whenFinished(std::function<void()> F) = 0;

  /// While \p On, Clang's diagnostics are a hit's, printed again: not the
  /// open headers' to hold.
  virtual void replaying(bool On) = 0;

  /// Whether a header this compile is exporting is open: one that missed,
  /// was entered, and has not been declined. Its part is a folded copy -- it
  /// holds every header inside it -- so the headers inside must be read as
  /// text, not served. A first read (exportIfSlow) does not count: the
  /// headers inside it are asked for.
  virtual bool writing() const = 0;

  /// Whether a header this compile is exporting is open and not declined, a
  /// first read included: a header inside it belongs to its copy.
  virtual bool recording() const = 0;

  /// Export the header whose absolute path is \p Path the first time it is
  /// entered, whatever the cache says. For CLANG_HEADER_CACHE_EXPORT.
  virtual void exportWhenEntered(std::string Path) = 0;

  /// Clang printed a diagnostic. One printed while a header being exported
  /// is open goes into its part, to be printed again at a hit.
  virtual void diagnostic(DiagnosticsEngine::Level Level,
                          const Diagnostic &Info) = 0;

  virtual const Stats &stats() const = 0;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_PARTEXPORTER_H
