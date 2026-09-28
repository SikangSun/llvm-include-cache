//===--- ExpansionCandidateObserver.h - Expansion candidates ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the ExpansionCandidateObserver interface, which the
//  Preprocessor notifies each time an identifier reaches the point where it
//  would be macro-expanded if it named a macro -- whether or not it does.
//
//  Those identifiers are exactly the ones whose meaning an earlier #define
//  could change. An identifier that is not a macro passes that point without
//  calling anything else in the Preprocessor, so no PPCallbacks event sees it;
//  this interface exists for callers that need the full set, such as a cache
//  of parsed headers deciding which names a stored parse assumed were not
//  macros.
//
//  It is a single registered object rather than a PPCallbacks method because
//  it fires once per identifier token. A compile normally has PPCallbacks
//  attached, so a new callback there would put a chain of virtual calls on
//  every identifier of every compile; an unset pointer costs one test.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_LEX_EXPANSIONCANDIDATEOBSERVER_H
#define LLVM_CLANG_LEX_EXPANSIONCANDIDATEOBSERVER_H

namespace clang {

class Token;

/// Receives every identifier that reaches the macro-expansion decision while
/// macro expansion is enabled.
class ExpansionCandidateObserver {
public:
  virtual ~ExpansionCandidateObserver();

  /// \p Identifier is about to be expanded if it names a macro, and passed
  /// through unchanged otherwise. It may come from a file or from a macro's
  /// replacement list, including the result of a ## paste. Identifiers read
  /// with expansion disabled -- a macro's own name in #define, the operand of
  /// #ifdef or defined(), the arguments of a function-like macro before they
  /// are substituted -- are not reported.
  virtual void expansionCandidate(const Token &Identifier) = 0;
};

} // namespace clang

#endif // LLVM_CLANG_LEX_EXPANSIONCANDIDATEOBSERVER_H
