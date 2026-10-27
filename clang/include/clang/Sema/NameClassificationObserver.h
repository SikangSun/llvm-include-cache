//===--- NameClassificationObserver.h - Symbol-dependency hook -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the NameClassificationObserver interface, which lets
//  a caller observe every point where the parser consults Sema to resolve
//  C/C++'s context-sensitive grammar ambiguities -- is "T" in "T * p;" a
//  type-name, is "a" in "a<b>" a template-name, and so on.
//
//  This exists for the AST-cache tier's symbol-dependency tracking: a
//  cached AST's soundness can depend on an identifier's classification at
//  population time still holding at splice time, which is a different
//  concern from macro substitution and needs its own tracking mechanism.
//  Unlike PPCallbacks::MacroExpands, there is no pre-existing external
//  hook for this in Sema -- ExternalSemaSource's lookup hooks only fire on
//  lookup *failure*. This interface, and Sema::setNameClassificationObserver,
//  are what stand in for one.
//
//  Per clang/docs/InternalsManual.rst's Annotation Tokens section, the
//  parser routes every identifier/"::" it must disambiguate through a small,
//  closed set of Sema entry points. Each gets a hook below, except:
//
//   - Sema::isCurrentClassName -- its result is "&II == the currently-open
//     class's identifier", and the open class comes from the header's own
//     text, so it cannot vary independently of inputs already tracked (a
//     macro-derived class name is recorded as a Value dependency).
//   - Sema::isTagName, Sema::DiagnoseUnknownTypeName -- error-recovery and
//     diagnostic paths, reached only once ordinary classification has already
//     failed. A population run that reaches either should be excluded from
//     caching rather than have its recovery decisions tracked.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_SEMA_NAMECLASSIFICATIONOBSERVER_H
#define LLVM_CLANG_SEMA_NAMECLASSIFICATIONOBSERVER_H

#include "clang/Sema/Sema.h"

namespace clang {

/// Callback handler that receives the result of every grammar-ambiguity
/// resolution the parser asks Sema to perform.
class NameClassificationObserver {
public:
  virtual ~NameClassificationObserver();

  /// Where a disambiguation query was asked, and what answered it.
  ///
  /// \c QueryLoc attributes the query to the header being parsed, so a
  /// per-header consumer can file it against the right one. \c ResolvedLoc
  /// is where the declaration that produced the answer is written, which is
  /// what decides whether the answer can vary at all: a declaration inside
  /// the header's own transitive-include closure is pinned by the header's
  /// own text, exactly as for a macro definition. It is invalid when the
  /// name resolved to nothing -- which is itself an answer that can vary,
  /// since a translation unit that does declare the name parses differently.
  struct QueryContext {
    SourceLocation QueryLoc;
    SourceLocation ResolvedLoc;
    /// Canonical spelling of the resolved type, for answers that name one.
    /// Checking only "is a type" is not enough for a cache that stores the
    /// resolved Decl graph: two translation units can both answer "yes" with
    /// different types, and the stored AST holds whichever one populated it.
    llvm::StringRef ResolvedTypeName;
  };

  /// Called once per Sema::ClassifyName call, with the (possibly
  /// typo-corrected) identifier that was classified and the result. This is
  /// the richer classification the parser needs where a statement or
  /// expression begins with a bare identifier -- in C, the "T * p;"
  /// declaration-versus-multiplication case.
  virtual void recordClassification(const IdentifierInfo *Name,
                                     const Sema::NameClassification &Result,
                                     const QueryContext &Ctx) = 0;

  /// Called once per Sema::getTypeName call: does \p Name currently resolve
  /// to a type in this scope. This is the dominant disambiguation funnel --
  /// declarators, cast-vs-parenthesized-expression, tentative parsing.
  virtual void recordTypeNameQuery(const IdentifierInfo *Name, bool IsType,
                                    const QueryContext &Ctx) = 0;

  /// Called once per Sema::isTemplateName call: does \p Name currently
  /// resolve to a template, and of what kind. Resolves "a<b>" as a
  /// template-argument list versus a less-than comparison.
  virtual void recordTemplateNameQuery(const IdentifierInfo *Name,
                                        TemplateNameKind Kind) = 0;

  /// Called once per Sema::isDeductionGuideName call: does \p Name currently
  /// resolve to a type template, making this a C++17 deduction-guide
  /// declaration.
  virtual void recordDeductionGuideNameQuery(const IdentifierInfo *Name,
                                              bool IsDeductionGuide) = 0;

  /// Called where a name, in an expression, a type or a tag reference,
  /// resolved to \p D: the declarations a header's text refers to.
  virtual void recordDeclarationUse(const NamedDecl *D, SourceLocation Loc) {}
};

}

#endif // LLVM_CLANG_SEMA_NAMECLASSIFICATIONOBSERVER_H
