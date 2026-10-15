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
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
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
  case MismatchReason::FileChanged:
    return "a file the part holds changed since the part was saved";
  case MismatchReason::WarningState:
    return "the warning flags or pragmas differ, so the header's warnings "
           "would too";
  case MismatchReason::MacroStack:
    return "a definition the header's #pragma pop_macro takes differs";
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
/// found in. Fills \p Found with what each found. Returns the first entry
/// that found a different file, if any.
std::optional<IncludeView> redoLookups(
    const MetadataView &Deps, Preprocessor &PP, const IncludeSite &Site,
    bool CheckNames,
    std::vector<std::pair<OptionalFileEntryRef, ConstSearchDirIterator>>
        &Found) {
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  FileManager &FM = PP.getFileManager();
  SourceManager &SM = PP.getSourceManager();
  for (const IncludeView &D : Deps.includes()) {
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
      return D;
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
      return D;
    if (CheckNames && File && File->getName() != D.Name)
      return D;
    Found.push_back({File, CurDir});
  }
  return std::nullopt;
}

} // namespace

MismatchReason clang::ast_cache::matchesLiveState(
    const MetadataView &Deps, Preprocessor &PP, Sema *S, std::string *Detail,
    const IncludeSite *Site, CheckCost *Cost, std::vector<NamedDecl *> *Found,
    SearchMemo *Memo) {
  // Names what failed, for a caller that reports it.
  auto Because = [&](MismatchReason R, const llvm::Twine &What) {
    if (Detail)
      *Detail = What.str();
    return R;
  };

  // All metadata carries pragma state, and only Sema holds the live value to
  // compare it against. Without Sema that check cannot run, and a check that
  // cannot run must reject rather than pass -- an unverifiable dependency is
  // the fail-open shape this whole design exists to avoid.
  if (!S)
    return Because(MismatchReason::PragmaState,
                   "no Sema to check pragma state");

  IdentifierTable &Idents = PP.getIdentifierTable();

  // Charges the time from one step's start to the next step's start, or to
  // the return.
  struct Laps {
    CheckCost *To;
    unsigned Step = CheckCost::Conditions;
    std::chrono::steady_clock::time_point Start =
        std::chrono::steady_clock::now();
    void next(CheckCost::Step S) {
      charge();
      Step = S;
    }
    void charge() {
      if (!To)
        return;
      auto Now = std::chrono::steady_clock::now();
      To->Nanos[Step] +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(Now - Start)
              .count();
      Start = Now;
    }
    ~Laps() { charge(); }
  } Lap{Cost};

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
  for (const ConditionalView &D : Deps.conditional()) {
    bool Defined = PP.isMacroDefined(Fresh(D.MacroName));
    if (Defined != D.WasDefined)
      return Because(MismatchReason::Conditional,
                     D.MacroName + (D.WasDefined ? " was defined, is not"
                                                 : " was not defined, is"));
  }

  Lap.next(CheckCost::Macros);

  // A name the stored parse read as plain text must still be plain text here.
  // isMacroDefined tests the identifier's macro bit first, which is clear for
  // nearly every name, and asks about module visibility only when it is set:
  // the bit alone is also set for a module's macro that is not visible.
  auto IsMacro = [&](llvm::StringRef Name) {
    return PP.isMacroDefined(Fresh(Name));
  };

  for (const ExpandsToView &D : Deps.expandsTo()) {
    const MacroInfo *MI = PP.getMacroDefinition(Fresh(D.Name)).getMacroInfo();
    std::optional<std::string> Live =
        MI ? serializeMacroTokens(*MI, PP.getSourceManager(), PP.getLangOpts())
           : std::nullopt;
    if (!Live || *Live != D.TokenSequence)
      return Because(MismatchReason::Syntactic,
                     D.Name + " expands differently");
  }

  // What the header's pops take from below it: the top of each name's
  // push_macro stack, as where the copy was written (REPORT.md Q11).
  for (const MacroStackView &Stack : Deps.macroStacks()) {
    const MacroStackEffect &E = Stack.Effect;
    if (E.Popped.empty())
      continue;
    const auto &Pushed = PP.getPushedMacros();
    auto It = Pushed.find(Fresh(E.Name));
    size_t Have = It == Pushed.end() ? 0 : It->second.size();
    if (Have < E.Popped.size())
      return Because(MismatchReason::MacroStack,
                     E.Name + " has fewer definitions pushed");
    for (size_t I = 0, N = E.Popped.size(); I != N; ++I) {
      const MacroInfo *MI = It->second[Have - 1 - I];
      std::optional<std::string> Live =
          MI ? serializeMacroTokens(*MI, PP.getSourceManager(),
                                    PP.getLangOpts())
             : std::nullopt;
      if (bool(MI) != E.Popped[I].has_value() || (MI && Live != E.Popped[I]))
        return Because(MismatchReason::MacroStack,
                       E.Name + "'s pushed definitions differ");
    }
  }

  // An #include the recorded compile skipped must also be skippable here,
  // which means the same file must already have been read.
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  for (const InclusionView &D : Deps.inclusion()) {
    OptionalFileEntryRef FE = PP.getFileManager().getOptionalFileRef(D.HeaderPath);
    if (!FE || (!HS.hasFileBeenImported(*FE) && !HS.getFileInfo(*FE).isImport))
      return Because(MismatchReason::Inclusion,
                     D.HeaderPath + " was already included, is not");
  }

  // Several hundred names for a system header, so after the short lists.
  std::optional<llvm::TimeTraceScope> NamesScope;
  NamesScope.emplace("HeaderCache Check NotAMacro");
  for (const NameView &N : Deps.notAMacro())
    if (IsMacro(N.Name))
      return Because(MismatchReason::Syntactic,
                     N.Name + " was not a macro, is");

  NamesScope.reset();
  Lap.next(CheckCost::TypeNamesAndPragmas);

  // Symbol dependencies last: the only ones that cost a name lookup.
  {
    for (const SymbolView &D : Deps.symbol()) {
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

  if (capturePragmaState(*S) != Deps.entry())
    return Because(MismatchReason::PragmaState, "#pragma pack or float state");

  // The copy holds the warnings its header printed under its compile's
  // flags and pragmas; under others it prints others (REPORT.md Q34).
  if (Site && Site->WarningState != Deps.warningState())
    return Because(MismatchReason::WarningState,
                   "the warning flags or pragmas differ");

  // The part holds the parse of each file's text when it was saved. A plain
  // compile reads the file again; a hit requires it unchanged, by size and
  // modification time, as Clang's own modules check their input files. Every
  // compile checks this itself.
  Lap.next(CheckCost::FileStamps);
  FileManager &FM = PP.getFileManager();
  SourceManager &SM = PP.getSourceManager();
  // The searches below start from the name this compile holds a file by when
  // it read the file before (refOnEntry). With the include path and the site,
  // those names make the searches' key (REPORT.md I9).
  std::string Key;
  if (Site)
    Key.append(reinterpret_cast<const char *>(&Site->SearchContext),
               sizeof(Site->SearchContext));
  for (const FileStampView &F : Deps.files()) {
    if (Cost)
      ++Cost->FilesCompared;
    OptionalFileEntryRef Live =
        FM.getOptionalFileRef(F.Path, /*OpenFile=*/false);
    if (!Live)
      return Because(MismatchReason::FileChanged, F.Path + " is gone");
    if (static_cast<uint64_t>(Live->getSize()) != F.Size ||
        static_cast<int64_t>(Live->getModificationTime()) != F.ModTime)
      return Because(MismatchReason::FileChanged,
                     F.Path + " changed since the part was saved");
    if (SM.hasFileInfo(&Live->getFileEntry())) {
      Key += F.Path;
      Key += '\0';
      Key += refOnEntry(SM, *Live).getName();
      Key += '\0';
    }
  }

  Lap.next(CheckCost::HeaderSearches);

  // The files the header's own #includes find depend on this compile's
  // include path, which nothing else compares. The stored parse also holds
  // the text each __FILE__ produced, which is the name the producer's compile
  // opened the file by.
  if (!Deps.includes().empty() || !Deps.fileNames().empty() ||
      !Deps.headerName().empty()) {
    if (!Site)
      return Because(MismatchReason::Include,
                     "no #include site to redo the header's lookups from");
    // A part exported from a real compile holds its files under the names
    // that compile opened them by, which __FILE__ and debug information
    // repeat; a copy the separate producer built holds its own, and only the
    // files themselves are compared.
    bool CheckNames = !Deps.headerName().empty();
    if (CheckNames && Site->File.getName() != Deps.headerName())
      return Because(MismatchReason::Include, "the header was opened as " +
                                                  Deps.headerName() + ", is " +
                                                  Site->File.getName());

    // Within a build the files in the include path do not appear or go away,
    // so searches under the same key find what an earlier compile's found.
    uint64_t SearchKey = llvm::xxh3_64bits(Key);
    std::optional<uint8_t> Known;
    if (Memo && Memo->Recall)
      Known = Memo->Recall(SearchKey);
    if (Known) {
      if (Cost)
        ++Cost->SearchesReused;
      if (*Known != static_cast<uint8_t>(MismatchReason::None))
        return Because(static_cast<MismatchReason>(*Known),
                       "an earlier compile of the build found the header's "
                       "searches to find other files");
    } else {
      auto Search = [&]() -> MismatchReason {
        std::vector<std::pair<OptionalFileEntryRef, ConstSearchDirIterator>>
            Found;
        std::optional<IncludeView> Bad =
            redoLookups(Deps, PP, *Site, CheckNames, Found);
        if (Cost)
          Cost->SearchesRedone += Found.size() + (Bad ? 1 : 0);
        if (Bad) {
          const IncludeView &D = *Bad;
          std::string Written = D.Angled ? "<" + D.Spelled.str() + ">"
                                         : "\"" + D.Spelled.str() + "\"";
          std::string Was = D.Found.empty() ? std::string("nothing")
                            : CheckNames    ? D.Name.str()
                                            : D.Found.str();
          return Because(MismatchReason::Include,
                         Written + " found " + Was + ", finds something else");
        }

        Lap.next(CheckCost::FileNames);
        for (const FileNameView &D : Deps.fileNames()) {
          OptionalFileEntryRef File = D.File < 0
                                          ? OptionalFileEntryRef(Site->File)
                                          : Found[D.File].first;
          if (!File)
            return Because(MismatchReason::FileName,
                           "__FILE__ is in a file this compile does not find");
          std::string Live =
              fileMacroText(refOnEntry(SM, *File).getName(),
                            D.Which == FileNameDependency::Macro::FileName,
                            PP.getLangOpts(), PP.getTargetInfo());
          if (Live != D.Text)
            return Because(MismatchReason::FileName, "__FILE__ was \"" +
                                                         D.Text + "\", is \"" +
                                                         Live + "\"");
        }
        return MismatchReason::None;
      };
      MismatchReason Searched = Search();
      if (Memo)
        Memo->Found = SearchOutcome{SearchKey, static_cast<uint8_t>(Searched)};
      if (Searched != MismatchReason::None)
        return Searched;
    }
  }

  Lap.next(CheckCost::Declarations);

  // The part's own declarations were compiled against the outside
  // declarations it refers to. Loading it resolves each name to this
  // compile's own declaration (or merges the part's copy with it), so the
  // name must find one with the same fingerprint: a struct fd_set with other
  // members, or an fscanf with another asm label, would compile differently.
  // The lookup is the one the reader resolves the names with.
  for (const DeclarationView &D : Deps.declarations()) {
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
    if (Found)
      Found->push_back(Live);
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
    llvm::StringRef Filename, Preprocessor &PP, Sema *S, MetadataView *Matched,
    llvm::function_ref<bool(llvm::StringRef Bytes, llvm::StringRef ModuleName)>
        Use,
    const IncludeSite *Site) {
  ++Counters.Lookups;

  llvm::SmallVector<char, 0> Msg;
  encodeLookupRequest({Filename.str(), Build, std::move(Learned)}, Msg);
  Learned.clear();
  auto Asked = std::chrono::steady_clock::now();
  std::optional<std::string> Reply;
  {
    llvm::TimeTraceScope Scope("HeaderCache Ask");
    if (!sendMessage(*Conn, Msg))
      return {};
    Reply = receiveMessage(*Conn);
  }
  Counters.DaemonMicros += microsSince(Asked);
  if (!Reply)
    return {};
  std::optional<LookupResponse> Resp = decodeLookupResponse(*Reply);
  if (!Resp)
    return {};

  LastCandidates = Resp->Entries.size();
  LastRejection = MismatchReason::None;
  MatchedDecls.clear();
  std::vector<NamedDecl *> Found;
  for (const CacheEntryRef &E : Resp->Entries) {
    // The metadata is read where the daemon stored it. If the offset
    // addresses nothing written, there is nothing to check the candidate
    // against, and it is not used.
    std::optional<MetadataView> Deps = MetadataView::open(
        Region.read(E.MetadataOffset, E.MetadataLength));
    if (!Deps)
      continue;
    std::string Detail;
    ++Counters.CandidatesChecked;
    auto Checking = std::chrono::steady_clock::now();
    MismatchReason Why;
    // What this compile, or an earlier one of its build, found the
    // candidate's searches to find under a key (REPORT.md I9).
    auto Recall = [&](uint64_t Key) -> std::optional<uint8_t> {
      auto Own = Outcomes.find({E.MetadataOffset, Key});
      if (Own != Outcomes.end())
        return Own->second;
      for (const SearchOutcome &O : E.Searches)
        if (O.Key == Key)
          return O.Outcome;
      return std::nullopt;
    };
    SearchMemo Memo{Recall, std::nullopt};
    {
      llvm::TimeTraceScope Scope("HeaderCache Check");
      Found.clear();
      Why = matchesLiveState(*Deps, PP, S, LogRejections ? &Detail : nullptr,
                             Site, &Counters.Check, &Found, &Memo);
    }
    if (Memo.Found) {
      Outcomes[{E.MetadataOffset, Memo.Found->Key}] = Memo.Found->Outcome;
      if (Build)
        Learned.push_back({Filename.str(), E.MetadataOffset, *Memo.Found});
    }
    Counters.CheckMicros += microsSince(Checking);
    if (Why != MismatchReason::None) {
      if (LastRejection == MismatchReason::None)
        LastRejection = Why;
      ++Counters.CandidatesRejected;
      ++Counters.RejectedFor[static_cast<unsigned>(Why)];
      if (LogRejections)
        Counters.Rejections.push_back(Filename.str() + ": " + Detail);
      continue;
    }
    llvm::StringRef Bytes = Region.read(E.Offset, E.Length);
    if (Bytes.empty())
      continue; // an offset that does not address written data
    // Before Use loads the candidate: loading may read the part's
    // references to outside declarations, which the caller pins to these.
    MatchedDecls = Found;
    if (Use && !Use(Bytes, Deps->moduleName()))
      continue;
    ++Counters.Hits;
    if (Matched)
      *Matched = *Deps;
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
  TimingReport WithSearches = Report;
  WithSearches.Build = Build;
  WithSearches.Learned = std::move(Learned);
  Learned.clear();
  encodeTimingReport(WithSearches, Msg);
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
