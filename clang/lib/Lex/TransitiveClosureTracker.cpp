//===- TransitiveClosureTracker.cpp ----------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Lex/TransitiveClosureTracker.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

std::optional<FileEntryRef> TransitiveClosureTracker::observeFileChanged(
    SourceManager &SM, SourceLocation Loc, PPCallbacks::FileChangeReason Reason) {
  if (Reason != PPCallbacks::EnterFile)
    return std::nullopt;

  FileID FID = SM.getFileID(Loc);
  if (Seen.contains(FID))
    return std::nullopt;

  OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
  if (!FE)
    return std::nullopt;

  Seen.insert(FID);
  return *FE;
}

bool TransitiveClosureTracker::isInternal(SourceManager &SM,
                                           SourceLocation Loc) const {
  return Seen.contains(SM.getFileID(Loc));
}
