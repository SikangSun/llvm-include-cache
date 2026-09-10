//===--- DeclBoundaryObserver.h - Declaration-boundary queries -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the DeclBoundaryObserver interface, which lets the
//  Preprocessor ask whatever Parser is currently driving it whether the
//  parse is positioned at a clean declaration-sequence boundary -- not
//  inside a function body, an initializer list, an enum/class body, or
//  between the comma-separated declarators of a single declaration.
//
//  This exists for callers deciding whether it is safe to splice a
//  pre-built declaration graph in place of lexing and parsing an
//  #include'd file textually: doing so is only sound when the #include
//  site is at such a boundary, since the Parser has no other notion of
//  file boundaries and would otherwise happily splice tokens from a new
//  file into the middle of an in-progress construct.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_LEX_DECLBOUNDARYOBSERVER_H
#define LLVM_CLANG_LEX_DECLBOUNDARYOBSERVER_H

namespace clang {

/// Callback handler that lets the Preprocessor query the driving Parser's
/// current nesting state.
class DeclBoundaryObserver {
public:
  virtual ~DeclBoundaryObserver();

  /// Returns true if the parse is not currently inside any unclosed '{',
  /// '(', or '[', and not positioned between the declarators of a single,
  /// still-open declaration.
  virtual bool isAtDeclarationBoundary() const = 0;
};

}

#endif // LLVM_CLANG_LEX_DECLBOUNDARYOBSERVER_H
