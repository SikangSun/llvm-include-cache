//===- CacheClient.cpp ------------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "llvm/Support/raw_socket_stream.h"

using namespace clang;
using namespace clang::ast_cache;

llvm::StringRef clang::ast_cache::describe(MismatchReason R) {
  switch (R) {
  case MismatchReason::None:
    return "matched";
  case MismatchReason::Conditional:
    return "a macro's definedness differs";
  case MismatchReason::Syntactic:
    return "an identifier is a macro here and was not, or expands differently";
  case MismatchReason::Symbol:
    return "an identifier classifies differently";
  case MismatchReason::Inclusion:
    return "a suppressed #include would not be suppressed here";
  case MismatchReason::PragmaState:
    return "pragma state at the #include differs";
  }
  return "unknown";
}

namespace {

/// Whether \p Name currently resolves to a type, and to which one. Mirrors
/// what Sema::getTypeName answers for the parser at a "T * p;" ambiguity --
/// the question the record captured -- without going through the parser.
bool resolvesToType(Sema &S, const IdentifierInfo &Name,
                    std::string &TypeNameOut) {
  LookupResult R(S, DeclarationName(&Name), SourceLocation(),
                 Sema::LookupOrdinaryName);
  S.LookupName(R, S.getCurScope());
  if (R.getResultKind() != LookupResultKind::Found)
    return false;

  NamedDecl *D = R.getFoundDecl();
  if (const auto *TD = dyn_cast<TypeDecl>(D)) {
    QualType QT = S.getASTContext().getTypeDeclType(TD);
    TypeNameOut = QT.getCanonicalType().getAsString();
    return true;
  }
  return false;
}

} // namespace

MismatchReason
clang::ast_cache::matchesLiveState(const HeaderDependencySet &Deps,
                                   Preprocessor &PP, Sema *S,
                                   llvm::StringRef NotAMacroNames) {
  // Every record carries pragma state, and only Sema holds the live value to
  // compare it against. Without Sema that check cannot run, and a check that
  // cannot run must reject rather than pass -- an unverifiable dependency is
  // the fail-open shape this whole design exists to avoid.
  if (!S)
    return MismatchReason::PragmaState;

  IdentifierTable &Idents = PP.getIdentifierTable();

  // The identifier for \p Name, brought up to date first. A hit earlier in
  // this compile loads a module, which can define a name the compile had
  // already seen; until the identifier is updated its macro bit still says
  // it is not a macro, and a check on it would accept an entry it must not.
  auto Fresh = [&](llvm::StringRef Name) {
    IdentifierInfo *II = PP.getIdentifierInfo(Name);
    PP.refreshIdentifier(*II);
    return II;
  };

  // Cheapest first: a definedness test is a hash lookup, and a header with
  // dozens of them usually fails on one.
  for (const ConditionalDependency &D : Deps.Conditional) {
    bool Defined = PP.isMacroDefined(Fresh(D.MacroName));
    if (Defined != D.WasDefined)
      return MismatchReason::Conditional;
  }

  // A name the stored parse read as plain text must still be plain text here.
  // isMacroDefined tests the identifier's macro bit first, which is clear for
  // nearly every name, and asks about module visibility only when it is set:
  // the bit alone is also set for a module's macro that is not visible.
  auto IsMacro = [&](llvm::StringRef Name) {
    return PP.isMacroDefined(Fresh(Name));
  };

  for (const SyntacticDependency &D : Deps.Syntactic) {
    if (D.Name.empty())
      return MismatchReason::Syntactic;
    if (D.Required == SyntacticDependency::State::NotAMacro) {
      if (IsMacro(D.Name))
        return MismatchReason::Syntactic;
      continue;
    }
    const MacroInfo *MI = PP.getMacroDefinition(Fresh(D.Name)).getMacroInfo();
    if (!MI || serializeMacroTokens(*MI) != D.TokenSequence)
      return MismatchReason::Syntactic;
  }

  // An #include the recorded compile skipped must also be skippable here,
  // which means the same file must already have been read.
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  for (const InclusionDependency &D : Deps.Inclusion) {
    OptionalFileEntryRef FE = PP.getFileManager().getOptionalFileRef(D.HeaderPath);
    if (!FE)
      return MismatchReason::Inclusion;
    if (!HS.hasFileBeenImported(*FE) &&
        !HS.getFileInfo(*FE).isImport)
      return MismatchReason::Inclusion;
  }

  // Several hundred names for a system header, so after the short lists.
  if (!NotAMacroNames.empty()) {
    std::optional<llvm::SmallVector<llvm::StringRef, 0>> Names =
        splitNameList(NotAMacroNames);
    if (!Names)
      return MismatchReason::Syntactic;
    for (llvm::StringRef Name : *Names)
      if (IsMacro(Name))
        return MismatchReason::Syntactic;
  }

  // Symbol dependencies last: the only ones that cost a name lookup.
  {
    for (const SymbolDependency &D : Deps.Symbol) {
      IdentifierInfo &II = Idents.get(D.Name);
      std::string LiveType;
      bool IsType = resolvesToType(*S, II, LiveType);

      bool WasType = D.Classification == SymbolClassification::Type;
      if (IsType != WasType)
        return MismatchReason::Symbol;
      // "Still a type" is not enough: two translation units can both answer
      // yes with different types, and the stored parse holds one of them.
      if (IsType && LiveType != D.ResolvedTypeName)
        return MismatchReason::Symbol;
    }
  }

  if (capturePragmaState(*S) != Deps.Entry)
    return MismatchReason::PragmaState;

  return MismatchReason::None;
}

llvm::Expected<std::unique_ptr<CacheClient>>
CacheClient::connect(llvm::StringRef SocketPath, llvm::StringRef RegionPath) {
  llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> S =
      llvm::raw_socket_stream::createConnectedUnix(SocketPath);
  if (!S)
    return S.takeError();

  llvm::Expected<SharedRegion> R = SharedRegion::attach(RegionPath);
  if (!R)
    return R.takeError();

  std::unique_ptr<CacheClient> C(new CacheClient());
  C->Conn = std::move(*S);
  C->Region = std::move(*R);
  return C;
}

CacheClient::~CacheClient() = default;

llvm::StringRef CacheClient::lookup(llvm::StringRef Filename, Preprocessor &PP,
                                    Sema *S, HeaderDependencySet *MatchedDeps) {
  ++Counters.Lookups;

  llvm::SmallVector<char, 0> Msg;
  encodeLookupRequest({Filename.str()}, Msg);
  if (!sendMessage(*Conn, Msg))
    return {};

  std::optional<std::string> Reply = receiveMessage(*Conn);
  if (!Reply)
    return {};
  std::optional<LookupResponse> Resp = decodeLookupResponse(*Reply);
  if (!Resp)
    return {};

  for (const CacheEntryRef &E : Resp->Entries) {
    // The names this entry requires not to be macros are in the region. If the
    // offset addresses nothing written, they cannot be checked, and checking
    // the rest without them would accept compiles it must not.
    llvm::StringRef Names;
    if (E.NamesLength) {
      Names = Region.read(E.NamesOffset, E.NamesLength);
      if (Names.empty()) {
        ++Counters.CandidatesRejected;
        ++Counters
              .RejectedFor[static_cast<unsigned>(MismatchReason::Syntactic)];
        continue;
      }
    }
    MismatchReason Why = matchesLiveState(E.Deps, PP, S, Names);
    if (Why != MismatchReason::None) {
      ++Counters.CandidatesRejected;
      ++Counters.RejectedFor[static_cast<unsigned>(Why)];
      continue;
    }
    llvm::StringRef Bytes = Region.read(E.Offset, E.Length);
    if (Bytes.empty())
      continue; // an offset that does not address written data
    ++Counters.Hits;
    if (MatchedDeps)
      *MatchedDeps = E.Deps;
    return Bytes;
  }
  return {};
}

bool CacheClient::push(const PushRequest &Req) {
  llvm::SmallVector<char, 0> Msg;
  encodePushRequest(Req, Msg);
  if (!sendMessage(*Conn, Msg))
    return false;
  return receiveMessage(*Conn).has_value();
}
