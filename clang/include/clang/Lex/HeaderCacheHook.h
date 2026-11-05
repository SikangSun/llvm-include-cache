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

  /// Fragments, for Bothner's compile server
  /// (clang/lib/ASTCache/COMPILE_SERVER_DESIGN.md 2.4): the text of a file
  /// between two directives, or between a directive and the file's start or
  /// end. A hook that sets this hears where every fragment of a file-backed
  /// buffer ends and starts. The header cache leaves it unset, so its
  /// compiles pay one test per directive.
  bool WantsFragments = false;

  /// How a stored fragment stands in for its text, when fragmentStart
  /// reuses it.
  struct FragmentSkip {
    /// Where lexing goes on: the '#' of the directive that ends the
    /// fragment, at the start of its line, or the end of the file.
    unsigned End = 0;
    /// Whether the fragment handed the parser a token and expanded a macro.
    /// The multiple-include optimization must hear both, or tokens after an
    /// include guard's #endif would go unseen and the header be taken as
    /// guarded.
    bool HadTokens = false;
    bool ExpandedMacro = false;
    /// The module whose visibility restores the fragment's declarations,
    /// carried to the parser by an annot_module_include token, or null.
    Module *Restore = nullptr;
  };

  /// The fragment of \p FID that ran up to \p Offset ends there: at the '#'
  /// of a directive, or at the end of the file. \p InMacroArgs: the
  /// directive sits inside a function-like macro's arguments, so skipping
  /// the fragment would skip the macro's name.
  virtual void fragmentEnd(FileID FID, unsigned Offset, bool InMacroArgs) {}

  /// A fragment of \p FID starts at \p Offset, before its first token is
  /// lexed: after a directive, or where an included file returned. With
  /// \p CanSkip false the preprocessor cannot skip it (a token stream is
  /// pending on top of the file, or macro arguments are being collected).
  /// Returning true, where \p CanSkip, skips the text up to \p Skip.End.
  virtual bool fragmentStart(FileID FID, unsigned Offset, bool CanSkip,
                             FragmentSkip &Skip) {
    return false;
  }
};

} // namespace clang

#endif // LLVM_CLANG_LEX_HEADERCACHEHOOK_H
