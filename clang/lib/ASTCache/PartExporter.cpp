//===- PartExporter.cpp -----------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/PartExporter.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/DeclFingerprint.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/DeclBoundaryObserver.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/ModuleMap.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/ModuleManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <chrono>
#include <unistd.h>
#include <map>
#include "llvm/ADT/StringSet.h"
#include <set>
#include <mutex>

using namespace clang;
using namespace clang::ast_cache;

PartExporter::~PartExporter() = default;

namespace {

class Exporter;

/// The exporter of each compile that has one. The plugin consumer below is
/// created for every compile, before or after the exporter, and finds it here.
std::mutex RegistryLock;
llvm::DenseMap<const CompilerInstance *, Exporter *> &registry() {
  static llvm::DenseMap<const CompilerInstance *, Exporter *> R;
  return R;
}

Exporter *exporterFor(const CompilerInstance &CI) {
  std::lock_guard<std::mutex> L(RegistryLock);
  return registry().lookup(&CI);
}

std::string absolutePath(FileManager &FM, llvm::StringRef Name) {
  llvm::SmallString<256> Path(Name);
  FM.makeAbsolutePath(Path);
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

/// CLANG_HEADER_CACHE_DDG: the declarations each file's text uses that another
/// file declared first, by the file the use is written in (a macro's
/// expansion counts where the macro is used). Kind 'D' is a name that must be
/// declared -- a typedef, function, variable, enumerator or field; kind 'T' a
/// struct, union or enum tag, which a pointer to it does not need declared.
class DeclUses : public RecursiveASTVisitor<DeclUses> {
public:
  explicit DeclUses(const SourceManager &SM) : SM(SM) {}

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    use('D', E->getLocation(), E->getDecl());
    return true;
  }
  bool VisitMemberExpr(MemberExpr *E) {
    use('D', E->getMemberLoc(), E->getMemberDecl());
    return true;
  }
  bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
    use('D', TL.getNameLoc(), TL.getDecl());
    return true;
  }
  bool VisitTagTypeLoc(TagTypeLoc TL) {
    if (!TL.isDefinition())
      use('T', TL.getNameLoc(), TL.getDecl());
    return true;
  }

  struct Use {
    char Kind;
    std::string Name;
    FileID Origin;
    std::string Value;
  };
  llvm::DenseMap<FileID, std::vector<Use>> Uses;

private:
  void use(char Kind, SourceLocation Loc, const NamedDecl *D) {
    if (!D || Loc.isInvalid())
      return;
    SourceLocation Declared = D->getCanonicalDecl()->getLocation();
    if (Declared.isInvalid())
      return;
    FileID From = SM.getFileID(SM.getExpansionLoc(Loc));
    FileID Origin = SM.getFileID(SM.getExpansionLoc(Declared));
    if (From.isInvalid() || Origin.isInvalid() || From == Origin)
      return;
    // A file that declares the name itself (git's progress.h, `struct
    // repository;`) does not need it from another.
    for (const Decl *R : D->redecls())
      if (R->getLocation().isValid() &&
          SM.getFileID(SM.getExpansionLoc(R->getLocation())) == From)
        return;
    std::string Name = D->getNameAsString();
    if (Name.empty())
      return;
    std::string Value;
    if (const auto *T = dyn_cast<TagDecl>(D)) {
      Name = (llvm::Twine(T->getKindName()) + " " + Name).str();
      Value = T->getDefinition() ? "defined" : "declared";
    } else if (const auto *T = dyn_cast<TypedefNameDecl>(D)) {
      Value = T->getUnderlyingType().getCanonicalType().getAsString();
    } else if (const auto *E = dyn_cast<EnumConstantDecl>(D)) {
      Value = toString(E->getInitVal(), 10);
    } else if (const auto *V = dyn_cast<ValueDecl>(D)) {
      Value = V->getType().getCanonicalType().getAsString();
    }
    Uses[From].push_back({Kind, std::move(Name), Origin,
                          llvm::utohexstr(llvm::xxh3_64bits(Value))});
  }

  const SourceManager &SM;
};

class Exporter : public PartExporter, public PPCallbacks {
public:
  Exporter(CompilerInstance &CI, ASTCacheDependencyTracker &Tracker,
           std::function<void(PushRequest &&)> Deliver, bool ByName,
           const WarningPragmaTracker *Pragmas)
      : CI(CI), SM(CI.getSourceManager()), Tracker(Tracker),
        Deliver(std::move(Deliver)), ByName(ByName), Pragmas(Pragmas) {
    const char *Link = std::getenv("CLANG_HEADER_CACHE_LINK");
    LinkMode = Link && llvm::StringRef(Link) == "1";
    std::lock_guard<std::mutex> L(RegistryLock);
    registry()[&CI] = this;
  }

  ~Exporter() override {
    std::lock_guard<std::mutex> L(RegistryLock);
    registry().erase(&CI);
  }

  void exportNext(FileEntryRef File) override {
    Expected = &File.getFileEntry();
  }

  void served(serialization::ModuleFile *Fold, const MetadataView *Deps,
              SourceLocation Loc,
              llvm::function_ref<SourceLocation(llvm::StringRef)> Resolve)
      override {
    if (!Fold || !Deps) {
      for (Part &P : Open)
        if (P.Decline.empty())
          P.Decline = "a saved copy was served inside the header";
      return;
    }
    // The copy's files get entries made here, inside every part open now;
    // a part that folded the copy before has its own already.
    llvm::DenseMap<int, FileID> Entries;
    if (llvm::any_of(Open, [&](const Part &P) {
          return P.Decline.empty() && !P.Folded.count(Fold);
        }))
      entriesFor(*Fold, Loc, Entries);
    for (Part &P : Open) {
      if (!P.Decline.empty())
        continue;
      if (P.Folded.insert(Fold).second)
        P.FoldedEntries.insert(Entries.begin(), Entries.end());
      // The copy's files are the part's: their stamps, and their header
      // search marks for a loading compile.
      for (const FileStampView &F : Deps->files()) {
        P.Stamps[F.Path.str()] = {F.Path.str(), F.Size, F.ModTime};
        if (OptionalFileEntryRef FE =
                CI.getFileManager().getOptionalFileRef(F.Path))
          P.Files.insert(&FE->getFileEntry());
      }
      // What the copy does to macros, the part does: the hit replays each
      // at the #include, inside the part.
      for (const MacroEffectView &M : Deps->macroEffects())
        touch(P, CI.getPreprocessor().getIdentifierInfo(M.Name));
    }
    Tracker.foldServed(Loc, Fold->SLocEntryBaseID, Fold->LocalNumSLocEntries,
                       *Deps, Resolve);
  }

  void linked(llvm::StringRef Path, llvm::StringRef Module,
              llvm::StringRef Guard, serialization::ModuleFile *MF,
              const MetadataView &Deps, SourceLocation Loc,
              llvm::function_ref<SourceLocation(llvm::StringRef)> Resolve,
              unsigned FirstModule, uint64_t Hash) override {
    if (Open.empty())
      return;
    Preprocessor &PP = CI.getPreprocessor();
    // A hit replays the links before the copy's own directives: one that
    // changes a macro the lines before it set would be undone by them.
    for (Part &P : Open)
      for (const MacroEffectView &M : Deps.macroEffects())
        if (P.Decline.empty() && P.Touched.contains(PP.getIdentifierInfo(M.Name)))
          P.Decline = "a copy it links changes a macro its lines set before: " +
                      M.Name.str();
    for (Part &P : Open) {
      if (!P.Decline.empty())
        continue;
      // The copy's files are the part's: their stamps, for a loading
      // compile's check.
      for (const FileStampView &F : Deps.files()) {
        P.Stamps[F.Path.str()] = {F.Path.str(), F.Size, F.ModTime};
        if (OptionalFileEntryRef FE =
                CI.getFileManager().getOptionalFileRef(F.Path))
          P.Files.insert(&FE->getFileEntry());
      }
    }
    // The innermost header holds the link; a part not written hands it to
    // the one around it (handOver).
    Part &In = Open.back();
    bool OnGuard = !Guard.empty() && !In.Touched.contains(
                                         PP.getIdentifierInfo(Guard));
    // Skipped where the header was read, the link must read nothing the
    // lines before it set: no macro they defined, no name they declared.
    auto Set = [&](llvm::StringRef Name) {
      return In.Touched.contains(PP.getIdentifierInfo(Name));
    };
    for (const ConditionalView &C : Deps.conditional())
      OnGuard = OnGuard && !Set(C.MacroName);
    for (const ExpandsToView &E : Deps.expandsTo())
      OnGuard = OnGuard && !Set(E.Name);
    for (const NameView &N : Deps.notAMacro())
      OnGuard = OnGuard && !Set(N.Name);
    for (const SymbolView &S : Deps.symbol()) {
      if (!OnGuard)
        break;
      SourceLocation At = Resolve ? Resolve(S.Name) : SourceLocation();
      if (At.isValid() && SM.isLocalSourceLocation(At) &&
          In.contains(SM.getExpansionLoc(At).getRawEncoding()))
        OnGuard = false;
    }
    In.Links.push_back(
        {Loc, {Path.str(), Module.str(), Guard.str(), OnGuard, {}, Hash}});
    In.LinkSets.emplace_back();
    for (const MacroEffectView &M : Deps.macroEffects())
      In.LinkSets.back().push_back(M.Name.str());
    In.LinkFroms.push_back(
        {FirstModule, static_cast<unsigned>(modulesLoaded()), 0, 0});
    if (getenv("DEBUG_LINK")) {
      llvm::errs() << "SERVED " << Path << " in " << In.Stored << " decls:";
      for (const DeclarationView &D : Deps.declarations())
        llvm::errs() << " " << D.Name
                     << (ownLinesDeclare(In,
                                         D.In == DeclarationDependency::Space::Tag,
                                         D.Name)
                             ? "(own)"
                             : "");
      llvm::errs() << "\n";
    }
    for (const DeclarationView &D : Deps.declarations()) {
      In.LinkOutside.push_back({int(In.Links.size()) - 1,
                                {D.In, D.Name.str(), D.Fingerprint.str()}});
      // A link loads before the lines of the copies around it: one that
      // names a declaration they made cannot be linked, and they hold none
      // of its lines either.
      if (ownLinesDeclare(In, D.In == DeclarationDependency::Space::Tag,
                          D.Name))
        for (Part &O : Open)
          if (O.Decline.empty())
            O.Decline = "a copy served inside it names a declaration of its "
                        "own lines: " + D.Name.str();
    }
    ++Counts.LinksServed;
    if (MF)
      Tracker.foldServed(Loc, MF->SLocEntryBaseID, MF->LocalNumSLocEntries,
                         Deps, Resolve, /*Linked=*/true);
  }

  void replaying(bool On) override { Replaying = On; }

  void writeOnlyIfSeen(
      std::function<bool(llvm::StringRef, llvm::StringRef)> Seen) override {
    SeenBefore = std::move(Seen);
  }

  bool writing() const override {
    return llvm::any_of(Open, [](const Part &P) { return P.Decline.empty(); });
  }

  void exportWhenEntered(std::string Path) override {
    ExpectedPath = std::move(Path);
  }

  void diagnostic(DiagnosticsEngine::Level Level,
                  const Diagnostic &Info) override {
    if (Level == DiagnosticsEngine::Ignored || Replaying)
      return;
    bool Note = Level == DiagnosticsEngine::Note;
    for (Part &P : Open)
      take(P, Level, Info);
    // A header's last declaration is finished after the header is left: the
    // parser has read a token past its closing brace. What Clang says then
    // about a place inside the header, until the part is written, is the
    // header's too; a note goes with the diagnostic before it.
    for (Part &P : Ready) {
      if (!Note)
        P.TakingNotes = Info.getLocation().isValid() &&
                        SM.isLocalSourceLocation(Info.getLocation()) &&
                        inPart(P, Info.getLocation());
      if (P.TakingNotes)
        take(P, Level, Info);
    }
  }

  const Stats &stats() const override { return Counts; }

  /// Whether the tracker should keep a record for \p FID: only for the
  /// headers being exported.
  bool tracks(FileID FID) const { return Tracked.contains(FID); }
  /// Whether a header being exported is open.
  bool inside() const { return !Open.empty(); }

  //===--------------------------------------------------------------------===//
  // The header's region, from the preprocessor.

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID PrevFID) override {
    if (Reason == EnterFile) {
      FileID FID = SM.getFileID(Loc);
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
      const FileEntry *Want = Expected;
      Expected = nullptr;
      if (!FE)
        return;
      // A header inside one being exported can be held as a submodule of
      // its part (REPORT.md T17) if it begins between declarations and
      // leaves the pragmas as it found them.
      if (!Open.empty()) {
        Nested &N = NestedAt[FID];
        DeclBoundaryObserver *B = CI.getPreprocessor().getDeclBoundaryObserver();
        N.AtBoundary = B && B->isAtDeclarationBoundary();
        if (CI.hasSema())
          N.Entry = capturePragmaState(CI.getSema());
        if (Pragmas)
          N.Mark = Pragmas->mark();
      }
      for (Part &P : Open)
        hold(P, *FE);
      bool ByPath =
          !ExpectedPath.empty() &&
          absolutePath(CI.getFileManager(), FE->getName()) == ExpectedPath;
      if (&FE->getFileEntry() != Want && !ByPath)
        return;
      if (ByPath)
        ExpectedPath.clear();
      begin(FID, *FE);
      return;
    }
    if (Reason != ExitFile)
      return;
    if (auto It = NestedAt.find(PrevFID); It != NestedAt.end()) {
      Nested &N = It->second;
      std::optional<std::vector<WarningPragma>> Since;
      if (Pragmas)
        Since = Pragmas->since(N.Mark);
      N.Neutral = CI.hasSema() &&
                  capturePragmaState(CI.getSema()) == N.Entry &&
                  (!Pragmas || (Since && Since->empty()));
    }
    if (Open.empty() || Open.back().FID != PrevFID)
      return;
    end();
  }

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *) override {
    for (Part &P : Open)
      touch(P, MacroNameTok.getIdentifierInfo());
  }

  void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &,
                      const MacroDirective *) override {
    for (Part &P : Open)
      touch(P, MacroNameTok.getIdentifierInfo());
  }

  void PragmaPushMacro(SourceLocation, const IdentifierInfo *II,
                       const MacroInfo *) override {
    for (Part &P : Open) {
      MacroStack &S = stackOf(P, II);
      S.Pushed.push_back(S.Now);
    }
  }

  void PragmaPopMacro(SourceLocation, const IdentifierInfo *II,
                      const MacroInfo *Taken) override {
    for (Part &P : Open) {
      MacroStack &S = stackOf(P, II);
      if (!S.Pushed.empty()) {
        S.Now = S.Pushed.back();
        S.Pushed.pop_back();
        continue;
      }
      S.Popped.push_back(Taken);
      S.Now = S.Popped.size();
    }
  }

  void MacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                    SourceRange, const MacroArgs *) override {
    if (Open.empty())
      return;
    const MacroInfo *MI = MD.getMacroInfo();
    if (!MI || !MI->isBuiltinMacro())
      return;
    llvm::StringRef Name = MacroNameTok.getIdentifierInfo()->getName();
    if (!isTranslationUnitVaryingBuiltin(Name))
      return;
    for (Part &P : Open)
      if (P.Decline.empty())
        P.Decline = "expands " + Name.str();
  }

  //===--------------------------------------------------------------------===//
  // From the plugin consumer.

  /// A top-level declaration is finished: every header that ended before it
  /// has its last declaration finished too.
  void declarationFinished(DeclGroupRef Group) {
    if (!Group.isNull()) {
      Decl *Last = *(Group.end() - 1);
      if (Last->getLexicalDeclContext()->isTranslationUnit())
        LastTopLevel = Last;
    }
    if (!Ready.empty())
      writeReady();
  }

  void translationUnitFinished() {
    writeReady();
    if (const char *Dir = std::getenv("CLANG_HEADER_CACHE_DDG"))
      dumpGraph(Dir);
  }

  /// CLANG_HEADER_CACHE_DDG=<dir>, for measuring a codebase's headers: for
  /// each file this compile read, to <dir>/<pid>.ddg, the file that
  /// included it and a hash of what its copy's metadata would say ("R"),
  /// and each state it read that another file set ("E": the kind, the name,
  /// the file that set it, and whether that file is inside it). The
  /// metadata itself goes, once per hash, to <dir>/v/<hash>.
  void dumpGraph(llvm::StringRef Dir) {
    using FR = ASTCacheDependencyTracker::FileReads;
    std::error_code EC;
    llvm::raw_fd_ostream OS((Dir + "/" + llvm::Twine(::getpid()) + ".ddg").str(),
                            EC);
    if (EC)
      return;
    llvm::sys::fs::create_directories(Dir + "/v");
    FileManager &FM = CI.getFileManager();
    auto PathOf = [&](FileID F) -> std::string {
      if (F.isInvalid())
        return "-";
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(F);
      return FE ? absolutePath(FM, FE->getName()) : "-";
    };
    llvm::ArrayRef<FR> All = Tracker.allFileReads();
    std::vector<const FR *> Stack;
    // The declarations the headers use from other files ('D', 'T').
    DeclUses Decls(SM);
    if (CI.hasASTContext())
      for (Decl *D : CI.getASTContext().getTranslationUnitDecl()->decls()) {
        SourceLocation L = SM.getExpansionLoc(D->getLocation());
        if (L.isValid() && SM.getFileID(L) != SM.getMainFileID())
          Decls.TraverseDecl(D);
      }
    OS << "C\t" << CI.getFrontendOpts().Inputs[0].getFile() << "\n";
    for (const FR &F : All) {
      while (!Stack.empty() && Stack.back()->Exit && Stack.back()->Exit < F.Enter)
        Stack.pop_back();
      std::string Path = PathOf(F.FID);
      std::string Parent = Stack.empty() ? "-" : PathOf(Stack.back()->FID);
      uint64_t Hash = 0;
      if (const HeaderDependencySet *Rec = Tracker.recordFor(F.FID)) {
        HeaderDependencySet D = *Rec;
        D.Files.clear();
        D.HeaderName.clear();
        llvm::SmallVector<char, 256> Bytes;
        encodeMetadata(D, {}, Bytes);
        Hash = llvm::xxh3_64bits(llvm::StringRef(Bytes.data(), Bytes.size()));
        std::string VPath = (Dir + "/v/" + llvm::utohexstr(Hash)).str();
        int FD;
        if (!llvm::sys::fs::openFileForWrite(VPath, FD,
                                             llvm::sys::fs::CD_CreateNew)) {
          llvm::raw_fd_ostream V(FD, /*shouldClose=*/true);
          V << "header\t" << Path << "\n";
          for (const ConditionalDependency &C : D.Conditional)
            V << "cond\t" << C.MacroName << "\t" << C.WasDefined << "\n";
          for (const SyntacticDependency &S : D.Syntactic)
            V << "syn\t" << S.Name << "\t" << int(S.Required) << "\t"
              << llvm::xxh3_64bits(S.TokenSequence) << "\n";
          for (const SymbolDependency &S : D.Symbol)
            V << "sym\t" << S.Name << "\t" << int(S.Classification) << "\n";
          for (const InclusionDependency &I : D.Inclusion)
            V << "inc\t" << I.HeaderPath << "\t" << int(I.Why) << "\n";
        }
      }
      OS << "R\t" << Path << "\t" << Parent << "\t" << llvm::utohexstr(Hash)
         << "\t" << F.Exit - F.Enter << "\n";
      auto Inside = [&](FileID O) {
        // No file (the command line, Clang itself) is no key of the map.
        if (O.isInvalid())
          return false;
        const FR *G = Tracker.fileReads(O);
        return G && G->Enter >= F.Enter && F.Exit && G->Exit <= F.Exit;
      };
      llvm::StringSet<> Seen;
      // The seventh column is the state the file saw: whether a macro was
      // defined; whether a name was a macro, and its expansion; a
      // declaration's type.
      auto Edge = [&](char Kind, llvm::StringRef Name, FileID O,
                      llvm::StringRef Value) {
        if (O == F.FID)
          return;
        std::string Key = (llvm::Twine(Kind) + Name + "\t" + PathOf(O)).str();
        if (!Seen.insert(Key).second)
          return;
        OS << "E\t" << Path << "\t" << Kind << "\t" << Name << "\t"
           << PathOf(O) << "\t" << (Inside(O) ? "in" : "out") << "\t" << Value
           << "\n";
      };
      for (const auto &E : F.Conditional)
        Edge('C', E.Dep.MacroName, E.Origin, E.Dep.WasDefined ? "1" : "0");
      for (const auto &E : F.Syntactic)
        Edge('S', E.Dep.Name, E.Origin,
             (llvm::Twine(int(E.Dep.Required)) + ":" +
              llvm::utohexstr(llvm::xxh3_64bits(E.Dep.TokenSequence)))
                 .str());
      for (const auto &E : F.Symbol)
        Edge('Y', E.Dep.Name, E.Origin, std::to_string(int(E.Dep.Classification)));
      for (const auto &E : F.Inclusion)
        Edge('I', E.Dep.HeaderPath, E.Origin, std::to_string(int(E.Dep.Why)));
      if (auto It = Decls.Uses.find(F.FID); It != Decls.Uses.end())
        for (const DeclUses::Use &U : It->second)
          Edge(U.Kind, U.Name, U.Origin, U.Value);
      Stack.push_back(&F);
    }
  }

private:
  /// A diagnostic as Clang printed it.
  struct Captured {
    DiagnosticsEngine::Level Level;
    unsigned ID;
    SourceLocation Loc;
    SmallString<128> Message;
    std::vector<CharSourceRange> Ranges;
    std::vector<FixItHint> FixIts;
  };

  /// Where one name stands in a header that pushes or pops it (REPORT.md
  /// Q11). Its definition is one of: the definition it had where the header
  /// began (Start), the Nth one a pop took from below the header (N), or one
  /// the header's own #define or #undef made (Own).
  struct MacroStack {
    static constexpr int Start = 0, Own = -1;
    int Now = Start;
    /// What each of the header's own pushes still on the stack saved.
    std::vector<int> Pushed;
    /// What the header's pops took from below it, in order.
    std::vector<const MacroInfo *> Popped;
  };

  struct Part {
    FileID FID;
    /// CLANG_HEADER_CACHE_LINK: the part's number, and that of the part
    /// whose header was open around it, 0 for none.
    unsigned Id = 0, ParentId = 0;
    /// The copies the part's copy links, each with its #include, and the
    /// regions of the headers inside it linked rather than held.
    std::vector<std::pair<SourceLocation, LinkRecord>> Links;
    /// For each link, the macros its header sets; where what it declares
    /// comes from: the module files loaded for it, and the region of the
    /// header written inside this one; and its place in the metadata.
    std::vector<std::vector<std::string>> LinkSets;
    struct LinkFrom {
      unsigned ModLo = 0, ModHi = 0;
      SourceLocation::UIntTy RegLo = 0, RegHi = 0;
    };
    std::vector<LinkFrom> LinkFroms;
    std::vector<size_t> LinkOrder;
    /// The outside declarations the copies it links refer to by name, which
    /// its own check must cover; and, once written, its own.
    /// Each with the link it is from, by index into Links.
    std::vector<std::pair<int, DeclarationDependency>> LinkOutside;
    std::vector<DeclarationDependency> WrittenDecls;
    size_t ModulesAtEnd = 0;
    std::vector<std::pair<SourceLocation::UIntTy, SourceLocation::UIntTy>>
        LinkedRegions;
    /// Each name the header touched: its directives inside the header,
    /// latest first, for telling its own from a linked header's.
    llvm::DenseMap<const IdentifierInfo *, std::vector<SourceLocation>>
        Directives;
    /// The names whose effect is a linked header's, not the part's.
    llvm::DenseSet<const IdentifierInfo *> LinkedNames;
    /// The module name the part was written under, and its bytes hashed;
    /// empty if not written.
    std::string Written;
    uint64_t WrittenHash = 0;
    /// Copies served inside the header, whose declarations the part holds
    /// as its own (CLANG_HEADER_CACHE_FOLD_SERVED).
    llvm::SmallPtrSet<serialization::ModuleFile *, 4> Folded;
    /// For each file entry of those copies, by its loaded ID, the entry for
    /// the same file made inside the header where the copy was served: a
    /// location in the copy is written as the same offset in it.
    llvm::DenseMap<int, FileID> FoldedEntries;
    /// The #include that entered the header.
    SourceLocation IncludeLoc;
    /// The name the compile opened the header by, and the absolute path the
    /// daemon stores it under.
    std::string Name, Stored;
    /// The offsets of every location made while the header was open, and
    /// the local source-location entries that hold them, from FID's on.
    SourceLocation::UIntTy Begin = 0, End = 0;
    unsigned SLocEnd = 0;
    /// A top-level declaration made before the header began; the header's
    /// own come after it. Null for none.
    const Decl *DeclsAfter = nullptr;
    /// How many module files the compile had loaded when the header began.
    size_t ModulesAtBegin = 0;
    PragmaSnapshot Entry, Exit;
    llvm::SetVector<const IdentifierInfo *> Touched;
    std::vector<MacroEffect> Effects;
    /// Each name the header's #pragma push_macro or pop_macro named.
    llvm::MapVector<const IdentifierInfo *, MacroStack> Stacks;
    std::vector<MacroStackEffect> MacroStacks;
    /// Every file entered while the header was open, the header included,
    /// and each one's size and modification time by real path.
    llvm::DenseSet<const FileEntry *> Files;
    std::map<std::string, FileStamp> Stamps;
    /// Non-empty: why the part is not saved.
    std::string Decline;
    /// The diagnostics Clang printed while the header was open.
    std::vector<Captured> Diagnostics;
    /// Where the warning pragmas stood when the header began, and what
    /// decided the diagnostics it printed (warningState).
    WarningPragmaTracker::Mark PragmasAtBegin;
    uint64_t WarningState = 0;
    /// The warning pragmas the header left in force.
    std::vector<WarningPragma> WarningPragmas;
    /// After the header ended: whether the latest diagnostic was the
    /// header's, so that its notes are too.
    bool TakingNotes = false;

    /// A header still open holds everything made since it began.
    bool contains(SourceLocation::UIntTy Offset) const {
      return Begin <= Offset && (End == 0 || Offset < End);
    }
    /// REPORT.md T17: the submodule each file in the part's region belongs
    /// to, for the writer; files held by the part's own module are absent.
    llvm::DenseMap<FileID, Module *> OwnerOf;
    /// The same by index into the metadata's Submodules, until the modules
    /// are made.
    llvm::DenseMap<FileID, int> SubmoduleOf;
    /// The condition of each region's elements, by file.
    llvm::DenseMap<FileID, uint32_t> CondOfFile;
    /// The submodules made, by index into the metadata's Submodules.
    std::vector<Module *> SubModules;
  };

  /// Puts a diagnostic into \p P, to be printed again at a hit.
  void take(Part &P, DiagnosticsEngine::Level Level, const Diagnostic &Info) {
    if (!P.Decline.empty())
      return;
    // Without the tracker, nothing says the warning flags alone decided what
    // the header printed.
    if (!Pragmas) {
      P.Decline = "it printed a diagnostic, and warning pragmas are not "
                  "followed";
      return;
    }
    // Another compile's IDs for a diagnostic made up at run time differ.
    if (DiagnosticIDs::IsCustomDiag(Info.getID())) {
      P.Decline = "it printed a diagnostic Clang made up at run time";
      return;
    }
    Captured C;
    C.Level = Level;
    C.ID = Info.getID();
    C.Loc = Info.getLocation();
    Info.FormatDiagnostic(C.Message);
    C.Ranges.assign(Info.getRanges().begin(), Info.getRanges().end());
    C.FixIts.assign(Info.getFixItHints().begin(), Info.getFixItHints().end());
    P.Diagnostics.push_back(std::move(C));
  }

  /// \p P holds the parse of \p File's text as it is now.
  void hold(Part &P, FileEntryRef File) {
    if (!P.Files.insert(&File.getFileEntry()).second)
      return;
    std::string Path = CI.getFileManager().getCanonicalName(File).str();
    P.Stamps[Path] = {Path, static_cast<uint64_t>(File.getSize()),
                      static_cast<int64_t>(File.getModificationTime())};
  }

  /// Every local source-location entry of a memory buffer -- the
  /// predefines, scratch space for pasted tokens -- in order: a part keeps
  /// these wherever they lie. Extended as the compile makes entries.
  llvm::ArrayRef<unsigned> memoryBuffers() {
    for (unsigned N = SM.local_sloc_entry_size(); BuffersSeen < N;
         ++BuffersSeen) {
      const SrcMgr::SLocEntry &E = SM.getLocalSLocEntry(BuffersSeen);
      if (E.isFile() && !E.getFile().getContentCache().OrigEntry)
        Buffers.push_back(BuffersSeen);
    }
    return Buffers;
  }

  /// \p II was defined or undefined by \p P's header.
  void touch(Part &P, const IdentifierInfo *II) {
    P.Touched.insert(II);
    if (!P.Stacks.empty())
      if (auto It = P.Stacks.find(II); It != P.Stacks.end())
        It->second.Now = MacroStack::Own;
  }

  MacroStack &stackOf(Part &P, const IdentifierInfo *II) {
    auto [It, New] = P.Stacks.try_emplace(II);
    if (New && P.Touched.contains(II))
      It->second.Now = MacroStack::Own;
    return It->second;
  }

  /// Your rule for push_macro and pop_macro (REPORT.md Q11): what a pop
  /// takes from below the header is a dependency; the pops and pushes that
  /// reach outside it are replayed; a name the header leaves as it found it
  /// is no effect. Fills P.MacroStacks and \p Unchanged, or declines \p P.
  void macroStacks(Part &P,
                   llvm::DenseSet<const IdentifierInfo *> &Unchanged) {
    Preprocessor &PP = CI.getPreprocessor();
    for (auto &[II, S] : P.Stacks) {
      MacroStackEffect E;
      E.Name = II->getName().str();
      for (const MacroInfo *MI : S.Popped) {
        if (!MI) {
          E.Popped.push_back(std::nullopt);
          continue;
        }
        std::optional<std::string> Tokens =
            serializeMacroTokens(*MI, SM, PP.getLangOpts());
        if (!Tokens) {
          P.Decline = "it pops a definition of " + E.Name +
                      " whose text cannot be read";
          return;
        }
        E.Popped.push_back(std::move(*Tokens));
      }
      // A hit pops as many, which leaves the last definition popped, then
      // pushes that definition as often as the header's pushes still on the
      // stack: each must have saved it.
      int Left = S.Popped.size();
      if (llvm::any_of(S.Pushed, [&](int Saved) { return Saved != Left; })) {
        P.Decline = "it pushes a definition of " + E.Name +
                    " a hit cannot push again";
        return;
      }
      E.Pushes = S.Pushed.size();
      if (S.Now == Left)
        Unchanged.insert(II);
      else if (S.Now != MacroStack::Own) {
        P.Decline = "it leaves " + E.Name +
                    " at a definition from outside it a hit cannot give back";
        return;
      }
      if (!E.Popped.empty() || E.Pushes)
        P.MacroStacks.push_back(std::move(E));
    }
  }

  /// Whether \p L, where a macro was used if it is in one, falls in \p P.
  bool inPart(const Part &P, SourceLocation L) const {
    if (L.isInvalid())
      return false;
    SourceLocation::UIntTy Offset = SM.getExpansionLoc(L).getRawEncoding();
    return P.contains(Offset) && !inLinked(P, Offset);
  }

  /// Whether \p Offset is in a header inside \p P that its copy links.
  static bool inLinked(const Part &P, SourceLocation::UIntTy Offset) {
    for (auto [B, E] : P.LinkedRegions)
      if (B <= Offset && Offset < E)
        return true;
    return false;
  }

  /// Makes, at \p Loc, an entry for each file in \p MF, in the order of
  /// MF's entries, each included where MF's entry was; \p Entries maps each
  /// of MF's loaded IDs to the one made for it.
  void entriesFor(serialization::ModuleFile &MF, SourceLocation Loc,
                  llvm::DenseMap<int, FileID> &Entries) {
    for (unsigned I = 0; I != MF.LocalNumSLocEntries; ++I) {
      int ID = MF.SLocEntryBaseID + int(I);
      bool Invalid = false;
      // A loaded entry's ID is -2 less its index (SourceManager).
      const SrcMgr::SLocEntry &E =
          SM.getLoadedSLocEntry(static_cast<unsigned>(-ID - 2), &Invalid);
      if (Invalid || !E.isFile())
        continue;
      const SrcMgr::FileInfo &Info = E.getFile();
      OptionalFileEntryRef FE = Info.getContentCache().OrigEntry;
      if (!FE)
        continue;
      SourceLocation At = Info.getIncludeLoc();
      At = At.isValid() ? translateLoaded(Entries, At) : Loc;
      if (At.isInvalid())
        At = Loc;
      Entries[ID] = SM.createFileID(*FE, At, Info.getFileCharacteristic());
    }
  }

  /// \p L, in a copy folded into a part, as a place in the part's own entry
  /// for its file (\p Entries); invalid if none.
  SourceLocation translateLoaded(const llvm::DenseMap<int, FileID> &Entries,
                                 SourceLocation L) const {
    L = SM.getFileLoc(L);
    if (!SM.isLoadedSourceLocation(L))
      return L;
    auto [FID, Offset] = SM.getDecomposedLoc(L);
    auto It = Entries.find(static_cast<int>(FID.getHashValue()));
    if (It == Entries.end()) {
      if (getenv("DEBUG_DROP"))
        llvm::errs() << "DROP " << SM.getFilename(L) << " fid "
                     << (int)FID.getHashValue() << " off " << Offset << "\n";
      return SourceLocation();
    }
    return SM.getComposedLoc(It->second, Offset);
  }

  /// Whether \p Offset is in a copy served inside \p P and folded into it.
  bool inFolded(const Part &P, SourceLocation::UIntTy Offset) const {
    if (P.Folded.empty())
      return false;
    FileID F = SM.getFileID(SourceLocation::getFromRawEncoding(Offset));
    int ID = static_cast<int>(F.getHashValue());
    for (const serialization::ModuleFile *MF : P.Folded)
      if (ID >= MF->SLocEntryBaseID &&
          ID < MF->SLocEntryBaseID + int(MF->LocalNumSLocEntries))
        return true;
    return false;
  }

  /// Whether the compile made \p D after \p P's header ended. Offsets do
  /// not say: the includer's text after the #include has offsets below the
  /// header's, since a file's offsets are handed out when it is entered.
  bool madeAfter(const Part &P, const Decl *D) const {
    SourceLocation L = D->getLocation();
    if (D->isFromASTFile() || L.isInvalid() || P.IncludeLoc.isInvalid())
      return false;
    L = SM.getExpansionLoc(L);
    if (!SM.isLocalSourceLocation(L) || P.contains(L.getRawEncoding()))
      return false;
    return SM.isBeforeInTranslationUnit(P.IncludeLoc, L);
  }

  /// Module files the compile has loaded. A deep copy cannot be of a
  /// declaration from one, since a part imports nothing.
  size_t modulesLoaded() const {
    if (!CI.getASTReader())
      return 0;
    return CI.getASTReader()->getModuleManager().size();
  }

  void begin(FileID FID, FileEntryRef FE) {
    Part P;
    P.FID = FID;
    P.Id = ++PartsBegun;
    P.ParentId = Open.empty() ? 0 : Open.back().Id;
    P.Name = FE.getName().str();
    P.Stored = absolutePath(CI.getFileManager(), P.Name);
    P.IncludeLoc = SM.getIncludeLoc(FID);
    P.Begin = SM.getSLocEntry(FID).getOffset();
    P.DeclsAfter = LastTopLevel;
    P.ModulesAtBegin = modulesLoaded();
    hold(P, FE);
    if (!CI.hasSema()) {
      P.Decline = "no Sema when the header was entered";
    } else {
      Sema &S = CI.getSema();
      if (!ObserverSet) {
        S.setNameClassificationObserver(Tracker);
        ObserverSet = true;
      }
      P.Entry = capturePragmaState(S);
      if (const char *Pragma = findNonDefaultPragmaState(S))
        P.Decline =
            std::string("pragma state a record cannot carry: ") + Pragma;
    }
    // The warning flags and the pragmas in force here decide what the
    // header prints; a hit compares them (REPORT.md Q34).
    P.WarningState =
        warningState(warningFlagsHash(CI.getDiagnosticOpts()), Pragmas,
                     P.IncludeLoc);
    if (Pragmas)
      P.PragmasAtBegin = Pragmas->mark();
    // A part refers to a declaration from a loaded copy by name; it cannot
    // hold a deep copy of one.
    if (P.ModulesAtBegin && !ByName && P.Decline.empty())
      P.Decline = "a saved copy was loaded before the header";
    Tracked.insert(FID);
    Open.push_back(std::move(P));
  }

  void end() {
    Part P = Open.pop_back_val();
    P.End = SM.getNextLocalOffset();
    P.ModulesAtEnd = modulesLoaded();
    P.SLocEnd = SM.local_sloc_entry_size();
    Preprocessor &PP = CI.getPreprocessor();
    llvm::DenseSet<const IdentifierInfo *> Unchanged;
    if (!P.Stacks.empty() && P.Decline.empty())
      macroStacks(P, Unchanged);
    for (const IdentifierInfo *II : P.Touched)
      if (!Unchanged.contains(II))
        P.Effects.push_back(
            {II->getName().str(),
             PP.isMacroDefined(const_cast<IdentifierInfo *>(II))});
    if (LinkMode)
      for (const IdentifierInfo *II : P.Touched) {
        std::vector<SourceLocation> &Dirs = P.Directives[II];
        for (const MacroDirective *MD = PP.getLocalMacroDirectiveHistory(II);
             MD; MD = MD->getPrevious())
          if (MD->getLocation().isValid() &&
              P.contains(
                  SM.getExpansionLoc(MD->getLocation()).getRawEncoding()))
            Dirs.push_back(MD->getLocation());
      }
    if (CI.hasSema()) {
      Sema &S = CI.getSema();
      P.Exit = capturePragmaState(S);
      if (const char *Pragma = findNonDefaultPragmaState(S))
        if (P.Decline.empty())
          P.Decline =
              std::string("pragma state a record cannot carry: ") + Pragma;
    }
    // A hit applies again the warning pragmas the header left in force; it
    // cannot leave a push unpopped, or pop what the includer pushed.
    if (Pragmas && P.Decline.empty()) {
      if (std::optional<std::vector<WarningPragma>> Left =
              Pragmas->since(P.PragmasAtBegin))
        P.WarningPragmas = std::move(*Left);
      else
        P.Decline = "it leaves a warning pragma pushed, or pops one";
    }
    // Unless the part folds it in (FoldServed), or links it.
    if (modulesLoaded() > P.ModulesAtBegin && P.Decline.empty() &&
        !LinkMode) {
      serialization::ModuleManager &MM = CI.getASTReader()->getModuleManager();
      for (size_t I = P.ModulesAtBegin, E = MM.size(); I != E; ++I)
        if (!P.Folded.count(&MM[I])) {
          P.Decline = "a saved copy was loaded inside the header";
          break;
        }
    }
    Ready.push_back(std::move(P));
  }

  void writeReady() {
    if (Ready.empty())
      return;
    auto Start = std::chrono::steady_clock::now();
    llvm::TimeTraceScope Scope("HeaderCache Export");
    std::vector<Part> Parts;
    Parts.swap(Ready);
    Batch = &Parts;
    for (Part &P : Parts) {
      write(P);
      if (LinkMode)
        handOver(P);
    }
    Batch = nullptr;
    Counts.Micros += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - Start)
                         .count();
  }

  /// The part numbered \p Id, open, waiting, or being written; null if
  /// it was written already.
  Part *findPart(unsigned Id) {
    for (Part &P : Open)
      if (P.Id == Id)
        return &P;
    for (Part &P : Ready)
      if (P.Id == Id)
        return &P;
    if (Batch)
      for (Part &P : *Batch)
        if (P.Id == Id)
          return &P;
    return nullptr;
  }

  /// CLANG_HEADER_CACHE_LINK: once \p P is written, the part around it
  /// links P's copy rather than hold P's header. If P was not written, or
  /// pushes or pops a macro, which both would replay, the part around it
  /// holds P's header and links what P would have.
  void handOver(Part &P) {
    Part *Parent = P.ParentId ? findPart(P.ParentId) : nullptr;
    if (!Parent)
      return;
    // A link loads before the lines of the copy linking it, so it cannot
    // name a declaration they make: git's repository.h's
    // `struct object_id;`, after hash.h's definition, inside hash.h.
    bool TakesParents = llvm::any_of(P.WrittenDecls,
                                     [&](const DeclarationDependency &D) {
      return ownLinesDeclare(*Parent,
                             D.In == DeclarationDependency::Space::Tag, D.Name);
    });
    if (getenv("DEBUG_LINK")) {
      llvm::errs() << "HANDOVER " << P.Stored << " -> " << Parent->Stored
                   << " written=" << !P.Written.empty()
                   << " takesparents=" << TakesParents << " decls:";
      for (const DeclarationDependency &D : P.WrittenDecls)
        llvm::errs() << " " << D.Name;
      llvm::errs() << "\n";
    }
    if (!P.Written.empty() && P.Stacks.empty() && !TakesParents) {
      std::string Guard = guardOf(P.FID);
      Parent->Links.push_back(
          {P.IncludeLoc,
           {P.Stored, P.Written, Guard,
            !Guard.empty() && independentOf(P, *Parent, Guard), {},
            P.WrittenHash}});
      Parent->LinkSets.emplace_back();
      for (const MacroEffect &E : P.Effects)
        Parent->LinkSets.back().push_back(E.Name);
      Parent->LinkFroms.push_back(
          {static_cast<unsigned>(P.ModulesAtBegin),
           static_cast<unsigned>(P.ModulesAtEnd), P.Begin, P.End});
      Parent->LinkedRegions.push_back({P.Begin, P.End});
      for (const DeclarationDependency &D : P.WrittenDecls)
        Parent->LinkOutside.push_back({int(Parent->Links.size()) - 1, D});
      ++Counts.LinksWritten;
    } else {
      int Offset = Parent->Links.size();
      for (const auto &[From, D] : P.LinkOutside)
        Parent->LinkOutside.push_back({Offset + From, D});
      for (size_t K = 0; K != P.Links.size(); ++K) {
        auto &[Loc, L] = P.Links[K];
        // Skippable inside P; the parent's lines before P are not checked.
        L.OnGuard = false;
        Parent->Links.push_back({Loc, std::move(L)});
        Parent->LinkSets.push_back(std::move(P.LinkSets[K]));
        Parent->LinkFroms.push_back(P.LinkFroms[K]);
      }
      Parent->LinkedRegions.insert(Parent->LinkedRegions.end(),
                                   P.LinkedRegions.begin(),
                                   P.LinkedRegions.end());
    }
    // What a linked header printed, its copy prints again.
    llvm::erase_if(Parent->Diagnostics, [&](const Captured &C) {
      return C.Loc.isValid() && SM.isLocalSourceLocation(C.Loc) &&
             inLinked(*Parent,
                      SM.getExpansionLoc(C.Loc).getRawEncoding());
    });
  }

  /// The include guard of the file entered as \p FID; empty for none.
  std::string guardOf(FileID FID) {
    OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
    if (!FE)
      return "";
    HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();
    if (const IdentifierInfo *G =
            HS.getFileInfo(*FE).getControllingMacro(HS.getExternalLookup()))
      return G->getName().str();
    return "";
  }

  /// Whether \p P's header read nothing that \p Parent's lines before it
  /// set, changed no macro they set, and its guard \p Guard was not tested
  /// there first (as compose decides for a submodule, REPORT.md T17).
  bool independentOf(const Part &P, const Part &Parent,
                     llvm::StringRef Guard) {
    using FR = ASTCacheDependencyTracker::FileReads;
    const FR *Root = Tracker.fileReads(Parent.FID);
    const FR *Self = Tracker.fileReads(P.FID);
    if (!Root || !Self || !Self->Exit)
      return false;
    auto Within = [&](const FR &F) {
      return Self->Enter <= F.Enter && F.Exit && F.Exit <= Self->Exit;
    };
    auto Before = [&](FileID O) {
      if (O.isInvalid())
        return false;
      const FR *F = Tracker.fileReads(O);
      return F && F->Enter >= Root->Enter && F->Enter < Self->Enter &&
             !Within(*F);
    };
    for (const FR &F : Tracker.allFileReads()) {
      if (F.Enter < Root->Enter)
        continue;
      if (F.Enter < Self->Enter) {
        for (const auto &E : F.Conditional)
          if (E.Dep.MacroName == Guard && E.Seq < Self->Enter)
            return false;
        for (const auto &E : F.Syntactic)
          if (E.Dep.Name == Guard && E.Seq < Self->Enter)
            return false;
        continue;
      }
      if (!Within(F))
        continue;
      auto Reads = [&](const auto &V) {
        return llvm::any_of(V, [&](const auto &E) { return Before(E.Origin); });
      };
      if (Reads(F.Conditional) || Reads(F.Syntactic) || Reads(F.Symbol) ||
          Reads(F.Inclusion) ||
          llvm::any_of(F.Changes, [&](const FR::Change &C) {
            return Before(C.Replaced);
          }))
        return false;
    }
    return true;
  }

  /// CLANG_HEADER_CACHE_LINK: the links \p P's copy carries, in the order
  /// of their #includes, and what they take from its metadata: the effects
  /// their own directives made, and the guard test of each skipped where
  /// the header was read. Returns why the part cannot be written, or "".
  std::string takeLinks(Part &P, HeaderDependencySet &Deps) {
    if (P.Links.empty())
      return "";
    Preprocessor &PP = CI.getPreprocessor();
    auto Linked = [&](SourceLocation L) {
      return inLinked(P, SM.getExpansionLoc(L).getRawEncoding());
    };
    // A name whose latest directive in the header is a linked header's is
    // that header's effect, which its copy replays. The links replay first,
    // so the part's own directive before it would win wrongly.
    for (auto &[II, Dirs] : P.Directives) {
      if (Dirs.empty() || !Linked(Dirs.front()))
        continue;
      if (!llvm::all_of(Dirs, Linked))
        return "a header it links changes a macro its lines set before: " +
               II->getName().str();
      P.LinkedNames.insert(II);
    }
    llvm::erase_if(Deps.MacroEffects, [&](const MacroEffect &E) {
      return P.LinkedNames.contains(PP.getIdentifierInfo(E.Name));
    });
    // Where a link is skipped the compile's own reading of its header
    // stands in, so the part's lines must have read nothing the header
    // sets: a macro they expanded or tested would be the copy's, made with
    // the link loaded.
    llvm::StringSet<> Read;
    using FR = ASTCacheDependencyTracker::FileReads;
    if (const FR *Root = Tracker.fileReads(P.FID))
      for (const FR &F : Tracker.allFileReads()) {
        if (F.Enter < Root->Enter || (Root->Exit && F.Enter > Root->Exit))
          continue;
        if (inLinked(P, SM.getSLocEntry(F.FID).getOffset()))
          continue;
        for (const auto &E : F.Conditional)
          Read.insert(E.Dep.MacroName);
        for (const auto &E : F.Syntactic)
          Read.insert(E.Dep.Name);
      }
    else
      for (auto &[Loc, L] : P.Links)
        L.OnGuard = false;
    for (size_t K = 0; K != P.Links.size(); ++K)
      if (P.Links[K].second.OnGuard &&
          llvm::any_of(P.LinkSets[K],
                       [&](const std::string &N) { return Read.contains(N); }))
        P.Links[K].second.OnGuard = false;
    std::vector<size_t> Order(P.Links.size());
    for (size_t K = 0; K != Order.size(); ++K)
      Order[K] = K;
    std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
      return SM.isBeforeInTranslationUnit(P.Links[A].first, P.Links[B].first);
    });
    llvm::StringSet<> Guards;
    P.LinkOrder.assign(P.Links.size(), 0);
    for (size_t K : Order) {
      const LinkRecord &L = P.Links[K].second;
      P.LinkOrder[K] = Deps.Links.size();
      if (getenv("DEBUG_LINK"))
        llvm::errs() << "WRITE " << P.Stored << " links " << L.Path << " "
                     << L.Module << (L.OnGuard ? " on-guard " : " always ")
                     << L.Guard << "\n";
      Deps.Links.push_back(L);
      if (L.OnGuard)
        Guards.insert(L.Guard);
    }
    // Where the guard is defined the link is skipped, as the #include would
    // be: the header's test of its own guard is no dependency of the copy.
    llvm::erase_if(Deps.Conditional, [&](const ConditionalDependency &D) {
      return !D.WasDefined && Guards.contains(D.MacroName);
    });
    return "";
  }

  /// The link of \p P that declares \p ND, by index into P.Links; -1 if
  /// none does.
  int linkOf(const Part &P, const NamedDecl &ND) const {
    for (size_t K = 0; K != P.LinkFroms.size(); ++K) {
      const Part::LinkFrom &F = P.LinkFroms[K];
      if (ND.isFromASTFile()) {
        const serialization::ModuleFile *MF =
            CI.getASTReader()->getOwningModuleFile(&ND);
        if (MF && MF->Index >= F.ModLo && MF->Index < F.ModHi)
          return K;
        continue;
      }
      SourceLocation L = ND.getLocation();
      if (L.isValid() && SM.isLocalSourceLocation(L)) {
        SourceLocation::UIntTy At = SM.getExpansionLoc(L).getRawEncoding();
        if (F.RegLo <= At && At < F.RegHi)
          return K;
      }
    }
    return -1;
  }

  /// Where the declaration \p Name (a tag if \p Tag) found now is, for
  /// \p P: in its link of that index, in its own lines (-2), or outside it
  /// (-1, also for none).
  int whereIs(const Part &P, bool Tag, llvm::StringRef Name) {
    Preprocessor &PP = CI.getPreprocessor();
    IdentifierInfo *II = PP.getIdentifierInfo(Name);
    PP.refreshIdentifier(*II);
    NamedDecl *D = ASTReader::lookupAtFileScope(CI.getSema(), *II, Tag,
                                                /*CreateBuiltin=*/false);
    if (!D)
      return -1;
    if (int K = linkOf(P, *D); K >= 0)
      return K;
    if (D->isFromASTFile()) {
      const serialization::ModuleFile *MF =
          CI.getASTReader()->getOwningModuleFile(D);
      return MF && MF->Index >= P.ModulesAtBegin ? -2 : -1;
    }
    return inPart(P, D->getLocation()) ? -2 : -1;
  }

  /// The names \p P's own lines declare at file scope, for a copy that
  /// links this one and skips it (HeaderDependencySet::Declares).
  void declares(const Part &P, std::vector<std::string> &Out) {
    llvm::StringSet<> Seen;
    auto Add = [&](const NamedDecl *ND) {
      if (!ND->getIdentifier())
        return;
      std::string N = (isa<TagDecl>(ND) ? "t:" : "o:") + ND->getName().str();
      if (Seen.insert(N).second)
        Out.push_back(std::move(N));
    };
    ASTContext &Ctx = CI.getASTContext();
    Decl *First =
        P.DeclsAfter
            ? const_cast<Decl *>(P.DeclsAfter)->getNextDeclInContext()
            : *Ctx.getTranslationUnitDecl()->noload_decls_begin();
    for (Decl *D = First; D; D = D->getNextDeclInContext()) {
      if (D->isFromASTFile() || !inPart(P, D->getLocation()))
        continue;
      if (auto *ND = dyn_cast<NamedDecl>(D))
        Add(ND);
      if (auto *ED = dyn_cast<EnumDecl>(D))
        for (const EnumConstantDecl *E : ED->enumerators())
          Add(E);
    }
  }

  /// Whether a declaration of \p Name (a tag if \p Tag) is in \p P's own
  /// lines, the latest or one it redeclares.
  bool ownLinesDeclare(const Part &P, bool Tag, llvm::StringRef Name) {
    Preprocessor &PP = CI.getPreprocessor();
    IdentifierInfo *II = PP.getIdentifierInfo(Name);
    PP.refreshIdentifier(*II);
    NamedDecl *D = ASTReader::lookupAtFileScope(CI.getSema(), *II, Tag,
                                                /*CreateBuiltin=*/false);
    for (Decl *R = D; R; R = R->getPreviousDecl()) {
      if (R->isFromASTFile()) {
        const serialization::ModuleFile *MF =
            CI.getASTReader()->getOwningModuleFile(R);
        if (MF && MF->Index >= P.ModulesAtBegin &&
            linkOf(P, *cast<NamedDecl>(R)) < 0)
          return true;
      } else if (inPart(P, R->getLocation())) {
        return true;
      }
    }
    return false;
  }

  /// As whereIs, for the declaration of \p Name the link \p From of \p P
  /// found: the latest made before it, not its own or one after it.
  int whereIsBefore(const Part &P, bool Tag, llvm::StringRef Name, int From) {
    Preprocessor &PP = CI.getPreprocessor();
    IdentifierInfo *II = PP.getIdentifierInfo(Name);
    PP.refreshIdentifier(*II);
    NamedDecl *D = ASTReader::lookupAtFileScope(CI.getSema(), *II, Tag,
                                                /*CreateBuiltin=*/false);
    SourceLocation At = From >= 0 && From < (int)P.Links.size()
                            ? P.Links[From].first
                            : SourceLocation();
    for (Decl *R = D; R; R = R->getPreviousDecl()) {
      auto *ND = cast<NamedDecl>(R);
      int K = linkOf(P, *ND);
      if (K >= 0 && K >= From)
        continue;
      if (K >= 0)
        return K;
      if (R->isFromASTFile()) {
        const serialization::ModuleFile *MF =
            CI.getASTReader()->getOwningModuleFile(R);
        if (MF && MF->Index >= P.ModulesAtBegin)
          return -2;
        return -1;
      }
      if (!inPart(P, R->getLocation()))
        return -1;
      if (At.isValid() && SM.isBeforeInTranslationUnit(At, R->getLocation()))
        continue;
      return -2;
    }
    return -1;
  }

  /// Whether \p ND, which \p P's lines name, is a linked header's.
  bool linkedDecl(const Part &P, const NamedDecl &ND) const {
    if (!LinkMode)
      return false;
    if (ND.isFromASTFile()) {
      const serialization::ModuleFile *MF =
          CI.getASTReader()->getOwningModuleFile(&ND);
      return MF && MF->Index >= P.ModulesAtBegin;
    }
    SourceLocation L = ND.getLocation();
    return L.isValid() && SM.isLocalSourceLocation(L) &&
           inLinked(P, SM.getExpansionLoc(L).getRawEncoding());
  }

  void decline(const Part &P, const llvm::Twine &Why) {
    ++Counts.Declined;
    Counts.Declines.push_back(P.Stored + ": " + Why.str());
  }

  void write(Part &P) {
    if (!P.Decline.empty())
      return decline(P, P.Decline);
    if (std::string Why = diagnosticsOutside(P); !Why.empty())
      return decline(P, Why);
    // After an error nothing is written: Clang marks a module written then as
    // holding errors, and a compile offered it refuses it with an error of
    // its own (REPORT.md Q35). A warning that -Werror made an error is not
    // one here, as for Clang: the declarations are what they would have been.
    if (CI.getDiagnostics().hasUncompilableErrorOccurred())
      return decline(P, "the compile has an error");
    const HeaderDependencySet *Rec = Tracker.recordFor(P.FID);
    if (!Rec)
      return decline(P, "the dependency tracker could not describe it");
    if (!CI.hasSema())
      return decline(P, "no Sema");

    std::optional<llvm::TimeTraceScope> DepsScope;
    DepsScope.emplace("HeaderCache ExportMetadata");
    HeaderDependencySet Deps = *Rec;
    Deps.Entry = P.Entry;
    Deps.Exit = P.Exit;
    Deps.MacroEffects = P.Effects;
    Deps.MacroStacks = P.MacroStacks;
    Deps.HeaderName = P.Name;
    for (auto &[Path, Stamp] : P.Stamps)
      Deps.Files.push_back(Stamp);

    // What the copies it links refer to: outside the header, the copy's own
    // check covers it, as a hit loads them unchecked; in another of its
    // links, a skip of that link must find it; in its own lines, which load
    // after the links, nothing can stand for it (postgres's pg_list.h,
    // handed over from a header not linked, after bitmapset.h's
    // `struct List;`), and the part is not written.
    std::vector<DeclarationDependency> LinkNeeds;
    if (LinkMode) {
      if (std::string Why = takeLinks(P, Deps); !Why.empty())
        return decline(P, Why);
      for (const auto &[From, D] : P.LinkOutside) {
        bool Tag = D.In == DeclarationDependency::Space::Tag;
        int K = whereIsBefore(P, Tag, D.Name, From);
        if (getenv("DEBUG_LINK"))
          llvm::errs() << "OUTSIDE " << P.Stored << " " << (Tag ? "tag " : "")
                       << D.Name << " from link " << From << " -> " << K << "\n";
        if (K == -2)
          return decline(P, "a copy it links names a declaration of its own "
                            "lines: " + D.Name);
        if (K >= 0) {
          std::vector<std::string> &Names = Deps.Links[P.LinkOrder[K]].Names;
          std::string N = (Tag ? "t:" : "o:") + D.Name;
          if (!llvm::is_contained(Names, N))
            Names.push_back(std::move(N));
        } else if (!llvm::is_contained(LinkNeeds, D)) {
          LinkNeeds.push_back(D);
        }
      }
    }

    // REPORT.md T17: the headers inside held as submodules, and the
    // dependencies put together per submodule.
    auto Composing = std::chrono::steady_clock::now();
    if (!LinkMode)
      compose(P, Deps);
    Counts.ComposeMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - Composing)
                                .count();

    // The same header, metadata and compiler options give the same name in
    // any compile, which lets the daemon recognise a part it already holds.
    llvm::SmallVector<char, 256> Encoded;
    encodeMetadata(Deps, /*ModuleName=*/{}, Encoded);
    std::string Key = P.Stored;
    Key += '\0';
    Key += CI.getInvocation().getModuleHash();
    Key += '\0';
    Key.append(Encoded.begin(), Encoded.end());
    std::string Name =
        "__hcp_" + llvm::utohexstr(llvm::xxh3_64bits(Key), /*LowerCase=*/true,
                                   /*Width=*/16);

    if (getenv("DEBUG_LINK"))
      llvm::errs() << "PART " << P.Stored << " " << Name << "\n";
    // The first time a copy would be written, it is not: most copies are of
    // a state no later compile reaches (copies never served).
    if (SeenBefore && !SeenBefore(P.Stored, Name)) {
      ++Counts.HeldBack;
      return;
    }
    Preprocessor &PP = CI.getPreprocessor();
    ModuleMap &MM = PP.getHeaderSearchInfo().getModuleMap();
    if (MM.findModule(Name))
      return decline(P, "the same part was already exported in this compile");
    Module *M = MM.createModule(Name, /*Parent=*/nullptr,
                                /*IsFramework=*/false, /*IsExplicit=*/false);
    M->PresumedModuleMapFile = "<header-cache-part>";
    // The headers held as submodules, each inside its parent's.
    // Explicit: a module made visible makes visible each submodule that is
    // not (Module::getExportedModules), and a skipped one must stay hidden.
    std::vector<Module *> Subs;
    for (const SubmoduleRecord &R : Deps.Submodules)
      Subs.push_back(MM.createModule(R.Name, R.Parent < 0 ? M : Subs[R.Parent],
                                     /*IsFramework=*/false,
                                     /*IsExplicit=*/true));
    for (auto &[F, K] : P.SubmoduleOf)
      P.OwnerOf[F] = Subs[K];
    P.SubModules = Subs;

    // A variable definition that must be emitted -- postgres's kwlist_d.h
    // defines `const ScanKeywordList ScanKeywords = {...}` -- reaches the
    // object of a compile that loads the part the way a module's does: as an
    // initializer of the module, emitted where it is imported. Clang lists a
    // module's initializers only for a module it builds, which a part is not.
    ASTContext &Ctx = CI.getASTContext();
    Decl *First =
        P.DeclsAfter
            ? const_cast<Decl *>(P.DeclsAfter)->getNextDeclInContext()
            : *Ctx.getTranslationUnitDecl()->noload_decls_begin();
    for (Decl *D = First; D; D = D->getNextDeclInContext())
      if (auto *VD = dyn_cast<VarDecl>(D))
        if (VD->isFileVarDecl() &&
            VD->isThisDeclarationADefinition() == VarDecl::Definition &&
            inPart(P, VD->getLocation()) && Ctx.DeclMustBeEmitted(VD))
          Ctx.addModuleInitializer(M, VD);

    DepsScope.reset();
    Written W = writePart(P, M, ByName);
    if (ByName && !W.Failure.empty()) {
      if (P.ModulesAtBegin || !P.Links.empty())
        return decline(P, "cannot refer by name to " + W.Failure);
      std::string Why = std::move(W.Failure);
      W = writePart(P, M, /*ByName=*/false);
      ++Counts.Copied;
      Counts.Copies.push_back(P.Stored + ": " + Why);
    }
    Deps.Declarations = std::move(W.Declarations);
    auto TakeName = [&](size_t K, std::string Name) {
      std::vector<std::string> &Names = Deps.Links[P.LinkOrder[K]].Names;
      if (!llvm::is_contained(Names, Name))
        Names.push_back(std::move(Name));
    };
    for (auto &[K, Name] : W.LinkNames)
      TakeName(K, std::move(Name));
    for (DeclarationDependency &D : LinkNeeds)
      if (!llvm::is_contained(Deps.Declarations, D))
        Deps.Declarations.push_back(std::move(D));
    P.WrittenDecls = Deps.Declarations;
    if (LinkMode)
      declares(P, Deps.Declares);
    provide(P, Deps, W.Cross);
    if (!W.DiagnosticFailure.empty())
      return decline(P, W.DiagnosticFailure);
    Deps.Diagnostics = std::move(W.Diagnostics);
    if (!P.CondOfFile.empty())
      for (size_t K = 0; K != Deps.Diagnostics.size(); ++K)
        Deps.Diagnostics[K].Cond = condAt(P, P.Diagnostics[K].Loc);
    Deps.WarningState = P.WarningState;
    Deps.WarningPragmas = P.WarningPragmas;

    PushRequest R;
    R.Filename = P.Stored;
    R.ModuleName = Name;
    R.Deps = std::move(Deps);
    R.Data.assign(W.Buffer.begin(), W.Buffer.end());
    ++Counts.Exported;
    std::string Line = P.Stored + ": " + std::to_string(W.Buffer.size()) +
                       " bytes in " + std::to_string(W.Micros) + " us, " +
                       std::to_string(W.ByName) + " by name";
    if (!R.Deps.Diagnostics.empty())
      Line += ", " + std::to_string(R.Deps.Diagnostics.size()) +
              " diagnostics to print again";
    if (W.DroppedLocations)
      Line += ", " + std::to_string(W.DroppedLocations) +
              " locations dropped (in module files or outside the part)";
    Counts.Exports.push_back(std::move(Line));
    P.Written = Name;
    P.WrittenHash = llvm::xxh3_64bits(
        llvm::StringRef(R.Data.data(), R.Data.size()));
    if (Tracker.measuringIndependence())
      measureIndependence(P, Name, W.Buffer.size());
    dump(P, R, W.Sizes);
    llvm::TimeTraceScope PushScope("HeaderCache Push");
    Deliver(std::move(R));
  }

  /// A declaration of a part's submodule that the part's lines outside it
  /// refer to, with the part's local index of it
  /// (ASTWriter::getPartCrossReferences).
  struct CrossReference {
    const Decl *D;
    Module *From, *To;
    uint32_t Index;
  };

  /// REPORT.md T17. What \p P's lines outside each submodule refer to in
  /// it (\p Cross, as the writer found it), by name and with the submodule
  /// owning it: where that submodule is skipped, the compile's own
  /// declaration of the name stands for each of these, and the reader never
  /// reads the copy's.
  void provide(const Part &P, HeaderDependencySet &Deps,
               llvm::ArrayRef<CrossReference> Cross) {
    llvm::DenseMap<std::pair<const NamedDecl *, int>, size_t> Entry;
    for (const CrossReference &C : Cross) {
      auto Sub = llvm::find(P.SubModules, C.To);
      if (Sub == P.SubModules.end())
        continue;
      int K = Sub - P.SubModules.begin();
      ProvidedDeclaration::Use U;
      U.Index = C.Index;
      const NamedDecl *E = nullptr;
      if (isa<FieldDecl, IndirectFieldDecl>(C.D)) {
        const auto *Member = cast<NamedDecl>(C.D);
        const auto *R = dyn_cast<RecordDecl>(C.D->getDeclContext());
        if (R && Member->getIdentifier()) {
          E = R->getIdentifier() ? static_cast<const NamedDecl *>(R)
                                 : R->getTypedefNameForAnonDecl();
          U.Of = ProvidedDeclaration::How::Member;
          U.Member = Member->getName().str();
        }
      } else if (const auto *TD = dyn_cast<TagDecl>(C.D);
                 TD && !TD->getIdentifier()) {
        E = TD->getTypedefNameForAnonDecl();
        U.Of = ProvidedDeclaration::How::TagOfTypedef;
      } else if (const auto *ND = dyn_cast<NamedDecl>(C.D);
                 ND && (isa<EnumConstantDecl>(ND) ||
                        ND->getDeclContext()->getRedeclContext()->isFileContext())) {
        E = ND;
      }
      // One without a name the reader can find is read as it is.
      if (!E || !E->getIdentifier())
        continue;
      auto [It, New] = Entry.try_emplace({E, K}, Deps.Provided.size());
      if (New) {
        ProvidedDeclaration D;
        D.Decl.In = nameSpaceOf(*E);
        D.Decl.Name = E->getName().str();
        // The submodule owning it, plus one: Conditions[K] is where it loads.
        D.Decl.Cond = K + 1;
        Deps.Provided.push_back(std::move(D));
      }
      Deps.Provided[It->second].Uses.push_back(std::move(U));
    }
  }

  /// The condition of what \p P's text holds at \p L: its submodule's.
  uint32_t condAt(const Part &P, SourceLocation L) const {
    if (L.isInvalid() || !SM.isLocalSourceLocation(L))
      return 0;
    auto It = P.CondOfFile.find(SM.getFileID(SM.getExpansionLoc(L)));
    return It == P.CondOfFile.end() ? 0 : It->second;
  }

  /// REPORT.md T17. Chooses the headers inside \p P to hold as submodules
  /// of its copy: each guarded (or #pragma once) header that begins between
  /// declarations, leaves the pragmas as it found them, reads nothing the
  /// part set before it -- in the header around it or in any header read
  /// before it -- changes no macro the part set before it, and whose guard
  /// nothing in the part tested before it. Such a header depends on nothing
  /// of the part's, so the copy keeps the folded part's dependencies, less
  /// the header's own guard test: where the compile has read the header,
  /// the guard says to skip its submodule instead. What the header leaves
  /// its macros as, and the warnings it printed, are its submodule's.
  void compose(Part &P, HeaderDependencySet &Deps) {
    auto LapStart = std::chrono::steady_clock::now();
    auto Lap = [&](uint64_t &To) {
      auto Now = std::chrono::steady_clock::now();
      To += std::chrono::duration_cast<std::chrono::microseconds>(Now - LapStart)
                .count();
      LapStart = Now;
    };
    using FR = ASTCacheDependencyTracker::FileReads;
    const FR *Root = Tracker.fileReads(P.FID);
    if (!Root || !P.Stacks.empty() || !Root->Exit)
      return;
    // The files read while the header was open, in the order entered.
    std::vector<const FR *> All;
    for (const FR &F : Tracker.allFileReads())
      if (F.Enter >= Root->Enter && F.Exit && F.Exit <= Root->Exit)
        All.push_back(&F);
    if (All.empty() || All[0] != Root)
      return;
    llvm::DenseMap<FileID, size_t> Index;
    std::vector<int> Parent(All.size(), -1);
    {
      std::vector<size_t> Stack;
      for (size_t I = 0; I != All.size(); ++I) {
        while (!Stack.empty() && All[Stack.back()]->Exit < All[I]->Enter)
          Stack.pop_back();
        Parent[I] = Stack.empty() ? -1 : static_cast<int>(Stack.back());
        Stack.push_back(I);
        Index[All[I]->FID] = I;
      }
    }
    auto Within = [&](size_t I, size_t Of) {
      return All[Of]->Enter <= All[I]->Enter && All[I]->Exit <= All[Of]->Exit;
    };

    // The candidates, and the first time anything in the part tested each
    // one's guard.
    HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();
    std::vector<std::optional<SubmoduleRecord>> Candidate(All.size());
    llvm::StringMap<uint64_t> FirstTest;
    for (size_t I = 1; I != All.size(); ++I) {
      auto N = NestedAt.find(All[I]->FID);
      if (N == NestedAt.end() || !N->second.AtBoundary || !N->second.Neutral)
        continue;
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(All[I]->FID);
      if (!FE)
        continue;
      HeaderFileInfo &Info = HS.getFileInfo(*FE);
      SubmoduleRecord Rec;
      if (const IdentifierInfo *G =
              Info.getControllingMacro(HS.getExternalLookup()))
        Rec.Guard = G->getName().str();
      else if (!Info.isPragmaOnce)
        continue;
      // Where the reader finds which declarations it holds.
      Rec.Path = absolutePath(CI.getFileManager(), FE->getName());
      if (!Rec.Guard.empty())
        FirstTest.try_emplace(Rec.Guard, UINT64_MAX);
      Candidate[I] = std::move(Rec);
    }
    if (!FirstTest.empty())
      for (const FR *F : All) {
        auto Note = [&](llvm::StringRef Name, uint64_t Seq) {
          auto It = FirstTest.find(Name);
          if (It != FirstTest.end() && Seq < It->second)
            It->second = Seq;
        };
        for (const auto &E : F->Conditional)
          Note(E.Dep.MacroName, E.Seq);
        for (const auto &E : F->Syntactic)
          Note(E.Dep.Name, E.Seq);
      }

    // Choose, outermost first: each file belongs to its nearest chosen
    // enclosing submodule, -1 for the part's own module.
    std::vector<int> Region(All.size(), -1);
    std::vector<SubmoduleRecord> Subs;
    for (size_t I = 1; I != All.size(); ++I) {
      int R = Parent[I] < 0 ? -1 : Region[Parent[I]];
      Region[I] = R;
      if (!Candidate[I])
        continue;
      SubmoduleRecord &Rec = *Candidate[I];
      if (!Rec.Guard.empty() && FirstTest.lookup(Rec.Guard) < All[I]->Enter)
        continue;
      // A file of the part entered before this header and not inside it.
      auto Before = [&](FileID O) {
        if (O.isInvalid())
          return false;
        auto J = Index.find(O);
        if (J == Index.end())
          return false;
        size_t G = J->second;
        return !Within(G, I) && All[G]->Enter < All[I]->Enter;
      };
      bool Qualifies = true;
      for (size_t J = I; J != All.size() && Within(J, I) && Qualifies; ++J) {
        const FR &F = *All[J];
        auto Reads = [&](const auto &V) {
          for (const auto &E : V)
            if (Before(E.Origin))
              Qualifies = false;
        };
        Reads(F.Conditional);
        Reads(F.Syntactic);
        Reads(F.Symbol);
        Reads(F.Inclusion);
        for (const FR::Change &C : F.Changes)
          if (Before(C.Replaced))
            Qualifies = false;
      }
      if (!Qualifies)
        continue;
      Rec.Name = "s" + std::to_string(Subs.size());
      Rec.Parent = R;
      Subs.push_back(std::move(Rec));
      Region[I] = static_cast<int>(Subs.size()) - 1;
    }
    Lap(Counts.ChooseMicros);
    if (Subs.empty())
      return;

    // The folded part's dependencies, less each submodule's guard test,
    // which only the header itself made.
    llvm::StringSet<> Guards;
    for (const SubmoduleRecord &S : Subs)
      if (!S.Guard.empty())
        Guards.insert(S.Guard);
    llvm::erase_if(Deps.Conditional, [&](const ConditionalDependency &D) {
      return !D.WasDefined && Guards.contains(D.MacroName);
    });

    // Condition K + 1 holds where submodule K loads: what the header leaves
    // its macros as is replayed, and its warnings printed, only there.
    Deps.Conditions.clear();
    for (size_t K = 0; K != Subs.size(); ++K)
      Deps.Conditions.push_back({static_cast<int32_t>(K), -1});
    auto CondOf = [](int R) { return R < 0 ? 0u : uint32_t(R) + 1; };
    auto RegionOf = [&](SourceLocation L) {
      if (L.isInvalid() || !SM.isLocalSourceLocation(L))
        return -1;
      auto J = Index.find(SM.getFileID(SM.getExpansionLoc(L)));
      return J == Index.end() ? -1 : Region[J->second];
    };
    // A macro the header left changed belongs to the submodule of the last
    // directive it made.
    Preprocessor &PP = CI.getPreprocessor();
    for (MacroEffect &E : Deps.MacroEffects) {
      const MacroDirective *MD =
          PP.getLocalMacroDirectiveHistory(PP.getIdentifierInfo(E.Name));
      while (MD && !inPart(P, MD->getLocation()))
        MD = MD->getPrevious();
      if (MD)
        E.Cond = CondOf(RegionOf(MD->getLocation()));
    }
    for (const auto &[F, K] : Index) {
      P.CondOfFile[F] = CondOf(Region[K]);
      if (Region[K] >= 0)
        P.SubmoduleOf[F] = Region[K];
    }
    Deps.Submodules = std::move(Subs);
    Lap(Counts.DepsMicros);
    Counts.Submodules += Deps.Submodules.size();
  }

  /// The declarations each file inside \p P refers to in another file inside
  /// it, as (referring file, referred-to file).
  void declarationReads(const Part &P,
                        llvm::DenseSet<std::pair<FileID, FileID>> &Pairs) {
    struct Refs : RecursiveASTVisitor<Refs> {
      llvm::function_ref<void(const Decl *)> On;
      bool VisitDeclRefExpr(DeclRefExpr *E) {
        On(E->getDecl());
        return true;
      }
      bool VisitMemberExpr(MemberExpr *E) {
        On(E->getMemberDecl());
        return true;
      }
      bool VisitTagTypeLoc(TagTypeLoc TL) {
        On(TL.getDecl());
        return true;
      }
      bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
        On(TL.getDecl());
        return true;
      }
    };
    ASTContext &Ctx = CI.getASTContext();
    Decl *First =
        P.DeclsAfter
            ? const_cast<Decl *>(P.DeclsAfter)->getNextDeclInContext()
            : *Ctx.getTranslationUnitDecl()->noload_decls_begin();
    for (Decl *D = First; D; D = D->getNextDeclInContext()) {
      if (!inPart(P, D->getLocation()))
        continue;
      FileID From = SM.getFileID(SM.getExpansionLoc(D->getLocation()));
      auto Note = [&](const Decl *R) {
        if (!R)
          return;
        SourceLocation L = R->getLocation();
        if (L.isInvalid() || !SM.isLocalSourceLocation(L) || !inPart(P, L))
          return;
        FileID To = SM.getFileID(SM.getExpansionLoc(L));
        if (To != From)
          Pairs.insert({From, To});
      };
      Refs V;
      V.On = Note;
      V.TraverseDecl(D);
    }
  }

  /// CLANG_HEADER_CACHE_INDEP=1: for each guarded header inside \p P,
  /// whether it reads or changes a state that \p P's lines before it set,
  /// one line each, then one for \p P itself.
  void measureIndependence(const Part &P, llvm::StringRef Module,
                           size_t Bytes) {
    using Independence = ASTCacheDependencyTracker::Independence;
    const Independence *H = Tracker.independenceFor(P.FID);
    if (!H)
      return;
    llvm::DenseSet<FileID> InH(H->Closure.begin(), H->Closure.end());
    llvm::DenseSet<std::pair<FileID, FileID>> DeclPairs;
    declarationReads(P, DeclPairs);
    HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();

    struct Nested {
      const Independence *X;
      llvm::DenseSet<FileID> Closure;
      std::string Guard;
      /// The files of P outside X that X reads from, by macro or by
      /// declaration.
      llvm::DenseSet<FileID> Reads;
      bool Alters = false, DeclRead = false, Strict = false;
      std::string First;
    };
    std::vector<Nested> All;
    for (FileID FID : H->Closure) {
      if (FID == P.FID)
        continue;
      const Independence *X = Tracker.independenceFor(FID);
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
      if (!X || !FE)
        continue;
      HeaderFileInfo &Info = HS.getFileInfo(*FE);
      std::string Guard;
      if (const IdentifierInfo *G =
              Info.getControllingMacro(HS.getExternalLookup()))
        Guard = G->getName().str();
      else if (Info.isPragmaOnce)
        Guard = "#pragma-once";
      else
        continue;
      Nested N{X, llvm::DenseSet<FileID>(X->Closure.begin(), X->Closure.end()),
               Guard};
      for (auto &[O, What] : X->ReadFrom)
        if (InH.contains(O)) {
          N.Reads.insert(O);
          if (N.First.empty())
            N.First = What;
        }
      for (auto [From, To] : DeclPairs)
        if (N.Closure.contains(From) && !N.Closure.contains(To) &&
            InH.contains(To)) {
          if (N.Reads.insert(To).second && N.First.empty())
            N.First = "refers to a declaration in " +
                      SM.getFileEntryRefForID(To)->getName().str();
          N.DeclRead = true;
        }
      for (auto &[O, What] : X->AltersFrom)
        if (InH.contains(O)) {
          N.Alters = true;
          N.First = What;
        }
      N.Strict = N.Reads.empty() && !N.Alters && !X->Poisoned;
      All.push_back(std::move(N));
    }
    // Relaxed: reads only from headers before it that are independent
    // themselves, which a hit would load first.
    for (Nested &N : All) {
      bool Relaxed = !N.Alters && !N.X->Poisoned;
      for (FileID O : N.Reads) {
        bool Covered = false;
        for (const Nested &Y : All)
          if (&Y != &N && Y.Strict && Y.Closure.contains(O) &&
              !Y.Closure.contains(N.X->FID)) {
            Covered = true;
            break;
          }
        if (!Covered) {
          Relaxed = false;
          break;
        }
      }
      std::string Line;
      llvm::raw_string_ostream OS(Line);
      OS << Module << " " << P.Stored << " "
         << absolutePath(CI.getFileManager(), N.X->Path) << " guard="
         << N.Guard << " strict=" << N.Strict << " relaxed=" << Relaxed
         << " readfiles=" << N.Reads.size() << " declread=" << N.DeclRead
         << " alters=" << N.Alters << " poisoned=" << N.X->Poisoned
         << " first=" << N.First;
      Counts.Independence.push_back(std::move(Line));
    }
    std::string Line;
    llvm::raw_string_ostream OS(Line);
    // The files before the header that it reads from; a header reading
    // from none depends only on the command line.
    size_t Files = 0;
    std::string First;
    for (auto &[O, What] : H->ReadFrom)
      if (SM.getFileEntryRefForID(O)) {
        ++Files;
        if (First.empty())
          First = What;
      }
    OS << Module << " " << P.Stored << " self readfiles=" << Files
       << " bytes=" << Bytes << " nested=" << All.size() << " first=" << First;
    Counts.Independence.push_back(std::move(Line));
  }

  /// CLANG_HEADER_CACHE_DUMP_PARTS=<dir>, for debugging: each part goes to
  /// <dir>/<module name>.pcm, and what it was exported from and the outside
  /// declarations it requires to <dir>/<module name>.txt.
  static const char *dumpDir() {
    const char *Dir = std::getenv("CLANG_HEADER_CACHE_DUMP_PARTS");
    return Dir && *Dir ? Dir : nullptr;
  }

  /// "<bits>\t<kind>\t<name>\t<file>:<line>" for one declaration or macro
  /// a part holds. A declaration from outside the header is of kind "ref"
  /// when referred to by name, "copy" when deep-copied.
  std::string describeSize(const Part &P, bool Named, const PartSize &Z) const {
    std::string Kind, Name;
    SourceLocation Loc;
    if (Z.D) {
      Kind = inPart(P, Z.D->getLocation()) ? Z.D->getDeclKindName()
             : Named                       ? "ref"
                                           : "copy";
      if (const auto *ND = dyn_cast<NamedDecl>(Z.D))
        Name = ND->getNameAsString();
      Loc = Z.D->getLocation();
    } else {
      Kind = "macro";
      Name = Z.MacroName->getName().str();
      Loc = Z.Macro->getDefinitionLoc();
    }
    if (Name.empty())
      Name = "(unnamed)";
    std::string Where = "?";
    if (Loc.isValid()) {
      PresumedLoc PL = SM.getPresumedLoc(SM.getFileLoc(Loc));
      if (PL.isValid())
        Where =
            std::string(PL.getFilename()) + ":" + std::to_string(PL.getLine());
    }
    return std::to_string(Z.Bits) + "\t" + Kind + "\t" + Name + "\t" + Where;
  }

  void dump(const Part &P, const PushRequest &R,
            const std::vector<std::string> &Sizes) {
    dumpDependencies(R);
    const char *Dir = dumpDir();
    if (!Dir)
      return;
    std::string Base = (llvm::Twine(Dir) + "/" + R.ModuleName).str();
    std::error_code EC;
    llvm::raw_fd_ostream Bytes(Base + ".pcm", EC);
    if (EC)
      return;
    Bytes.write(R.Data.data(), R.Data.size());
    llvm::raw_fd_ostream Text(Base + ".txt", EC);
    if (EC)
      return;
    Text << "header " << P.Stored << "\nfrom "
         << CI.getFrontendOpts().Inputs[0].getFile() << "\nloaded before "
         << P.ModulesAtBegin << "\n";
    for (const DeclarationDependency &D : R.Deps.Declarations)
      Text << "requires " << D.Name << ": " << D.Fingerprint << "\n";
    llvm::raw_fd_ostream SizeText(Base + ".sizes", EC);
    if (EC)
      return;
    for (const std::string &Line : Sizes)
      SizeText << Line << "\n";
  }

  /// CLANG_HEADER_CACHE_DUMP_DEPS=<dir>, for measurement: each copy's
  /// metadata as the check reads it, one element per line, to <dir>/<module
  /// name>.deps.
  void dumpDependencies(const PushRequest &R) {
    static const char *Dir = std::getenv("CLANG_HEADER_CACHE_DUMP_DEPS");
    if (!Dir)
      return;
    std::error_code EC;
    static unsigned Seq = 0;
    llvm::raw_fd_ostream OS((llvm::Twine(Dir) + "/" + R.ModuleName + "." +
                             llvm::Twine(::getpid()) + "." + llvm::Twine(Seq++) +
                             ".deps")
                                .str(),
                            EC);
    if (EC)
      return;
    const HeaderDependencySet &D = R.Deps;
    OS << "module\t" << R.ModuleName << "\nheader\t" << R.Filename << "\nfrom\t"
       << CI.getFrontendOpts().Inputs[0].getFile() << "\nwarn\t"
       << D.WarningState << "\npragma\t" << D.Entry.AlignPackRaw << " "
       << D.Entry.FpPragmaRaw << "\n";
    for (const auto &E : D.Conditional)
      OS << "cond\t" << E.MacroName << "\t" << E.WasDefined << "\n";
    for (const auto &E : D.Syntactic)
      OS << (E.Required == SyntacticDependency::State::NotAMacro ? "notmacro\t"
                                                                 : "expands\t")
         << E.Name << "\t" << llvm::xxh3_64bits(E.TokenSequence) << "\n";
    for (const auto &E : D.Inclusion)
      OS << "inclusion\t" << E.HeaderPath << "\n";
    for (const auto &E : D.Symbol)
      OS << "symbol\t" << E.Name << "\t" << unsigned(E.Classification) << "\t"
         << E.ResolvedTypeName << "\n";
    for (const auto &E : D.Files)
      OS << "file\t" << E.Path << "\t" << E.Size << "\t" << E.ModTime << "\n";
    for (const auto &E : D.Includes)
      OS << "include\t" << unsigned(E.K) << "\t" << E.Angled << "\t"
         << E.Spelled << "\t" << E.Includer << "\t" << E.Found << "\n";
    for (const auto &E : D.FileNames)
      OS << "filename\t" << unsigned(E.Which) << "\t" << E.File << "\t"
         << E.Text << "\n";
    for (const auto &E : D.Declarations)
      OS << "decl\t" << unsigned(E.In) << "\t" << E.Name << "\t"
         << llvm::xxh3_64bits(E.Fingerprint) << "\n";
  }

  struct Written {
    SmallVector<char, 0> Buffer;
    /// What the part's lines outside a submodule refer to in it.
    std::vector<CrossReference> Cross;
    long long Micros = 0;
    /// Outside declarations referred to by name.
    size_t ByName = 0;
    unsigned DroppedLocations = 0;
    /// The outside declarations a hit requires, by name and fingerprint.
    std::vector<DeclarationDependency> Declarations;
    /// CLANG_HEADER_CACHE_LINK: the declarations taken from each link, by
    /// index into Part::Links.
    std::vector<std::pair<size_t, std::string>> LinkNames;
    /// Non-empty: an outside declaration that could not be named, and why.
    std::string Failure;
    /// The part's diagnostics, located in its own encoding; or, if one
    /// cannot be, why.
    std::vector<SavedDiagnostic> Diagnostics;
    std::string DiagnosticFailure;
    /// With CLANG_HEADER_CACHE_DUMP_PARTS: one line per declaration and
    /// macro written, "<bits>\t<kind>\t<name>\t<file>:<line>".
    std::vector<std::string> Sizes;
  };

  /// Why \p P's diagnostics cannot be printed again as they were, or
  /// empty. One located outside the part -- a note on the includer's
  /// declaration, a warning about another file -- or in a macro expansion,
  /// whose spelling may be outside, cannot.
  std::string diagnosticsOutside(const Part &P) const {
    auto Inside = [&](SourceLocation L) {
      return L.isInvalid() ||
             (L.isFileID() && !SM.isLoadedSourceLocation(L) && inPart(P, L));
    };
    for (const Captured &C : P.Diagnostics) {
      bool Ok = Inside(C.Loc);
      for (const CharSourceRange &R : C.Ranges)
        Ok = Ok && Inside(R.getBegin()) && Inside(R.getEnd());
      for (const FixItHint &F : C.FixIts)
        Ok = Ok && F.InsertFromRange.isInvalid() &&
             Inside(F.RemoveRange.getBegin()) && Inside(F.RemoveRange.getEnd());
      if (!Ok)
        return "it printed a diagnostic located outside it: " +
               C.Message.str().str();
    }
    return "";
  }

  /// Every location of \p P's diagnostics, in the order saveDiagnostics
  /// reads their encodings; no location first.
  static std::vector<SourceLocation> diagnosticLocations(const Part &P) {
    std::vector<SourceLocation> Locs{SourceLocation()};
    for (const Captured &C : P.Diagnostics) {
      Locs.push_back(C.Loc);
      for (const CharSourceRange &R : C.Ranges)
        Locs.insert(Locs.end(), {R.getBegin(), R.getEnd()});
      for (const FixItHint &F : C.FixIts)
        Locs.insert(Locs.end(),
                    {F.RemoveRange.getBegin(), F.RemoveRange.getEnd()});
    }
    return Locs;
  }

  /// Puts \p P's diagnostics into \p W, with the encodings the writer gave
  /// diagnosticLocations. A valid location that came out as no location is in
  /// a file the part dropped, and the part is not saved.
  static void saveDiagnostics(const Part &P,
                              const std::vector<uint64_t> &Encoded,
                              Written &W) {
    size_t Next = 1;
    bool Lost = false;
    auto Take = [&](SourceLocation L) {
      uint64_t E = Encoded[Next++];
      Lost |= L.isValid() && E == Encoded[0];
      return E;
    };
    auto TakeRange = [&](const CharSourceRange &R) {
      SavedDiagnostic::Range Out;
      Out.Begin = Take(R.getBegin());
      Out.End = Take(R.getEnd());
      Out.TokenRange = R.isTokenRange();
      return Out;
    };
    for (const Captured &C : P.Diagnostics) {
      SavedDiagnostic D;
      D.Level = C.Level;
      D.ID = C.ID;
      D.Message = C.Message.str();
      D.Loc = Take(C.Loc);
      for (const CharSourceRange &R : C.Ranges)
        D.Ranges.push_back(TakeRange(R));
      for (const FixItHint &F : C.FixIts) {
        SavedDiagnostic::FixIt &S = D.FixIts.emplace_back();
        S.Remove = TakeRange(F.RemoveRange);
        S.Insert = F.CodeToInsert;
        S.BeforePreviousInsertions = F.BeforePreviousInsertions;
      }
      W.Diagnostics.push_back(std::move(D));
    }
    if (Lost)
      W.DiagnosticFailure =
          "it printed a diagnostic in a file it does not hold";
  }

  /// Writes \p P's part as module \p M, referring to outside declarations
  /// by name if \p Named, and by deep copy otherwise.
  Written writePart(const Part &P, Module *M, bool Named) {
    Written W;
    llvm::BitstreamWriter Stream(W.Buffer);
    ASTWriter Writer(Stream, W.Buffer, CI.getModuleCache(), CI.getCodeGenOpts(),
                     {}, /*IncludeTimestamps=*/false);
    std::vector<const Decl *> FoldedDecls;
    if (!P.Folded.empty()) {
      SmallVector<Decl *, 64> Top;
      for (serialization::ModuleFile *MF : P.Folded)
        CI.getASTReader()->getTopLevelDecls(*MF, Top);
      for (Decl *D : Top)
        if (!isa<ImportDecl>(D))
          FoldedDecls.push_back(D);
    }
    Writer.setPartFoldedDecls(FoldedDecls);
    if (!P.Folded.empty())
      Writer.setPartLoadedLocations(
          [&](SourceLocation L) { return translateLoaded(P.FoldedEntries, L); });
    Writer.setPartFilter(
        [&](const Decl *D) {
          // The import of a copy served inside the header: the part holds
          // what it folds in from the copy, and imports nothing.
          if (isa<ImportDecl>(D))
            return false;
          if (D->isFromASTFile())
            return !P.Folded.empty() &&
                   P.Folded.count(
                       CI.getASTReader()->getOwningModuleFile(D));
          return inPart(P, D->getLocation());
        },
        [&](const MacroDirective *MD) {
          SourceLocation::UIntTy At = MD->getLocation().getRawEncoding();
          return P.contains(At) && !inLinked(P, At);
        },
        [&](SourceLocation::UIntTy Offset) {
          return P.contains(Offset) || inFolded(P, Offset);
        },
        [&](FileEntryRef F) { return P.Files.contains(&F.getFileEntry()); });
    Writer.setPartOutside(Named,
                          [&](const Decl *D) { return madeAfter(P, D); });
    if (!P.OwnerOf.empty())
      Writer.setPartOwner([&](SourceLocation L) -> Module * {
        // Most declarations a part refers to from outside it are before it.
        if (!inPart(P, L))
          return nullptr;
        auto It = P.OwnerOf.find(SM.getFileID(SM.getExpansionLoc(L)));
        return It == P.OwnerOf.end() ? nullptr : It->second;
      });
    // What the writer needs to look at: the part's own entries, macros and
    // declarations, not the whole compile's (REPORT.md I5).
    std::vector<const IdentifierInfo *> MacroNames;
    for (const IdentifierInfo *II : P.Touched)
      if (!P.LinkedNames.contains(II))
        MacroNames.push_back(II);
    for (const auto &[II, Stack] : P.Stacks)
      if (!P.Touched.contains(II))
        MacroNames.push_back(II);
    Writer.setPartScope(P.FID.getHashValue(), P.SLocEnd, memoryBuffers(),
                        MacroNames, P.DeclsAfter);
    std::vector<PartSize> Sizes;
    if (dumpDir())
      Writer.setPartSizes(&Sizes);
    std::vector<uint64_t> Encoded;
    Writer.setPartLocations(diagnosticLocations(P), &Encoded);
    auto Start = std::chrono::steady_clock::now();
    Writer.WriteAST(&CI.getSema(), P.Stored, M, /*isysroot=*/"");
    W.Micros = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - Start)
                   .count();
    Counts.WriterMicros += W.Micros;
    for (const ASTWriter::PartCrossReference &C :
         Writer.getPartCrossReferences())
      W.Cross.push_back(
          {C.D, C.From, C.To,
           static_cast<uint32_t>(Writer.getDeclID(C.D).getRawValue() -
                                 NUM_PREDEF_DECL_IDS)});
    W.DroppedLocations = Writer.getPartDroppedLocations();
    saveDiagnostics(P, Encoded, W);
    llvm::TimeTraceScope OutsideScope("HeaderCache ExportOutsideDecls");
    for (const PartSize &Z : Sizes)
      W.Sizes.push_back(describeSize(P, Named, Z));

    // The outside declarations the part refers to, each once. A member of a
    // struct, or an unnamed struct a typedef names, is reached through the
    // file-scope declaration listed here, whose fingerprint covers it.
    //
    // A hit checks the loading compile where the header begins, so the
    // fingerprint is of the declarations made before the header: the latest
    // of them, and none of the header's own redeclarations or definitions.
    auto Before = [&](const Decl *D) {
      return !inPart(P, D->getLocation()) && !madeAfter(P, D);
    };
    llvm::DenseSet<std::pair<unsigned, IdentifierInfo *>> Seen;
    auto Require = [&](const NamedDecl &ND) {
      DeclarationDependency::Space In = nameSpaceOf(ND);
      if (!Seen.insert({unsigned(In), ND.getIdentifier()}).second)
        return true;
      const Decl *AtEntry = ND.getMostRecentDecl();
      while (AtEntry && !Before(AtEntry))
        AtEntry = AtEntry->getPreviousDecl();
      std::optional<std::string> F = declarationFingerprint(
          AtEntry ? *cast<NamedDecl>(AtEntry) : ND, Before);
      if (!F)
        return false;
      W.Declarations.push_back({In, ND.getName().str(), std::move(*F)});
      return true;
    };
    if (Named) {
      W.Failure = Writer.getPartNameReferenceFailure().str();
      W.ByName = Writer.getPartNameReferences().size();
      for (const NamedDecl *ND : Writer.getPartNameReferences()) {
        if (getenv("DEBUG_LINK"))
          llvm::errs() << "NAMEDREF " << P.Stored << " " << ND->getName()
                       << " linked=" << linkedDecl(P, *ND)
                       << " link=" << linkOf(P, *ND)
                       << " fromAST=" << ND->isFromASTFile() << "\n";
        // A linked copy loads before the part, and declares it there.
        if (linkedDecl(P, *ND)) {
          int K = linkOf(P, *ND);
          if (K >= 0 && ND->getIdentifier()) {
            W.LinkNames.push_back(
                {size_t(K), (nameSpaceOf(*ND) == DeclarationDependency::Space::Tag
                                 ? "t:"
                                 : "o:") +
                                ND->getName().str()});
            continue;
          }
        }
        if (W.Failure.empty() && !Require(*ND))
          W.Failure = std::string(ND->getDeclKindName()) + " '" +
                      ND->getNameAsString() + "': it has no fingerprint";
      }
      return W;
    }
    // A deep copy: anything written that the header's region did not
    // produce.
    for (Decl *D :
         CI.getASTContext().getTranslationUnitDecl()->noload_decls()) {
      if (D->isFromASTFile() || Writer.isDeclPredefined(D) ||
          !Writer.wasDeclEmitted(D) || !Before(D))
        continue;
      if (auto *ND = dyn_cast<NamedDecl>(D))
        if (ND->getIdentifier())
          Require(*ND);
    }
    return W;
  }

  CompilerInstance &CI;
  SourceManager &SM;
  ASTCacheDependencyTracker &Tracker;
  std::function<void(PushRequest &&)> Deliver;
  /// Whether a part refers to outside declarations by name (the default) or
  /// holds deep copies of them.
  bool ByName;
  /// Where a warning pragma is in force; null if not followed.
  const WarningPragmaTracker *Pragmas;
  /// Set by exportNext for the file about to be entered.
  const FileEntry *Expected = nullptr;
  std::string ExpectedPath;
  /// Parts whose header is open, innermost last, and parts whose header has
  /// ended, waiting for the next finished declaration.
  llvm::SmallVector<Part, 4> Open;
  std::vector<Part> Ready;
  /// The parts writeReady is writing, out of Ready.
  std::vector<Part> *Batch = nullptr;
  /// CLANG_HEADER_CACHE_LINK: a copy links the copies of the headers inside
  /// it, instead of holding them.
  bool LinkMode = false;
  unsigned PartsBegun = 0;
  /// While a hit's diagnostics are printed again.
  bool Replaying = false;
  std::function<bool(llvm::StringRef, llvm::StringRef)> SeenBefore;
  llvm::DenseSet<FileID> Tracked;
  bool ObserverSet = false;
  /// See memoryBuffers.
  std::vector<unsigned> Buffers;
  unsigned BuffersSeen = 1;
  /// The latest top-level declaration the parser finished.
  const Decl *LastTopLevel = nullptr;
  /// Each header entered inside one being exported: whether it began
  /// between declarations, and whether it left the pragmas as it found them
  /// (REPORT.md T17).
  struct Nested {
    bool AtBoundary = false, Neutral = false;
    PragmaSnapshot Entry;
    WarningPragmaTracker::Mark Mark;
  };
  llvm::DenseMap<FileID, Nested> NestedAt;
  Stats Counts;
};

/// Every compile gets one of these, through the plugin registry. It tells the
/// compile's exporter, if there is one, when a top-level declaration or the
/// translation unit is finished.
class ExportConsumer : public ASTConsumer {
public:
  explicit ExportConsumer(const CompilerInstance &CI) : CI(CI) {}

  bool HandleTopLevelDecl(DeclGroupRef Group) override {
    if (Exporter *E = exporterFor(CI))
      E->declarationFinished(Group);
    return true;
  }

  /// A declaration a loaded module file hands over, one that must be
  /// emitted, is not one the parser finished. A header that ended inside a
  /// function definition -- the parser reads one token past its last `}`,
  /// and that token can be the next #include, served -- is still waiting
  /// for the function's body, and must not be written yet: git's
  /// fsmonitor.h was saved with its last function, mark_fsmonitor_invalid,
  /// declared but not defined.
  void HandleInterestingDecl(DeclGroupRef) override {}

  /// Nor is the import a linked copy's loading makes (BuildModuleInclude),
  /// for the same reason: git's progress.h was saved with stop_progress
  /// declared but not defined when the token after its `}` was an #include
  /// whose copy has links.
  void HandleImplicitImportDecl(ImportDecl *) override {}

  void HandleTranslationUnit(ASTContext &) override {
    if (Exporter *E = exporterFor(CI))
      E->translationUnitFinished();
  }

private:
  const CompilerInstance &CI;
};

class ExportPlugin : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    return std::make_unique<ExportConsumer>(CI);
  }
  bool ParseArgs(const CompilerInstance &,
                 const std::vector<std::string> &) override {
    return true;
  }
  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<ExportPlugin>
    ExportPluginEntry("header-cache-export",
                      "tells the header cache when a declaration is finished");

PartExporter &PartExporter::install(CompilerInstance &CI,
                                    std::function<void(PushRequest &&)> Deliver,
                                    const WarningPragmaTracker *Pragmas) {
  Preprocessor &PP = CI.getPreprocessor();
  auto Tracker = std::make_unique<ASTCacheDependencyTracker>(PP);
  ASTCacheDependencyTracker &T = *Tracker;
  const char *Outside = std::getenv("CLANG_HEADER_CACHE_OUTSIDE");
  bool ByName = !Outside || llvm::StringRef(Outside) != "copy";
  auto E =
      std::make_unique<Exporter>(CI, T, std::move(Deliver), ByName, Pragmas);
  Exporter &Ex = *E;
  // Measuring independence (REPORT.md T15) needs a record of every header
  // inside one being written as well.
  const char *Indep = std::getenv("CLANG_HEADER_CACHE_INDEP");
  if (Indep && llvm::StringRef(Indep) == "1") {
    T.measureIndependence();
    T.trackOnly([&Ex](FileID FID) { return Ex.tracks(FID) || Ex.inside(); });
  } else {
    T.trackOnly([&Ex](FileID FID) { return Ex.tracks(FID); });
  }
  // What each file inside a header being exported read, for holding the
  // headers inside as submodules (REPORT.md T17).
  // Measuring only: CLANG_HEADER_CACHE_NO_SUBMODULES writes folded copies,
  // as before REPORT.md T17.
  // So does linking (CLANG_HEADER_CACHE_LINK), to tell which headers
  // inside are independent of the one around them.
  const char *Link = std::getenv("CLANG_HEADER_CACHE_LINK");
  if (!std::getenv("CLANG_HEADER_CACHE_NO_SUBMODULES") ||
      (Link && llvm::StringRef(Link) == "1") ||
      std::getenv("CLANG_HEADER_CACHE_DDG"))
    T.recordFiles();
  // Measuring a codebase's headers: a record for every one.
  if (std::getenv("CLANG_HEADER_CACHE_DDG"))
    T.trackOnly([](FileID) { return true; });
  PP.setExpansionCandidateObserver(T);
  // Callbacks added later run first: the exporter marks a header as tracked
  // before the tracker decides whether to open a frame for it.
  PP.addPPCallbacks(std::move(Tracker));
  PP.addPPCallbacks(std::move(E));
  return Ex;
}
