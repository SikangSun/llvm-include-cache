//===--- HeaderCacheHook.h - Serving an #include from a cache ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lets a compile serve an #include from a cache of already-parsed headers
// instead of entering the file.
//
// Deciding whether a stored parse fits this compile reads the live symbol
// table, and loading one goes through the module reader; both live above the
// Lex layer. So the preprocessor knows only this interface, and whatever
// installs an implementation comes from outside (clang/lib/ASTCache).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEX_HEADERCACHEHOOK_H
#define LLVM_CLANG_LEX_HEADERCACHEHOOK_H

#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/HeaderSearch.h"

namespace clang {

class Module;

class HeaderCacheHook {
public:
  virtual ~HeaderCacheHook();

  /// Called where the preprocessor is about to enter \p File, with
  /// \p IncludeLoc the location of the #include's filename token and
  /// \p CurDir the search directory \p File was found in (null when found
  /// without searching), from which an #include_next inside it would continue.
  ///
  /// Returning a module makes that module's declarations stand in for the
  /// file's contents, and the file is never entered. Returning null -- the
  /// ordinary answer -- enters the file as usual, so a cache that holds
  /// nothing suitable costs only the time it took to decide that.
  virtual Module *tryServe(FileEntryRef File, SourceLocation IncludeLoc,
                           ConstSearchDirIterator CurDir) = 0;

  /// Called where tryServe returned null: a file to enter in \p File's
  /// place, or an invalid FileID to enter \p File. A cache that can serve
  /// \p File once other headers are read first enters a file that includes
  /// them and then \p File again.
  virtual FileID enterInstead(FileEntryRef File, SourceLocation IncludeLoc) {
    return FileID();
  }
};

} // namespace clang

#endif // LLVM_CLANG_LEX_HEADERCACHEHOOK_H
