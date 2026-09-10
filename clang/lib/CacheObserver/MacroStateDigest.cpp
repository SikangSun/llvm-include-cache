//===- MacroStateDigest.cpp ------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CacheObserver/MacroStateDigest.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace clang::cache_observer;

const llvm::MD5::MD5Result &MacroStateDigest::snapshot() {
  if (!Dirty)
    return Cached;

  llvm::SmallVector<std::pair<StringRef, const MacroInfo *>, 64> ActiveMacros;
  for (Preprocessor::macro_iterator It = PP.macro_begin(), End = PP.macro_end();
       It != End; ++It) {
    const MacroDirective *MD = It->second.getLatest();
    if (!MD || !MD->isDefined())
      continue;
    ActiveMacros.emplace_back(It->first->getName(), MD->getMacroInfo());
  }

  llvm::sort(ActiveMacros,
             [](const std::pair<StringRef, const MacroInfo *> &A,
                const std::pair<StringRef, const MacroInfo *> &B) {
               return A.first < B.first;
             });

  llvm::MD5 Hasher;
  const uint8_t Sentinel = 0;
  for (const auto &NameAndInfo : ActiveMacros) {
    StringRef Name = NameAndInfo.first;
    const MacroInfo *MI = NameAndInfo.second;

    Hasher.update(Name);
    Hasher.update(llvm::ArrayRef<uint8_t>(&Sentinel, 1));

    for (const Token &Tok : MI->tokens()) {
      uint16_t Kind = static_cast<uint16_t>(Tok.getKind());
      Hasher.update(llvm::ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>(&Kind), sizeof(Kind)));
      if (const IdentifierInfo *TokII = Tok.getIdentifierInfo())
        Hasher.update(TokII->getName());
      else if (Tok.isLiteral() && Tok.getLiteralData())
        Hasher.update(StringRef(Tok.getLiteralData(), Tok.getLength()));
    }
    Hasher.update(llvm::ArrayRef<uint8_t>(&Sentinel, 1));
  }

  Hasher.final(Cached);
  Dirty = false;
  return Cached;
}
