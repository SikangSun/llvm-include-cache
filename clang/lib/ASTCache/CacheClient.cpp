//===- CacheClient.cpp ------------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/DeclFingerprint.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "llvm/Support/raw_socket_stream.h"
#include <chrono>

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
  case MismatchReason::FileName:
    return "__FILE__ would name a file differently";
  case MismatchReason::Include:
    return "an #include inside the header finds a different file";
  case MismatchReason::Declaration:
    return "an outside declaration the part refers to differs or is missing";
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

/// \p File as this compile names it once entered. A file the compile has
/// read before keeps the name it was first read by, whatever this #include
/// calls it: the source manager keeps one record per file. That name is what
/// __FILE__ gives, and the directory a quoted #include inside it searches.
FileEntryRef refOnEntry(const SourceManager &SM, FileEntryRef File) {
  if (SM.hasFileInfo(&File.getFileEntry()))
    for (auto It = SM.fileinfo_begin(), E = SM.fileinfo_end(); It != E; ++It)
      if (&It->first.getFileEntry() == &File.getFileEntry() &&
          It->second->OrigEntry)
        return *It->second->OrigEntry;
  return File;
}

/// Redoes \p Deps' #include and __has_include lookups as this compile would
/// make them from \p Site, in order, the way Preprocessor::LookupFile makes
/// them: a quoted lookup also searches beside the file holding the
/// directive, and a _next lookup starts after the directory that file was
/// found in. Fills \p Found with what each found. Returns the index of the
/// first entry that found a different file, or -1.
int redoLookups(
    const HeaderDependencySet &Deps, Preprocessor &PP, const IncludeSite &Site,
    bool CheckNames,
    std::vector<std::pair<OptionalFileEntryRef, ConstSearchDirIterator>>
        &Found) {
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  FileManager &FM = PP.getFileManager();
  SourceManager &SM = PP.getSourceManager();
  for (size_t I = 0; I != Deps.Includes.size(); ++I) {
    const IncludeDependency &D = Deps.Includes[I];
    OptionalFileEntryRef Includer;
    ConstSearchDirIterator IncluderDir = nullptr;
    if (D.Includer < 0) {
      Includer = Site.File;
      IncluderDir = Site.Dir;
    } else {
      Includer = Found[D.Includer].first;
      IncluderDir = Found[D.Includer].second;
    }
    // __has_include found nothing for the file holding this directive, so
    // the directive cannot be in a file this compile reaches the same way.
    if (!Includer)
      return static_cast<int>(I);
    FileEntryRef Holder = refOnEntry(SM, *Includer);

    bool Next = D.K == IncludeDependency::Kind::IncludeNext ||
                D.K == IncludeDependency::Kind::HasIncludeNext;
    // A _next lookup in a file found without searching starts from the top,
    // as an ordinary lookup does (Preprocessor::getIncludeNextStart).
    ConstSearchDirIterator FromDir = nullptr;
    if (Next && IncluderDir) {
      FromDir = IncluderDir;
      ++FromDir;
    }
    SmallVector<std::pair<OptionalFileEntryRef, DirectoryEntryRef>, 1>
        Includers;
    if (!FromDir)
      Includers.push_back({Holder, Holder.getDir()});

    ConstSearchDirIterator CurDir = nullptr;
    OptionalFileEntryRef File = HS.LookupFile(
        D.Spelled, Site.Loc, D.Angled, FromDir, &CurDir, Includers,
        /*SearchPath=*/nullptr, /*RelativePath=*/nullptr,
        /*RequestingModule=*/nullptr, /*SuggestedModule=*/nullptr,
        /*IsMapped=*/nullptr, /*IsFrameworkFound=*/nullptr);
    std::string Path = File ? FM.getCanonicalName(*File).str() : "";
    if (Path != D.Found)
      return static_cast<int>(I);
    if (CheckNames && File && File->getName() != D.Name)
      return static_cast<int>(I);
    Found.push_back({File, CurDir});
  }
  return -1;
}

} // namespace

MismatchReason clang::ast_cache::matchesLiveState(
    const HeaderDependencySet &Deps, Preprocessor &PP, Sema *S,
    llvm::StringRef NotAMacroNames, std::string *Detail,
    const IncludeSite *Site) {
  // Names what failed, for a caller that reports it.
  auto Because = [&](MismatchReason R, const llvm::Twine &What) {
    if (Detail)
      *Detail = What.str();
    return R;
  };

  // Every record carries pragma state, and only Sema holds the live value to
  // compare it against. Without Sema that check cannot run, and a check that
  // cannot run must reject rather than pass -- an unverifiable dependency is
  // the fail-open shape this whole design exists to avoid.
  if (!S)
    return Because(MismatchReason::PragmaState,
                   "no Sema to check pragma state");

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
      return Because(MismatchReason::Conditional,
                     D.MacroName + (D.WasDefined ? " was defined, is not"
                                                 : " was not defined, is"));
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
      return Because(MismatchReason::Syntactic, "a dependency with no name");
    if (D.Required == SyntacticDependency::State::NotAMacro) {
      if (IsMacro(D.Name))
        return Because(MismatchReason::Syntactic,
                       D.Name + " was not a macro, is");
      continue;
    }
    const MacroInfo *MI = PP.getMacroDefinition(Fresh(D.Name)).getMacroInfo();
    std::optional<std::string> Live =
        MI ? serializeMacroTokens(*MI, PP.getSourceManager(), PP.getLangOpts())
           : std::nullopt;
    if (!Live || *Live != D.TokenSequence)
      return Because(MismatchReason::Syntactic,
                     D.Name + " expands differently");
  }

  // An #include the recorded compile skipped must also be skippable here,
  // which means the same file must already have been read.
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  for (const InclusionDependency &D : Deps.Inclusion) {
    OptionalFileEntryRef FE = PP.getFileManager().getOptionalFileRef(D.HeaderPath);
    if (!FE || (!HS.hasFileBeenImported(*FE) && !HS.getFileInfo(*FE).isImport))
      return Because(MismatchReason::Inclusion,
                     D.HeaderPath + " was already included, is not");
  }

  // Several hundred names for a system header, so after the short lists.
  if (!NotAMacroNames.empty()) {
    std::optional<llvm::SmallVector<llvm::StringRef, 0>> Names =
        splitNameList(NotAMacroNames);
    if (!Names)
      return Because(MismatchReason::Syntactic,
                     "the not-a-macro name list is damaged");
    for (llvm::StringRef Name : *Names)
      if (IsMacro(Name))
        return Because(MismatchReason::Syntactic,
                       Name + " was not a macro, is");
  }

  // Symbol dependencies last: the only ones that cost a name lookup.
  {
    for (const SymbolDependency &D : Deps.Symbol) {
      IdentifierInfo &II = Idents.get(D.Name);
      std::string LiveType;
      bool IsType = resolvesToType(*S, II, LiveType);

      bool WasType = D.Classification == SymbolClassification::Type;
      if (IsType != WasType)
        return Because(
            MismatchReason::Symbol,
            D.Name + (WasType ? " was a type, is not" : " was not a type, is"));
      // "Still a type" is not enough: two translation units can both answer
      // yes with different types, and the stored parse holds one of them.
      if (IsType && LiveType != D.ResolvedTypeName)
        return Because(MismatchReason::Symbol, D.Name + " was " +
                                                   D.ResolvedTypeName +
                                                   ", is " + LiveType);
    }
  }

  if (capturePragmaState(*S) != Deps.Entry)
    return Because(MismatchReason::PragmaState, "#pragma pack or float state");

  // The files the header's own #includes find depend on this compile's
  // include path, which nothing else compares. The stored parse also holds
  // the text each __FILE__ produced, which is the name the producer's compile
  // opened the file by.
  if (!Deps.Includes.empty() || !Deps.FileNames.empty() ||
      !Deps.HeaderName.empty()) {
    if (!Site)
      return Because(MismatchReason::Include,
                     "no #include site to redo the header's lookups from");
    // A part exported from a real compile holds its files under the names
    // that compile opened them by, which __FILE__ and debug information
    // repeat; a copy the separate producer built holds its own, and only the
    // files themselves are compared.
    bool CheckNames = !Deps.HeaderName.empty();
    if (CheckNames && Site->File.getName() != Deps.HeaderName)
      return Because(MismatchReason::Include, "the header was opened as " +
                                                  Deps.HeaderName + ", is " +
                                                  Site->File.getName());
    std::vector<std::pair<OptionalFileEntryRef, ConstSearchDirIterator>> Found;
    int Bad = redoLookups(Deps, PP, *Site, CheckNames, Found);
    if (Bad >= 0) {
      const IncludeDependency &D = Deps.Includes[Bad];
      std::string Written =
          D.Angled ? "<" + D.Spelled + ">" : "\"" + D.Spelled + "\"";
      std::string Was = D.Found.empty() ? std::string("nothing")
                        : CheckNames    ? D.Name
                                        : D.Found;
      return Because(MismatchReason::Include,
                     Written + " found " + Was + ", finds something else");
    }

    SourceManager &SM = PP.getSourceManager();
    for (const FileNameDependency &D : Deps.FileNames) {
      OptionalFileEntryRef File =
          D.File < 0 ? OptionalFileEntryRef(Site->File) : Found[D.File].first;
      if (!File)
        return Because(MismatchReason::FileName,
                       "__FILE__ is in a file this compile does not find");
      std::string Live =
          fileMacroText(refOnEntry(SM, *File).getName(),
                        D.Which == FileNameDependency::Macro::FileName,
                        PP.getLangOpts(), PP.getTargetInfo());
      if (Live != D.Text)
        return Because(MismatchReason::FileName,
                       "__FILE__ was \"" + D.Text + "\", is \"" + Live + "\"");
    }
  }

  // The part's own declarations were compiled against the outside
  // declarations it refers to. Loading it resolves each name to this
  // compile's own declaration (or merges the part's copy with it), so the
  // name must find one with the same fingerprint: a struct fd_set with other
  // members, or an fscanf with another asm label, would compile differently.
  // The lookup is the one the reader resolves the names with.
  for (const DeclarationDependency &D : Deps.Declarations) {
    NamedDecl *Live = ASTReader::lookupAtFileScope(
        *S, Idents.get(D.Name), D.In == DeclarationDependency::Space::Tag,
        /*CreateBuiltin=*/true);
    std::optional<std::string> F;
    if (Live)
      F = declarationFingerprint(*Live);
    if (!F || !fingerprintSatisfies(D.Fingerprint, *F))
      return Because(MismatchReason::Declaration,
                     D.Name + " was " + D.Fingerprint + ", is " +
                         (F ? *F : std::string("not declared")));
  }

  return MismatchReason::None;
}

namespace {
uint64_t microsSince(std::chrono::steady_clock::time_point Start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - Start)
      .count();
}
} // namespace

llvm::Expected<std::unique_ptr<CacheClient>>
CacheClient::connect(llvm::StringRef SocketPath, llvm::StringRef RegionPath) {
  auto Start = std::chrono::steady_clock::now();
  if (llvm::Error E = checkSocketPath(SocketPath))
    return std::move(E);
  llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> S =
      llvm::raw_socket_stream::createConnectedUnix(SocketPath);
  if (!S)
    return S.takeError();

  llvm::SmallVector<char, 0> Hello;
  encodeHello(Hello);
  std::optional<HelloReply> Reply;
  if (sendMessage(**S, Hello))
    if (std::optional<std::string> Body = receiveMessage(**S))
      Reply = decodeHelloReply(*Body);
  if (!Reply)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "the daemon did not say which region it "
                                   "writes");

  std::string Path = RegionPath.empty() ? Reply->RegionPath : RegionPath.str();
  llvm::Expected<SharedRegion> R = SharedRegion::attach(Path);
  if (!R)
    return R.takeError();
  if (R->instanceId() != Reply->InstanceId)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "region " + Path +
                                       " is not the one the daemon writes (" +
                                       Reply->RegionPath + ")");

  std::unique_ptr<CacheClient> C(new CacheClient());
  C->Conn = std::move(*S);
  C->Region = std::move(*R);
  C->Counters.ConnectMicros = microsSince(Start);
  return C;
}

CacheClient::~CacheClient() = default;

llvm::StringRef CacheClient::lookup(
    llvm::StringRef Filename, Preprocessor &PP, Sema *S,
    HeaderDependencySet *MatchedDeps,
    llvm::function_ref<bool(llvm::StringRef Bytes, llvm::StringRef ModuleName)>
        Use,
    const IncludeSite *Site) {
  ++Counters.Lookups;

  llvm::SmallVector<char, 0> Msg;
  encodeLookupRequest({Filename.str()}, Msg);
  auto Asked = std::chrono::steady_clock::now();
  if (!sendMessage(*Conn, Msg))
    return {};

  std::optional<std::string> Reply = receiveMessage(*Conn);
  Counters.DaemonMicros += microsSince(Asked);
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
    std::string Detail;
    auto Checking = std::chrono::steady_clock::now();
    MismatchReason Why = matchesLiveState(
        E.Deps, PP, S, Names, LogRejections ? &Detail : nullptr, Site);
    Counters.CheckMicros += microsSince(Checking);
    if (Why != MismatchReason::None) {
      ++Counters.CandidatesRejected;
      ++Counters.RejectedFor[static_cast<unsigned>(Why)];
      if (LogRejections)
        Counters.Rejections.push_back(Filename.str() + ": " + Detail);
      continue;
    }
    llvm::StringRef Bytes = Region.read(E.Offset, E.Length);
    if (Bytes.empty())
      continue; // an offset that does not address written data
    // A hit must leave the macro table as the header would have, so an entry
    // whose effects cannot be read is not used.
    std::optional<std::vector<MacroEffect>> Effects = decodeMacroEffects(
        E.EffectsLength ? Region.read(E.EffectsOffset, E.EffectsLength)
                        : llvm::StringRef());
    if (!Effects || (E.EffectsLength && Effects->empty()))
      continue;
    if (Use && !Use(Bytes, E.ModuleName))
      continue;
    ++Counters.Hits;
    if (MatchedDeps) {
      *MatchedDeps = E.Deps;
      MatchedDeps->MacroEffects = std::move(*Effects);
    }
    return Bytes;
  }
  return {};
}

bool CacheClient::isListed(llvm::StringRef Path) {
  uint64_t Version = 0;
  llvm::StringRef List = Region.askList(&Version);
  if (Version != AskListVersion) {
    AskList.clear();
    // A damaged list reads as empty: nothing is asked about, and every
    // header is parsed as it would be without a cache.
    if (std::optional<llvm::SmallVector<llvm::StringRef, 0>> Names =
            splitNameList(List))
      for (llvm::StringRef N : *Names)
        AskList.insert(N);
    AskListVersion = Version;
  }
  return AskList.contains(Path);
}

bool CacheClient::reportTimings(const TimingReport &Report) {
  auto Start = std::chrono::steady_clock::now();
  llvm::SmallVector<char, 0> Msg;
  encodeTimingReport(Report, Msg);
  bool Sent = sendMessage(*Conn, Msg) && receiveMessage(*Conn).has_value();
  Counters.ReportMicros += microsSince(Start);
  return Sent;
}

bool CacheClient::push(const PushRequest &Req) {
  llvm::SmallVector<char, 0> Msg;
  encodePushRequest(Req, Msg);
  if (!sendMessage(*Conn, Msg))
    return false;
  return receiveMessage(*Conn).has_value();
}
