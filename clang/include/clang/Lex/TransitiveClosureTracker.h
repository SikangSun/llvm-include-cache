//===- TransitiveClosureTracker.h ------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEX_TRANSITIVECLOSURETRACKER_H
#define LLVM_CLANG_LEX_TRANSITIVECLOSURETRACKER_H

#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/PPCallbacks.h"
#include "llvm/ADT/DenseSet.h"
#include <optional>

namespace clang {
class SourceManager;

/// Tracks the set of real, on-disk files entered while a Preprocessor runs a
/// translation unit -- the transitive #include closure. Synthetic buffers
/// (the predefines/"<built-in>" buffer, -D/-include command-line buffers,
/// and similar) have no backing FileEntry and are never added: a Preprocessor
/// enters them the same way it enters an ordinary file, but they don't
/// represent a file on disk, so no meaningful cache-observation concept
/// (a per-file cache payload, "is this macro's definition inside the
/// header's own included text") should ever apply to them.
///
/// Feed this from a PPCallbacks::FileChanged override; query it with
/// contains()/isInternal().
class TransitiveClosureTracker {
public:
  /// Call once per FileChanged callback. If \p Reason is EnterFile, \p Loc
  /// names a real file (has a backing FileEntry), and that file has not
  /// been observed before, records it and returns its FileEntryRef.
  /// Otherwise returns std::nullopt and leaves the tracked set unchanged --
  /// covering a non-EnterFile reason, a synthetic buffer, and a repeat visit
  /// to an already-tracked file alike.
  std::optional<FileEntryRef>
  observeFileChanged(SourceManager &SM, SourceLocation Loc,
                      PPCallbacks::FileChangeReason Reason);

  /// Whether \p FID has been recorded via observeFileChanged.
  bool contains(FileID FID) const { return Seen.contains(FID); }

  /// Whether \p Loc's FileID has been recorded via observeFileChanged.
  bool isInternal(SourceManager &SM, SourceLocation Loc) const;

private:
  llvm::DenseSet<FileID> Seen;
};

} // namespace clang

#endif // LLVM_CLANG_LEX_TRANSITIVECLOSURETRACKER_H
