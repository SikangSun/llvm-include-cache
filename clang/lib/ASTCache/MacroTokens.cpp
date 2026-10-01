//===- MacroTokens.cpp ------------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/MacroTokens.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"

using namespace clang;

std::optional<std::string> clang::ast_cache::serializeMacroTokens(
    const MacroInfo &MI, const SourceManager &SM, const LangOptions &LangOpts) {
  std::string Sequence;
  for (const Token &Tok : MI.tokens()) {
    unsigned Kind = Tok.getKind();
    Sequence.append(reinterpret_cast<const char *>(&Kind), sizeof(Kind));
    if (const IdentifierInfo *II = Tok.getIdentifierInfo()) {
      Sequence += II->getName();
    } else if (Tok.isLiteral()) {
      bool Invalid = false;
      std::string Text = Lexer::getSpelling(Tok, SM, LangOpts, &Invalid);
      if (Invalid)
        return std::nullopt;
      Sequence += Text;
    }
    Sequence.push_back('\0');
  }
  return Sequence;
}
