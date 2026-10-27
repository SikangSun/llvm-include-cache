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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_socket_stream.h"
#include <algorithm>
#include <chrono>
#include <unistd.h>

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
  case MismatchReason::GuardWithoutHeader:
    return "a header's guard is defined, but the header was never read";
  case MismatchReason::Counter:
    return "its header expanded __COUNTER__, which starts at another value "
           "here (or, measuring, is never served)";
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
/// found in. \p Includes are the metadata's #includes from index \p At on;
/// what each finds goes in \p Found at its index. Returns the first entry
/// that found a different file, if any.
std::optional<IncludeView> redoLookups(
    MetadataSection<IncludeView> Includes, size_t At, Preprocessor &PP,
    const IncludeSite &Site, bool CheckNames,
    std::vector<std::pair<OptionalFileEntryRef, ConstSearchDirIterator>>
        &Found,
    llvm::function_ref<bool(uint32_t)> Apply) {
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  FileManager &FM = PP.getFileManager();
  SourceManager &SM = PP.getSourceManager();
  for (const IncludeView &D : Includes) {
    size_t Index = At++;
    // In a submodule that does not load: nothing to find.
    if (!Apply(D.Cond)) {
      Found[Index] = {std::nullopt, nullptr};
      continue;
    }
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
    Found[Index] = {File, CurDir};
  }
  return std::nullopt;
}

} // namespace

MismatchReason clang::ast_cache::matchesLiveState(
    const MetadataView &Deps, Preprocessor &PP, Sema *S, std::string *Detail,
    const IncludeSite *Site, CheckCost *Cost, std::vector<NamedDecl *> *Found,
    SearchMemo *Memo, bool SkipConditionals, std::vector<bool> *LoadedOut,
    const std::vector<bool> *Plugged, SectionFailures *Sections,
    std::vector<std::string> *Definedness, bool StateOnly,
    const std::vector<std::pair<const NamedDecl *, const Decl *>> *Prior) {
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

  // The copy holds the warnings its header printed under its compile's
  // flags and pragmas; under others it prints others (REPORT.md Q34). One
  // integer, so first (I15).
  if (Site && Site->WarningState != Deps.warningState())
    return Because(MismatchReason::WarningState,
                   "the warning flags or pragmas differ");

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

  // Which submodules load (REPORT.md T17): each whose header the compile
  // has not read, inside one that loads. An element applies where its own
  // submodule loads and, if it takes something from another submodule,
  // where that one is skipped -- then the compile's own header gives it.
  std::vector<bool> Loaded;
  auto WasRead = [&](llvm::StringRef Path) {
    HeaderSearch &HS = PP.getHeaderSearchInfo();
    OptionalFileEntryRef FE = PP.getFileManager().getOptionalFileRef(Path);
    return FE && (PP.alreadyIncluded(*FE) || HS.hasFileBeenImported(*FE));
  };
  for (const SubmoduleView &M : Deps.submodules()) {
    // Inside a skipped one: skipped too.
    if (M.Parent >= 0 && !Loaded[M.Parent]) {
      Loaded.push_back(false);
      continue;
    }
    bool Read;
    if (!M.Guard.empty()) {
      Read = PP.mayBeMacro(M.Guard) && PP.isMacroDefined(Fresh(M.Guard));
      // Defined by hand, not by reading the header: plain clang leaves the
      // header out, and the rest of the copy was made with it read.
      if (Read && !WasRead(M.Path))
        return Because(MismatchReason::GuardWithoutHeader,
                       M.Guard + " is defined, " + M.Path + " was not read");
    } else {
      Read = WasRead(M.Path);
    }
    Loaded.push_back(!Read);
  }
  std::vector<bool> Applies{true};
  std::vector<int32_t> OwnerOf{-1};
  for (const ConditionView &C : Deps.conditions()) {
    Applies.push_back((C.Owner < 0 || Loaded[C.Owner]) &&
                      (C.Provider < 0 || !Loaded[C.Provider]) &&
                      !(C.LeftOut >= 0 && Plugged &&
                        C.LeftOut < static_cast<int32_t>(Plugged->size()) &&
                        (*Plugged)[C.LeftOut]));
    OwnerOf.push_back(C.Owner);
  }
  if (Sections) {
    Sections->Failing.assign(Loaded.size(), false);
    Sections->First = MismatchReason::None;
    Sections->Detail.clear();
  }
  // A section found failing is checked no further.
  auto Apply = [&](uint32_t Cond) {
    return Applies[Cond] && !(Sections && OwnerOf[Cond] >= 0 &&
                              Sections->Failing[OwnerOf[Cond]]);
  };
  // CLANG_HEADER_CACHE_PIECES: what only a section's files read failing
  // marks the section, which another version can replace.
  auto InSection = [&](uint32_t Cond, MismatchReason R,
                       const llvm::Twine &What) {
    if (!Sections || OwnerOf[Cond] < 0)
      return false;
    Sections->Failing[OwnerOf[Cond]] = true;
    if (Sections->First == MismatchReason::None) {
      Sections->First = R;
      if (Detail)
        Sections->Detail = What.str();
    }
    return true;
  };
  auto AnySectionFails = [&] {
    return Sections && llvm::is_contained(Sections->Failing, true);
  };
  if (LoadedOut)
    *LoadedOut = Loaded;

  // CLANG_HEADER_CACHE_PIECES: a grouped copy (HeaderDependencySet::Grouped)
  // is read group by group -- the base's first, then the group of each
  // section that loads and has not failed -- and the group of a section
  // that does not load is passed over whole. An ungrouped copy is read
  // whole, as one group.
  const uint32_t NG = Deps.groups();
  const uint32_t Groups = NG ? NG : 1;
  auto Live = [&](uint32_t G) {
    return G == 0 ||
           (Loaded[G - 1] && !(Sections && Sections->Failing[G - 1]));
  };
  auto Slice = [&](auto Whole, MetadataView::Section Sec, uint32_t G) {
    using T = typename decltype(Whole)::value_type;
    return NG ? Deps.template group<T>(Sec, G) : Whole;
  };
  using Result = std::optional<MismatchReason>;
  HeaderSearch &HS = PP.getHeaderSearchInfo();
  FileManager &FM = PP.getFileManager();
  SourceManager &SM = PP.getSourceManager();

  // Cheapest first: a definedness test is a hash lookup, and a header with
  // dozens of them usually fails on one.
  auto Conditionals = [&](MetadataSection<ConditionalView> Sec) -> Result {
    for (const ConditionalView &D : Sec) {
      if (!Apply(D.Cond))
        continue;
      if (SkipConditionals)
        break;
      if (Cost)
        ++Cost->Read[MetadataView::Conditional];
      // A name never made a macro is not one, without looking it up (I14).
      bool Defined =
          PP.mayBeMacro(D.MacroName) && PP.isMacroDefined(Fresh(D.MacroName));
      if (Defined != D.WasDefined) {
        std::string What =
            D.MacroName.str() +
            (D.WasDefined ? " was defined, is not" : " was not defined, is");
        if (InSection(D.Cond, MismatchReason::Conditional, What))
          continue;
        if (Definedness) {
          Definedness->push_back(D.MacroName.str() +
                                 (D.WasDefined ? "-" : "+"));
          continue;
        }
        return Because(MismatchReason::Conditional, What);
      }
    }
    return std::nullopt;
  };

  // A name the stored parse read as plain text must still be plain text here.
  // isMacroDefined tests the identifier's macro bit first, which is clear for
  // nearly every name, and asks about module visibility only when it is set:
  // the bit alone is also set for a module's macro that is not visible.
  auto IsMacro = [&](llvm::StringRef Name) {
    return PP.mayBeMacro(Name) && PP.isMacroDefined(Fresh(Name));
  };

  auto Expansions = [&](MetadataSection<ExpandsToView> Sec) -> Result {
    for (const ExpandsToView &D : Sec) {
      if (!Apply(D.Cond))
        continue;
      if (Cost)
        ++Cost->Read[MetadataView::ExpandsTo];
      const MacroInfo *MI =
          PP.mayBeMacro(D.Name)
              ? PP.getMacroDefinition(Fresh(D.Name)).getMacroInfo()
              : nullptr;
      std::optional<std::string> Live =
          MI ? serializeMacroTokens(*MI, PP.getSourceManager(),
                                    PP.getLangOpts())
             : std::nullopt;
      if (!Live || *Live != D.TokenSequence) {
        if (InSection(D.Cond, MismatchReason::Syntactic,
                      D.Name + " expands differently"))
          continue;
        return Because(MismatchReason::Syntactic,
                       D.Name + " expands differently");
      }
    }
    return std::nullopt;
  };

  // What the header's pops take from below it: the top of each name's
  // push_macro stack, as where the copy was written (REPORT.md Q11).
  auto MacroStacks = [&]() -> Result {
    for (const MacroStackView &Stack : Deps.macroStacks()) {
      if (Cost)
        ++Cost->Read[MetadataView::MacroStacks];
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
        if (bool(MI) != E.Popped[I].has_value() ||
            (MI && Live != E.Popped[I]))
          return Because(MismatchReason::MacroStack,
                         E.Name + "'s pushed definitions differ");
      }
    }
    return std::nullopt;
  };

  // An #include the recorded compile skipped must also be skippable here,
  // which means the same file must already have been read.
  auto Inclusions = [&](MetadataSection<InclusionView> Sec) -> Result {
    for (const InclusionView &D : Sec) {
      if (!Apply(D.Cond))
        continue;
      if (Cost)
        ++Cost->Read[MetadataView::Inclusion];
      OptionalFileEntryRef FE = FM.getOptionalFileRef(D.HeaderPath);
      if (!FE ||
          (!HS.hasFileBeenImported(*FE) && !HS.getFileInfo(*FE).isImport)) {
        if (InSection(D.Cond, MismatchReason::Inclusion,
                      D.HeaderPath + " was already included, is not"))
          continue;
        return Because(MismatchReason::Inclusion,
                       D.HeaderPath + " was already included, is not");
      }
    }
    return std::nullopt;
  };

  // Several hundred names for a system header, so after the short lists.
  auto NotMacros = [&](MetadataSection<NameView> Sec) -> Result {
    llvm::TimeTraceScope NamesScope("HeaderCache Check NotAMacro");
    for (const NameView &N : Sec) {
      if (!Apply(N.Cond))
        continue;
      if (Cost)
        ++Cost->Read[MetadataView::NotAMacro];
      if (IsMacro(N.Name)) {
        if (InSection(N.Cond, MismatchReason::Syntactic,
                      N.Name + " was not a macro, is"))
          continue;
        return Because(MismatchReason::Syntactic,
                       N.Name + " was not a macro, is");
      }
    }
    return std::nullopt;
  };

  // Symbol dependencies last: the only ones that cost a name lookup.
  auto Symbols = [&](MetadataSection<SymbolView> Sec) -> Result {
    for (const SymbolView &D : Sec) {
      if (!Apply(D.Cond))
        continue;
      if (Cost)
        ++Cost->Read[MetadataView::Symbol];
      IdentifierInfo &II = Idents.get(D.Name);
      std::string LiveType;
      bool IsType = resolvesToType(*S, II, LiveType);

      bool WasType = D.Classification == SymbolClassification::Type;
      if (IsType != WasType) {
        std::string What = D.Name.str() + (WasType ? " was a type, is not"
                                                   : " was not a type, is");
        if (InSection(D.Cond, MismatchReason::Symbol, What))
          continue;
        return Because(MismatchReason::Symbol, What);
      }
      // "Still a type" is not enough: two translation units can both answer
      // yes with different types, and the stored parse holds one of them.
      if (IsType && LiveType != D.ResolvedTypeName) {
        std::string What = D.Name.str() + " was " + D.ResolvedTypeName.str() +
                           ", is " + LiveType;
        if (InSection(D.Cond, MismatchReason::Symbol, What))
          continue;
        return Because(MismatchReason::Symbol, What);
      }
    }
    return std::nullopt;
  };

  // The part holds the parse of each file's text when it was saved. A plain
  // compile reads the file again; a hit requires it unchanged, by size and
  // modification time, as Clang's own modules check their input files. Every
  // compile checks this itself.
  //
  // The searches below start from the name this compile holds a file by when
  // it read the file before (refOnEntry). With the include path and the site,
  // those names make the searches' key (REPORT.md I9).
  std::string Key;
  if (Site)
    Key.append(reinterpret_cast<const char *>(&Site->SearchContext),
               sizeof(Site->SearchContext));
  auto Files = [&]() -> Result {
    // Within a build files do not change: once a compile of the build found
    // the copy's files as they were, later ones take that (REPORT.md I9).
    std::optional<uint8_t> FilesKnown;
    if (Memo && Memo->Recall)
      FilesKnown = Memo->Recall(SearchMemo::FilesKey);
    if (FilesKnown &&
        *FilesKnown != static_cast<uint8_t>(MismatchReason::None))
      return Because(static_cast<MismatchReason>(*FilesKnown),
                     "an earlier compile of the build found a file changed");
    bool FilesVerified = FilesKnown && Memo->FindOpen;
    auto FileChanged = [&](const llvm::Twine &What) {
      if (Memo)
        Memo->FilesFound = SearchOutcome{
            SearchMemo::FilesKey,
            static_cast<uint8_t>(MismatchReason::FileChanged)};
      return Because(MismatchReason::FileChanged, What);
    };
    for (const FileStampView &F : Deps.files()) {
      if (!Apply(F.Cond))
        continue;
      if (FilesVerified) {
        if (OptionalFileEntryRef Open = Memo->FindOpen(F.Path)) {
          Key += F.Path;
          Key += '\0';
          Key += Open->getName();
          Key += '\0';
        }
        continue;
      }
      if (Cost) {
        ++Cost->FilesCompared;
        ++Cost->Read[MetadataView::Files];
      }
      OptionalFileEntryRef Live =
          FM.getOptionalFileRef(F.Path, /*OpenFile=*/false);
      if (!Live)
        return FileChanged(F.Path + " is gone");
      if (static_cast<uint64_t>(Live->getSize()) != F.Size ||
          static_cast<int64_t>(Live->getModificationTime()) != F.ModTime)
        return FileChanged(F.Path + " changed since the part was saved");
      if (SM.hasFileInfo(&Live->getFileEntry())) {
        Key += F.Path;
        Key += '\0';
        Key += refOnEntry(SM, *Live).getName();
        Key += '\0';
      }
    }
    // Recorded only where every section's files were compared.
    if (Memo && !FilesKnown && !AnySectionFails())
      Memo->FilesFound = SearchOutcome{
          SearchMemo::FilesKey, static_cast<uint8_t>(MismatchReason::None)};
    return std::nullopt;
  };

  // The files the header's own #includes find depend on this compile's
  // include path, which nothing else compares. The stored parse also holds
  // the text each __FILE__ produced, which is the name the producer's compile
  // opened the file by. What each #include found, by its index, for the
  // #includes and __FILE__s inside it.
  // The names themselves matter only where something repeats them: debug
  // information, or warnings the copy stored. postgres opens one header as
  // ../../src/include/x.h or ../../../src/include/x.h by the directory it
  // compiles in; without -g either copy serves (__FILE__ is compared apart).
  bool CheckNames = !Deps.headerName().empty() &&
                    (!Site || Site->NamesMatter || !Deps.diagnostics().empty());
  std::vector<std::pair<OptionalFileEntryRef, ConstSearchDirIterator>> Opened(
      Deps.includes().size(), {std::nullopt, nullptr});
  auto Written = [&](const IncludeView &D) {
    std::string Spelling = D.Angled ? "<" + D.Spelled.str() + ">"
                                    : "\"" + D.Spelled.str() + "\"";
    std::string Was = D.Found.empty() ? std::string("nothing")
                      : CheckNames    ? D.Name.str()
                                      : D.Found.str();
    return Spelling + " found " + Was + ", finds something else";
  };
  auto FileNames = [&](MetadataSection<FileNameView> Sec) -> MismatchReason {
    Lap.next(CheckCost::FileNames);
    for (const FileNameView &D : Sec) {
      if (!Apply(D.Cond))
        continue;
      OptionalFileEntryRef File = D.File < 0
                                      ? OptionalFileEntryRef(Site->File)
                                      : Opened[D.File].first;
      if (!File)
        return Because(MismatchReason::FileName,
                       "__FILE__ is in a file this compile does not find");
      std::string Live =
          fileMacroText(refOnEntry(SM, *File).getName(),
                        D.Which == FileNameDependency::Macro::FileName,
                        PP.getLangOpts(), PP.getTargetInfo());
      if (Live != D.Text)
        return Because(MismatchReason::FileName, "__FILE__ was \"" + D.Text +
                                                     "\", is \"" + Live +
                                                     "\"");
    }
    return MismatchReason::None;
  };
  auto SiteChecks = [&]() -> Result {
    if (Deps.includes().empty() && Deps.fileNames().empty() &&
        Deps.headerName().empty())
      return std::nullopt;
    if (!Site)
      return Because(MismatchReason::Include,
                     "no #include site to redo the header's lookups from");
    // A part exported from a real compile holds its files under the names
    // that compile opened them by, which __FILE__ and debug information
    // repeat; a copy the separate producer built holds its own, and only the
    // files themselves are compared.
    if (CheckNames && Site->File.getName() != Deps.headerName())
      return Because(MismatchReason::Include, "the header was opened as " +
                                                  Deps.headerName() + ", is " +
                                                  Site->File.getName());
    return std::nullopt;
  };

  // An ungrouped copy's searches, all at once. Within a build the files in
  // the include path do not appear or go away, so searches under the same
  // key find what an earlier compile's found. The key says which
  // submodules' searches are made: a section that fails, as one already
  // read, has none.
  auto WholeSearches = [&]() -> Result {
    if (Result R = SiteChecks())
      return R;
    if (Deps.includes().empty() && Deps.fileNames().empty())
      return std::nullopt;
    for (size_t K = 0; K != Loaded.size(); ++K)
      Key += Loaded[K] && !(Sections && Sections->Failing[K]) ? '1' : '0';
    uint64_t SearchKey = llvm::xxh3_64bits(Key);
    // An outcome is kept only where no section's search failed: the key
    // still holds that section.
    bool Whole = true;
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
      return std::nullopt;
    }
    auto Search = [&]() -> MismatchReason {
      std::optional<IncludeView> Bad;
      // A section's search finding another file marks the section, and the
      // searches are made again without it.
      do {
        Bad = redoLookups(Deps.includes(), 0, PP, *Site, CheckNames, Opened,
                          Apply);
        if (Cost)
          Cost->SearchesRedone += Deps.includes().size();
        if (!Bad)
          break;
        std::string What = Written(*Bad);
        if (!InSection(Bad->Cond, MismatchReason::Include, What))
          return Because(MismatchReason::Include, What);
        Whole = false;
      } while (true);
      return FileNames(Deps.fileNames());
    };
    MismatchReason Searched = Search();
    if (Memo && Whole)
      Memo->Found.push_back({SearchKey, static_cast<uint8_t>(Searched)});
    if (Searched != MismatchReason::None)
      return Searched;
    return std::nullopt;
  };

  // A grouped copy's searches, group by group, each under a key of its
  // own: a section's searches are the same whichever other sections load.
  // A group whose searches an earlier compile made is not searched; a
  // later group's #include whose includer is in it has its searches made
  // then.
  std::vector<uint32_t> GroupStart;
  std::vector<char> Searched;
  if (NG) {
    uint32_t At = 0;
    for (uint32_t G = 0; G != NG; ++G) {
      GroupStart.push_back(At);
      At += Deps.template group<IncludeView>(MetadataView::Includes, G).size();
    }
    Searched.assign(NG, 0);
  }
  // Forced: the group of a section that does not load, whose files the
  // compile has not all opened: its searches are made whatever their
  // conditions, and a different file found is the copy's failure.
  auto GroupSearches = [&](uint32_t G, bool Forced = false) -> Result {
    if (G == 0)
      if (Result R = SiteChecks())
        return R;
    MetadataSection<IncludeView> Incs =
        Deps.template group<IncludeView>(MetadataView::Includes, G);
    MetadataSection<FileNameView> Names =
        Deps.template group<FileNameView>(MetadataView::FileNames, G);
    if (Incs.empty() && Names.empty())
      return std::nullopt;
    // A section's failure marks it; the base's is the copy's.
    auto Fail = [&](MismatchReason R, const std::string &What) -> Result {
      if (G > 0 && Sections && !Forced) {
        Sections->Failing[G - 1] = true;
        if (Sections->First == MismatchReason::None) {
          Sections->First = R;
          if (Detail)
            Sections->Detail = What;
        }
        return std::nullopt;
      }
      return Because(R, What);
    };
    uint64_t SearchKey = llvm::xxh3_64bits(Key + "g" + std::to_string(G));
    std::optional<uint8_t> Known;
    if (Memo && Memo->Recall)
      Known = Memo->Recall(SearchKey);
    if (Known) {
      if (Cost)
        ++Cost->SearchesReused;
      if (*Known != static_cast<uint8_t>(MismatchReason::None))
        return Fail(static_cast<MismatchReason>(*Known),
                    "an earlier compile of the build found the header's "
                    "searches to find other files");
      return std::nullopt;
    }
    // The base's #includes hold this group's includers.
    if (G > 0 && !Searched[0] && GroupStart.size() > 1 && GroupStart[1]) {
      Searched[0] = 1;
      if (Cost)
        Cost->SearchesRedone += GroupStart[1];
      if (std::optional<IncludeView> Bad =
              redoLookups(Deps.template group<IncludeView>(
                              MetadataView::Includes, 0),
                          0, PP, *Site, CheckNames, Opened, Apply))
        return Because(MismatchReason::Include, Written(*Bad));
    }
    Searched[G] = 1;
    if (Cost)
      Cost->SearchesRedone += Incs.size();
    MismatchReason Outcome = MismatchReason::None;
    std::string What;
    auto All = [](uint32_t) { return true; };
    std::optional<IncludeView> Bad =
        Forced ? redoLookups(Incs, GroupStart[G], PP, *Site, CheckNames,
                             Opened, All)
               : redoLookups(Incs, GroupStart[G], PP, *Site, CheckNames,
                             Opened, Apply);
    if (Bad) {
      Outcome = MismatchReason::Include;
      What = Written(*Bad);
    } else if (!Forced) {
      Outcome = FileNames(Names);
      if (Outcome != MismatchReason::None && Detail)
        What = *Detail;
    }
    if (Memo)
      Memo->Found.push_back({SearchKey, static_cast<uint8_t>(Outcome)});
    if (Outcome != MismatchReason::None)
      return Fail(Outcome, What);
    return std::nullopt;
  };

  // The part's own declarations were compiled against the outside
  // declarations it refers to. Loading it resolves each name to this
  // compile's own declaration (or merges the part's copy with it), so the
  // name must find one with the same fingerprint: a struct fd_set with other
  // members, or an fscanf with another asm label, would compile differently.
  // The lookup is the one the reader resolves the names with.
  size_t DeclIndex = 0;
  auto Declarations = [&](MetadataSection<DeclarationView> Sec) -> Result {
    for (const DeclarationView &D : Sec) {
      size_t Index = DeclIndex++;
      // Kept in step with the declarations for the caller's pinning.
      if (!Apply(D.Cond)) {
        if (Found)
          Found->push_back(nullptr);
        continue;
      }
      if (Cost)
        ++Cost->Read[MetadataView::Declarations];
      NamedDecl *Live = ASTReader::lookupAtFileScope(
          *S, Idents.get(D.Name), D.In == DeclarationDependency::Space::Tag,
          /*CreateBuiltin=*/true);
      // The declaration an earlier check of the copy found, not declared
      // again since: its fingerprint is the one that check compared.
      if (Prior && Index < Prior->size() && Live &&
          (*Prior)[Index].first == Live &&
          (*Prior)[Index].second == Live->getMostRecentDecl()) {
        if (Found)
          Found->push_back(Live);
        continue;
      }
      std::optional<std::string> F;
      if (Live)
        F = declarationFingerprint(*Live);
      if (!F || !fingerprintSatisfies(D.Fingerprint, *F)) {
        std::string What = D.Name.str() + " was " + D.Fingerprint.str() +
                           ", is " + (F ? *F : std::string("not declared"));
        if (InSection(D.Cond, MismatchReason::Declaration, What)) {
          if (Found)
            Found->push_back(nullptr);
          continue;
        }
        return Because(MismatchReason::Declaration, What);
      }
      if (Found)
        Found->push_back(Live);
    }
    return std::nullopt;
  };

  // A section that does not load: the copy can still hold its declarations
  // under the names its files were opened by -- a type the base's lines
  // name -- so those must be the names this compile has. A file the compile
  // has open, or holds in a copy it served, is compared by name; where one
  // is neither, the section's searches are made. A section that failed is
  // compared after another version of it is read (StateOnly), when the
  // compile has its files.
  auto UnloadedNames = [&](uint32_t G) -> Result {
    if (!CheckNames || !NG || (G && Loaded[G - 1]))
      return std::nullopt;
    bool Unknown = false;
    for (const IncludeView &D :
         Deps.template group<IncludeView>(MetadataView::Includes, G)) {
      if (D.Found.empty() || D.K == IncludeDependency::Kind::HasInclude ||
          D.K == IncludeDependency::Kind::HasIncludeNext)
        continue;
      std::optional<llvm::StringRef> Name =
          Memo && Memo->NameOf ? Memo->NameOf(D.Found) : std::nullopt;
      if (!Name) {
        Unknown = true;
        continue;
      }
      if (*Name != D.Name)
        return Because(MismatchReason::Include, D.Found + " was opened as " +
                                                    D.Name + ", is " + *Name);
    }
    if (Unknown)
      return GroupSearches(G, /*Forced=*/true);
    return std::nullopt;
  };

  for (uint32_t G = 0; G != Groups; ++G) {
    if (!Live(G)) {
      if (Result R = UnloadedNames(G))
        return *R;
      // Kept in step with the declarations for the caller's pinning.
      if (Found)
        Found->resize(
            Found->size() +
                Deps.template group<DeclarationView>(MetadataView::Declarations,
                                                     G)
                    .size(),
            nullptr);
      continue;
    }
    Lap.next(CheckCost::Conditions);
    if (Result R = Conditionals(
            Slice(Deps.conditional(), MetadataView::Conditional, G)))
      return *R;
    Lap.next(CheckCost::Macros);
    if (Result R =
            Expansions(Slice(Deps.expandsTo(), MetadataView::ExpandsTo, G)))
      return *R;
    if (G == 0)
      if (Result R = MacroStacks())
        return *R;
    if (Result R =
            Inclusions(Slice(Deps.inclusion(), MetadataView::Inclusion, G)))
      return *R;
    if (Result R =
            NotMacros(Slice(Deps.notAMacro(), MetadataView::NotAMacro, G)))
      return *R;
    Lap.next(CheckCost::TypeNamesAndPragmas);
    if (Result R = Symbols(Slice(Deps.symbol(), MetadataView::Symbol, G)))
      return *R;
    if (G == 0 && capturePragmaState(*S) != Deps.entry())
      return Because(MismatchReason::PragmaState,
                     "#pragma pack or float state");
    // Within a compile files and the include path do not change: a check
    // made again after the sections it needed were read (StateOnly) leaves
    // them out.
    if (!StateOnly) {
      if (G == 0) {
        Lap.next(CheckCost::FileStamps);
        if (Result R = Files())
          return *R;
      }
      Lap.next(CheckCost::HeaderSearches);
      if (Result R = !NG ? WholeSearches()
                         : Live(G) ? GroupSearches(G)
                                   : UnloadedNames(G))
        return *R;
    }
    Lap.next(CheckCost::Declarations);
    if (Result R = Declarations(
            Slice(Deps.declarations(), MetadataView::Declarations, G)))
      return *R;
  }

  if (Definedness && !Definedness->empty())
    return Because(MismatchReason::Conditional, "only definedness tests differ");
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
  C->SocketPath = SocketPath.str();
  // Unique among the compiles one daemon serves at once: the process, and
  // the client within it (tests run several in one process).
  uint64_t Seed[3] = {static_cast<uint64_t>(::getpid()),
                      reinterpret_cast<uintptr_t>(C.get()),
                      static_cast<uint64_t>(
                          Start.time_since_epoch().count())};
  C->ClientId = llvm::xxh3_64bits(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Seed), sizeof(Seed)));
  auto On = [](const char *Name) {
    const char *V = std::getenv(Name);
    return V && llvm::StringRef(V) == "1";
  };
  C->WaitMode = On("CLANG_HEADER_CACHE_WAIT");
  C->SendThread = C->WaitMode || On("CLANG_HEADER_CACHE_SEND_THREAD");
  if (const char *V = std::getenv("CLANG_HEADER_CACHE_CLAIM");
      V && (llvm::StringRef(V) == "1" || llvm::StringRef(V) == "check" ||
            llvm::StringRef(V) == "deps" || llvm::StringRef(V) == "depscheck"))
    C->ClaimMode = V;
  if (const char *V = std::getenv("CLANG_HEADER_CACHE_COUNTER"))
    C->CounterMode = llvm::StringRef(V) == "1";
  C->Counters.ConnectMicros = microsSince(Start);
  return C;
}

CacheClient::~CacheClient() { finishSending(); }

void CacheClient::enqueue(llvm::SmallVector<char, 0> Msg) {
  {
    std::lock_guard<std::mutex> L(QueueLock);
    if (!Sender.joinable())
      Sender = std::thread([this] { sendLoop(); });
    Queue.push_back(std::move(Msg));
  }
  QueueReady.notify_one();
}

void CacheClient::sendLoop() {
  llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> S =
      llvm::raw_socket_stream::createConnectedUnix(SocketPath);
  if (!S)
    llvm::consumeError(S.takeError());
  for (;;) {
    llvm::SmallVector<char, 0> Msg;
    {
      std::unique_lock<std::mutex> L(QueueLock);
      QueueReady.wait(L, [&] { return Finishing || !Queue.empty(); });
      if (Queue.empty())
        return;
      Msg = std::move(Queue.front());
      Queue.pop_front();
    }
    // A failed send loses the copy, never the compile: the next compile
    // that misses the header writes it.
    if (S && *S && sendMessage(**S, Msg))
      (void)receiveMessage(**S);
  }
}

void CacheClient::finishSending() {
  if (!Sender.joinable())
    return;
  {
    std::lock_guard<std::mutex> L(QueueLock);
    Finishing = true;
  }
  QueueReady.notify_one();
  Sender.join();
  Finishing = false;
}

void CacheClient::compareStored(const PushRequest &R) {
  llvm::SmallVector<char, 0> Msg;
  LookupRequest Req{R.Filename, Build, {}, {}};
  Req.ClientId = ClientId;
  encodeLookupRequest(Req, Msg);
  std::optional<std::string> Reply;
  if (sendMessage(*Conn, Msg))
    Reply = receiveMessage(*Conn);
  std::optional<LookupResponse> Resp =
      Reply ? decodeLookupResponse(*Reply) : std::nullopt;
  llvm::SmallVector<char, 0> Mine;
  encodeMetadata(R.Deps, R.ModuleName, Mine);
  // The same as any stored copy of the name: claims by the whole
  // dependency list (CLANG_HEADER_CACHE_CLAIM=deps) leave several.
  bool Stored = false;
  if (Resp)
    for (const CacheEntryRef &E : Resp->Entries) {
      llvm::StringRef Bytes = Region.read(E.MetadataOffset, E.MetadataLength);
      std::optional<MetadataView> V = MetadataView::open(Bytes);
      if (!V || V->moduleName() != R.ModuleName)
        continue;
      Stored = true;
      if (Bytes == llvm::StringRef(Mine.data(), Mine.size())) {
        ++Counters.ClaimSame;
        return;
      }
    }
  ++(Stored ? Counters.ClaimDiffers : Counters.ClaimNotStored);
}

void CacheClient::noteWriting(llvm::StringRef Filename, bool Ended) {
  if (!WaitMode)
    return;
  llvm::SmallVector<char, 0> Msg;
  encodeWritingNotice({Filename.str(), ClientId, Ended}, Msg);
  enqueue(std::move(Msg));
}

bool CacheClient::claim(llvm::StringRef Filename, llvm::StringRef ModuleName,
                        bool Release) {
  llvm::SmallVector<char, 0> Msg;
  encodeClaimRequest({Filename.str(), ModuleName.str(), Release}, Msg);
  auto Asked = std::chrono::steady_clock::now();
  std::optional<std::string> Reply;
  if (sendMessage(*Conn, Msg))
    Reply = receiveMessage(*Conn);
  Counters.DaemonMicros += microsSince(Asked);
  std::optional<bool> Granted = Reply ? decodeClaimReply(*Reply) : std::nullopt;
  if (Granted && !*Granted && !Release)
    ++Counters.ClaimsRefused;
  return !Granted || *Granted;
}

llvm::StringRef CacheClient::lookup(
    llvm::StringRef Filename, Preprocessor &PP, Sema *S, MetadataView *Matched,
    llvm::function_ref<bool(llvm::StringRef Bytes, llvm::StringRef ModuleName)>
        Use,
    const IncludeSite *Site, PiecesStep Step) {
  ++Counters.Lookups;
  LastPartial.clear();
  LastPieces.clear();
  LastWaited = false;
  LastCounterFit = -1;

  // CLANG_HEADER_CACHE_PIECES: asked again after its plan's sections were
  // read, the header has the copy the plan chose, checked once more: the
  // sections read since can change what its base reads.
  std::vector<CacheEntryRef> Entries;
  std::vector<LookupResponse::SectionVersions> Versions;
  std::optional<std::pair<std::string, CacheEntryRef>> Plan;
  std::swap(Plan, Planned);
  auto Cached = PlannedVersions.find(Filename);
  // The copy a plan chose, asked again after the sections it needed were
  // read: checked for what reading them can change.
  bool Recheck = false;
  if (Step == PiecesStep::Again && Plan && Plan->first == Filename) {
    Entries.push_back(std::move(Plan->second));
    PlannedVersions.clear();
    Recheck = true;
  } else if (Step == PiecesStep::WholeOnly && Cached != PlannedVersions.end()) {
    // A version of a section of the copy a plan chose: it came with the
    // copy.
    Entries = std::move(Cached->second);
    PlannedVersions.erase(Cached);
  } else {
    llvm::SmallVector<char, 0> Msg;
    LookupRequest Req{Filename.str(), Build, std::move(Learned),
                      std::move(Used)};
    Req.ClientId = ClientId;
    Req.Wait = WaitMode;
    encodeLookupRequest(Req, Msg);
    Learned.clear();
    Used.clear();
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
    LastWaited = Resp->Waited;
    if (LastWaited)
      ++Counters.Waited;
    Entries = std::move(Resp->Entries);
    Versions = std::move(Resp->Versions);
  }
  // A plan for another header stays for its own asking again.
  if (Plan && Plan->first != Filename)
    std::swap(Plan, Planned);
  Plan.reset();

  LastCandidates = Entries.size();
  LastRejection = MismatchReason::None;
  MatchedDecls.clear();
  // A file the compile has entered, by the canonical path a copy names it
  // by. Found by file name first, so that only a file named the same has its
  // canonical path made.
  SourceManager &SM = PP.getSourceManager();
  FileManager &FM = PP.getFileManager();
  auto FindOpen = [&](llvm::StringRef CanonicalPath) -> OptionalFileEntryRef {
    if (SM.getNextLocalOffset() != OpenSeenUpTo) {
      OpenSeenUpTo = SM.getNextLocalOffset();
      for (auto It = SM.fileinfo_begin(), E = SM.fileinfo_end(); It != E; ++It)
        if (OpenSeen.insert(&It->first.getFileEntry()).second) {
          FileEntryRef Ref =
              It->second->OrigEntry ? *It->second->OrigEntry : It->first;
          OpenByName[llvm::sys::path::filename(Ref.getName())].push_back(Ref);
        }
    }
    auto It = OpenByName.find(llvm::sys::path::filename(CanonicalPath));
    if (It == OpenByName.end())
      return std::nullopt;
    for (FileEntryRef Ref : It->second)
      if (FM.getCanonicalName(Ref) == CanonicalPath)
        return Ref;
    return std::nullopt;
  };
  auto Read = [&](const LeftOutView &L) {
    IdentifierInfo *II = PP.getIdentifierInfo(L.Guard);
    PP.refreshIdentifier(*II);
    if (PP.isMacroDefined(II))
      return true;
    OptionalFileEntryRef FE = FM.getOptionalFileRef(L.Path);
    return FE && PP.alreadyIncluded(*FE);
  };
  // CLANG_HEADER_CACHE_PIECES: the first copy whose base fits where the
  // whole does not, and what it needs: each section that would load and
  // fails, and each header the copy left out that this compile has not
  // read, in the order the header includes them.
  std::optional<CacheEntryRef> PlanEntry;
  std::vector<LeftOutRecord> Need;

  auto NameOf = [&](llvm::StringRef CanonicalPath)
      -> std::optional<llvm::StringRef> {
    if (OptionalFileEntryRef F = FindOpen(CanonicalPath))
      return F->getName();
    auto It = ServedNames.find(CanonicalPath);
    if (It != ServedNames.end())
      return llvm::StringRef(It->second);
    return std::nullopt;
  };
  // With logChecks, CLANG_HEADER_CACHE_LOG_COMPLETE=1 compares every
  // definedness test of each copy instead of stopping at the first that
  // differs (measurement: what else each copy differs in).
  static const bool CompleteChecks = [] {
    const char *V = std::getenv("CLANG_HEADER_CACHE_LOG_COMPLETE");
    return V && llvm::StringRef(V) == "1";
  }();
  std::vector<NamedDecl *> Found;
  std::vector<bool> CandidateLoaded;
  for (const CacheEntryRef &E : Entries) {
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
    SearchMemo Memo{Recall, {}, std::nullopt, FindOpen, NameOf};
    CheckCost Prev = Counters.Check;
    // CLANG_HEADER_CACHE_PIECES: a copy with sections or left-out headers is
    // checked once for both, whether it fits whole and else whether its
    // base does, until a plan is found. The header is included again from
    // beside it, by its file name, so a copy with an #include_next in its
    // own lines, which would search from elsewhere, gives none.
    bool MayPlan =
        PiecesMode && Step == PiecesStep::Plan && !PlanEntry &&
        (!Deps->submodules().empty() || !Deps->leftOut().empty()) &&
        !(Site && Site->Dir &&
          llvm::any_of(Deps->includes(), [](const IncludeView &D) {
            return D.Includer == -1 &&
                   (D.K == IncludeDependency::Kind::IncludeNext ||
                    D.K == IncludeDependency::Kind::HasIncludeNext);
          }));
    std::vector<bool> Plugged;
    SectionFailures Sections;
    std::vector<std::string> AllFails;
    if (MayPlan)
      for (const LeftOutView &L : Deps->leftOut())
        Plugged.push_back(!Read(L));
    {
      llvm::TimeTraceScope Scope("HeaderCache Check");
      Found.clear();
      // Asked again after the sections its plan needed were read, the
      // chosen copy is checked for what those sections can change.
      Why = matchesLiveState(*Deps, PP, S,
                             LogRejections || LogChecks ? &Detail : nullptr,
                             Site, &Counters.Check, &Found, &Memo,
                             /*SkipConditionals=*/false, &CandidateLoaded,
                             MayPlan ? &Plugged : nullptr,
                             MayPlan ? &Sections : nullptr,
                             LogChecks && CompleteChecks ? &AllFails : nullptr,
                             /*StateOnly=*/Recheck,
                             Recheck ? &PlannedDecls : nullptr);
    }
    MismatchReason Rest = Why;
    if (MayPlan && Why == MismatchReason::None) {
      size_t K = 0;
      for (const SubmoduleView &M : Deps->submodules()) {
        if (CandidateLoaded[K] && Sections.Failing[K])
          Need.push_back({M.Guard.str(), M.Path.str(), M.Spelled.str(),
                          M.Angled, M.At});
        ++K;
      }
      K = 0;
      for (const LeftOutView &L : Deps->leftOut())
        if (Plugged[K++])
          Need.push_back(
              {L.Guard.str(), L.Path.str(), L.Spelled.str(), L.Angled, L.At});
      if (!Need.empty()) {
        PlanEntry = E;
        PlannedDecls.clear();
        for (NamedDecl *D : Found)
          PlannedDecls.push_back(
              {D, D ? D->getMostRecentDecl() : static_cast<const Decl *>(nullptr)});
        Why = Sections.First != MismatchReason::None
                  ? Sections.First
                  : MismatchReason::Conditional;
        Detail = Sections.First != MismatchReason::None
                     ? Sections.Detail
                     : "a header the copy left out is not read";
      }
    }
    if (LogChecks) {
      // Where the check stopped: each step's time, and each section's
      // elements read out of its size.
      std::string Line;
      llvm::raw_string_ostream OS(Line);
      const CheckCost &Now = Counters.Check;
      OS << Filename << " lookup=" << Counters.Lookups
         << " cand=" << (&E - Entries.data()) << "/"
         << Entries.size() << " entry=" << E.MetadataOffset
         << " why=" << static_cast<unsigned>(Why) << " ns=";
      for (unsigned I = 0; I != CheckCost::NumSteps; ++I)
        OS << (I ? "," : "") << Now.Nanos[I] - Prev.Nanos[I];
      OS << " reused=" << Now.SearchesReused - Prev.SearchesReused
         << " redone=" << Now.SearchesRedone - Prev.SearchesRedone
         << " read=";
      size_t Sizes[] = {Deps->conditional().size(), Deps->expandsTo().size(),
                        Deps->macroStacks().size(), Deps->inclusion().size(),
                        Deps->notAMacro().size(),   Deps->symbol().size(),
                        Deps->files().size(),       Deps->includes().size(),
                        Deps->fileNames().size(),   Deps->declarations().size()};
      for (unsigned I = 0; I != std::size(Sizes); ++I)
        OS << (I ? "," : "") << Now.Read[I] - Prev.Read[I] << "/" << Sizes[I];
      OS << " bytes=" << E.MetadataLength << " module=" << Deps->moduleName();
      // Every definedness test that differs outside a failing section, +
      // for "was not defined, is" and - for "was defined, is not", and the
      // first other difference, 0 for none (REPORT.md T15). A copy that
      // gave a plan says how many sections and headers it needs.
      OS << " fails=";
      for (size_t I = 0; I != AllFails.size(); ++I)
        OS << (I ? "," : "") << AllFails[I];
      OS << " rest="
         << static_cast<unsigned>(Rest == MismatchReason::Conditional &&
                                          !AllFails.empty()
                                      ? MismatchReason::None
                                      : Rest);
      if (PlanEntry && PlanEntry->MetadataOffset == E.MetadataOffset &&
          Why != MismatchReason::None)
        OS << " plan=" << Need.size();
      std::string D = Detail;
      std::replace(D.begin(), D.end(), ' ', '_');
      OS << " detail=" << D;
      Counters.Checks.push_back(std::move(Line));
    }
    if (Memo.FilesFound)
      Memo.Found.push_back(*Memo.FilesFound);
    for (const SearchOutcome &O : Memo.Found) {
      Outcomes[{E.MetadataOffset, O.Key}] = O.Outcome;
      if (Build)
        Learned.push_back({Filename.str(), E.MetadataOffset, O});
    }
    Counters.CheckMicros += microsSince(Checking);
    // CLANG_HEADER_CACHE_COUNTER_PROBE: a copy whose header expanded
    // __COUNTER__ is not served, its values being the writing compile's
    // (DECISIONS.md 2.7). Where it fits otherwise, what it would have needed
    // is noted: the counter where the header begins at the value it began
    // at where the copy was written, or, its values having landed only in
    // names and strings, renaming them.
    // CLANG_HEADER_CACHE_COUNTER=1: the copy holds the values the header's
    // __COUNTER__ expansions had, which they have again only where the
    // counter starts where it did (DECISIONS.md 2.7).
    if (Why == MismatchReason::None && Deps->counterUses() && CounterMode) {
      if (PP.getCounterValue() != Deps->counterStart())
        Why = MismatchReason::Counter;
    } else if (Why == MismatchReason::None && Deps->counterUses()) {
      Why = MismatchReason::Counter;
      LastCounterFit = std::max(LastCounterFit,
                                PP.getCounterValue() == Deps->counterStart() ? 2
                                : Deps->counterSemantic()                    ? 0
                                                                             : 1);
    }
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
    LastSubmodules = CandidateLoaded;
    LastMetadata = Deps;
    LastSubmoduleNames.clear();
    LastSubmodulePaths.clear();
    for (const SubmoduleView &M : Deps->submodules()) {
      LastSubmoduleNames.push_back(M.Name.str());
      LastSubmodulePaths.push_back(M.Path.str());
    }
    if (Use && !Use(Bytes, Deps->moduleName()))
      continue;
    ++Counters.Hits;
    if (LastWaited)
      ++Counters.WaitedHits;
    Used.push_back(E.MetadataOffset);
    // Each file the copy holds has, in this compile, the name its check
    // found it by.
    if (PiecesMode && !Deps->headerName().empty())
      for (const IncludeView &D : Deps->includes())
        if (!D.Found.empty() && (D.K == IncludeDependency::Kind::Include ||
                                 D.K == IncludeDependency::Kind::IncludeNext))
          ServedNames.try_emplace(D.Found, D.Name);
    if (LogChecks)
      Counters.Checks.back() += " used";
    if (Matched)
      *Matched = *Deps;
    return Bytes;
  }

  if (PlanEntry) {
    llvm::stable_sort(Need, [](const LeftOutRecord &A,
                               const LeftOutRecord &B) { return A.At < B.At; });
    LastPieces = std::move(Need);
    Planned.emplace(Filename.str(), std::move(*PlanEntry));
    // A section with no version stored has none to ask the daemon for.
    PlannedVersions.clear();
    for (const LeftOutRecord &R : LastPieces)
      PlannedVersions[Filename.str() + "#section:" + R.Path];
    for (LookupResponse::SectionVersions &V : Versions)
      PlannedVersions[Filename.str() + "#section:" + V.Section] =
          std::move(V.Entries);
    ++Counters.BaseFits;
    return {};
  }

  // CLANG_HEADER_CACHE_PARTIAL: a copy that fails only on what headers it
  // left out gave it, where this compile has not read them, serves once they
  // are parsed first: the caller enters them, then asks for the header again
  // (a partial hit).
  LastPartial.clear();
  if (!PartialMode)
    return {};
  for (const CacheEntryRef &E : Entries) {
    std::optional<MetadataView> Deps = MetadataView::open(
        Region.read(E.MetadataOffset, E.MetadataLength));
    if (!Deps || Deps->leftOut().empty())
      continue;
    std::vector<bool> Plugged;
    bool Any = false;
    for (const LeftOutView &L : Deps->leftOut()) {
      Plugged.push_back(!Read(L));
      Any |= Plugged.back();
    }
    if (!Any)
      continue;
    auto Checking = std::chrono::steady_clock::now();
    MismatchReason Why = matchesLiveState(
        *Deps, PP, S, /*Detail=*/nullptr, Site, /*Cost=*/nullptr,
        /*Found=*/nullptr, /*Memo=*/nullptr, /*SkipConditionals=*/false,
        /*Loaded=*/nullptr, &Plugged);
    Counters.CheckMicros += microsSince(Checking);
    if (Why != MismatchReason::None)
      continue;
    size_t K = 0;
    for (const LeftOutView &L : Deps->leftOut())
      if (Plugged[K++])
        LastPartial.push_back(
            {L.Guard.str(), L.Path.str(), L.Spelled.str(), L.Angled});
    ++Counters.Partial;
    break;
  }
  return {};
}

llvm::StringRef CacheClient::fetch(llvm::StringRef Filename,
                                   llvm::StringRef ModuleName, uint64_t Hash,
                                   std::optional<MetadataView> &Meta) {
  ++Counters.Fetches;
  llvm::SmallVector<char, 0> Msg;
  encodeLookupRequest(
      {Filename.str(), Build, std::move(Learned), std::move(Used)}, Msg);
  Learned.clear();
  Used.clear();
  auto Asked = std::chrono::steady_clock::now();
  std::optional<std::string> Reply;
  if (sendMessage(*Conn, Msg))
    Reply = receiveMessage(*Conn);
  Counters.DaemonMicros += microsSince(Asked);
  if (!Reply)
    return {};
  std::optional<LookupResponse> Resp = decodeLookupResponse(*Reply);
  if (!Resp)
    return {};
  for (const CacheEntryRef &E : Resp->Entries) {
    std::optional<MetadataView> Deps = MetadataView::open(
        Region.read(E.MetadataOffset, E.MetadataLength));
    if (!Deps || Deps->moduleName() != ModuleName)
      continue;
    llvm::StringRef Bytes = Region.read(E.Offset, E.Length);
    if (Bytes.empty() || llvm::xxh3_64bits(Bytes) != Hash)
      continue;
    ++Counters.Fetched;
    Meta = Deps;
    return Bytes;
  }
  return {};
}

bool CacheClient::seenBefore(llvm::StringRef Filename,
                             llvm::StringRef ModuleName) {
  llvm::SmallVector<char, 0> Msg;
  encodeSeenRequest({Filename.str(), ModuleName.str()}, Msg);
  auto Asked = std::chrono::steady_clock::now();
  std::optional<std::string> Reply;
  if (sendMessage(*Conn, Msg))
    Reply = receiveMessage(*Conn);
  Counters.DaemonMicros += microsSince(Asked);
  std::optional<bool> Seen = Reply ? decodeSeenReply(*Reply) : std::nullopt;
  return !Seen || *Seen;
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
  WithSearches.Used = std::move(Used);
  Learned.clear();
  Used.clear();
  encodeTimingReport(WithSearches, Msg);
  bool Sent = sendMessage(*Conn, Msg) && receiveMessage(*Conn).has_value();
  Counters.ReportMicros += microsSince(Start);
  return Sent;
}

bool CacheClient::push(const PushRequest &Req) {
  llvm::SmallVector<char, 0> Msg;
  encodePushRequest(Req, Msg);
  if (SendThread) {
    enqueue(std::move(Msg));
    return true;
  }
  if (!sendMessage(*Conn, Msg))
    return false;
  return receiveMessage(*Conn).has_value();
}
