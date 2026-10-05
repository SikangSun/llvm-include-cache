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
#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/DeclFingerprint.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
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
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include <chrono>
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

class Exporter : public PartExporter, public PPCallbacks {
public:
  Exporter(CompilerInstance &CI, ASTCacheDependencyTracker &Tracker,
           std::function<void(PushRequest &&)> Deliver)
      : CI(CI), SM(CI.getSourceManager()), Tracker(Tracker),
        Deliver(std::move(Deliver)) {
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

  void exportWhenEntered(std::string Path) override {
    ExpectedPath = std::move(Path);
  }

  const Stats &stats() const override { return Counts; }

  /// Whether the tracker should keep a record for \p FID: only for the
  /// headers being exported.
  bool tracks(FileID FID) const { return Tracked.contains(FID); }

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
      for (Part &P : Open)
        P.Files.insert(&FE->getFileEntry());
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
    if (Reason != ExitFile || Open.empty() || Open.back().FID != PrevFID)
      return;
    end();
  }

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *) override {
    for (Part &P : Open)
      P.Touched.insert(MacroNameTok.getIdentifierInfo());
  }

  void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &,
                      const MacroDirective *) override {
    for (Part &P : Open)
      P.Touched.insert(MacroNameTok.getIdentifierInfo());
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
  void declarationFinished() {
    if (!Ready.empty())
      writeReady();
  }

  void translationUnitFinished() { writeReady(); }

private:
  struct Part {
    FileID FID;
    /// The name the compile opened the header by, and the absolute path the
    /// daemon stores it under.
    std::string Name, Stored;
    /// The offsets of every location made while the header was open.
    SourceLocation::UIntTy Begin = 0, End = 0;
    PragmaSnapshot Entry, Exit;
    llvm::SetVector<const IdentifierInfo *> Touched;
    std::vector<MacroEffect> Effects;
    /// Every file entered while the header was open, the header included.
    llvm::DenseSet<const FileEntry *> Files;
    /// Non-empty: why the part is not saved.
    std::string Decline;

    bool contains(SourceLocation::UIntTy Offset) const {
      return Begin <= Offset && Offset < End;
    }
  };

  /// Whether \p L, where a macro was used if it is in one, falls in \p P.
  bool inPart(const Part &P, SourceLocation L) const {
    if (L.isInvalid())
      return false;
    return P.contains(SM.getExpansionLoc(L).getRawEncoding());
  }

  /// Copies a module load leaves in this compile: the part cannot name them,
  /// since a part imports nothing.
  size_t modulesLoaded() const {
    if (!CI.getASTReader())
      return 0;
    return CI.getASTReader()->getModuleManager().size();
  }

  void begin(FileID FID, FileEntryRef FE) {
    Part P;
    P.FID = FID;
    P.Name = FE.getName().str();
    P.Stored = absolutePath(CI.getFileManager(), P.Name);
    P.Begin = SM.getSLocEntry(FID).getOffset();
    P.Files.insert(&FE.getFileEntry());
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
    // A declaration from a loaded copy cannot be written into a part.
    if (modulesLoaded())
      P.Decline = "a saved copy was loaded before the header";
    Tracked.insert(FID);
    Open.push_back(std::move(P));
  }

  void end() {
    Part P = Open.pop_back_val();
    P.End = SM.getNextLocalOffset();
    Preprocessor &PP = CI.getPreprocessor();
    for (const IdentifierInfo *II : P.Touched)
      P.Effects.push_back(
          {II->getName().str(),
           PP.isMacroDefined(const_cast<IdentifierInfo *>(II))});
    if (CI.hasSema()) {
      Sema &S = CI.getSema();
      P.Exit = capturePragmaState(S);
      if (const char *Pragma = findNonDefaultPragmaState(S))
        if (P.Decline.empty())
          P.Decline =
              std::string("pragma state a record cannot carry: ") + Pragma;
    }
    if (modulesLoaded() && P.Decline.empty())
      P.Decline = "a saved copy was loaded inside the header";
    Ready.push_back(std::move(P));
  }

  void writeReady() {
    std::vector<Part> Parts;
    Parts.swap(Ready);
    for (Part &P : Parts)
      write(P);
  }

  void decline(const Part &P, const llvm::Twine &Why) {
    ++Counts.Declined;
    Counts.Declines.push_back(P.Stored + ": " + Why.str());
  }

  void write(Part &P) {
    if (!P.Decline.empty())
      return decline(P, P.Decline);
    const HeaderDependencySet *Rec = Tracker.recordFor(P.FID);
    if (!Rec)
      return decline(P, "the dependency tracker could not describe it");
    if (!CI.hasSema())
      return decline(P, "no Sema");

    HeaderDependencySet Deps = *Rec;
    Deps.Entry = P.Entry;
    Deps.Exit = P.Exit;
    Deps.MacroEffects = P.Effects;
    Deps.HeaderName = P.Name;

    // The same header, metadata and compiler options give the same name in
    // any compile, which lets the daemon recognise a part it already holds.
    llvm::SmallVector<char, 256> Encoded;
    encodeDependencySet(Deps, Encoded);
    std::string Key = P.Stored;
    Key += '\0';
    Key += CI.getInvocation().getModuleHash();
    Key += '\0';
    Key.append(Encoded.begin(), Encoded.end());
    std::string Name =
        "__hcp_" + llvm::utohexstr(llvm::xxh3_64bits(Key), /*LowerCase=*/true,
                                   /*Width=*/16);

    Preprocessor &PP = CI.getPreprocessor();
    ModuleMap &MM = PP.getHeaderSearchInfo().getModuleMap();
    if (MM.findModule(Name))
      return decline(P, "the same part was already exported in this compile");
    Module *M = MM.createModule(Name, /*Parent=*/nullptr,
                                /*IsFramework=*/false, /*IsExplicit=*/false);
    M->PresumedModuleMapFile = "<header-cache-part>";

    SmallVector<char, 0> Buffer;
    llvm::BitstreamWriter Stream(Buffer);
    ASTWriter Writer(Stream, Buffer, CI.getModuleCache(), CI.getCodeGenOpts(),
                     {}, /*IncludeTimestamps=*/false);
    Writer.setPartFilter(
        [&](const Decl *D) { return inPart(P, D->getLocation()); },
        [&](const MacroDirective *MD) {
          return P.contains(MD->getLocation().getRawEncoding());
        },
        [&](SourceLocation::UIntTy Offset) { return P.contains(Offset); },
        [&](FileEntryRef F) { return P.Files.contains(&F.getFileEntry()); });
    auto Start = std::chrono::steady_clock::now();
    Writer.WriteAST(&CI.getSema(), P.Stored, M, /*isysroot=*/"");
    auto Micros = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - Start)
                      .count();

    // The declarations from outside the header that the part carries a copy
    // of: anything written that the header's region did not produce.
    llvm::DenseSet<std::pair<unsigned, IdentifierInfo *>> Seen;
    for (Decl *D :
         CI.getASTContext().getTranslationUnitDecl()->noload_decls()) {
      if (D->isFromASTFile() || !Writer.wasDeclEmitted(D) ||
          inPart(P, D->getLocation()))
        continue;
      auto *ND = dyn_cast<NamedDecl>(D);
      if (!ND)
        continue;
      std::optional<std::string> F = declarationFingerprint(*ND);
      if (!F)
        continue;
      DeclarationDependency::Space In = nameSpaceOf(*ND);
      if (!Seen.insert({unsigned(In), ND->getIdentifier()}).second)
        continue;
      Deps.Declarations.push_back({In, ND->getName().str(), std::move(*F)});
    }

    PushRequest R;
    R.Filename = P.Stored;
    R.ModuleName = Name;
    R.Deps = std::move(Deps);
    R.Data.assign(Buffer.begin(), Buffer.end());
    ++Counts.Exported;
    Counts.Exports.push_back(P.Stored + ": " + std::to_string(Buffer.size()) +
                             " bytes in " + std::to_string(Micros) + " us");
    Deliver(std::move(R));
  }

  CompilerInstance &CI;
  SourceManager &SM;
  ASTCacheDependencyTracker &Tracker;
  std::function<void(PushRequest &&)> Deliver;
  /// Set by exportNext for the file about to be entered.
  const FileEntry *Expected = nullptr;
  std::string ExpectedPath;
  /// Parts whose header is open, innermost last, and parts whose header has
  /// ended, waiting for the next finished declaration.
  llvm::SmallVector<Part, 4> Open;
  std::vector<Part> Ready;
  llvm::DenseSet<FileID> Tracked;
  bool ObserverSet = false;
  Stats Counts;
};

/// Every compile gets one of these, through the plugin registry. It tells the
/// compile's exporter, if there is one, when a top-level declaration or the
/// translation unit is finished.
class ExportConsumer : public ASTConsumer {
public:
  explicit ExportConsumer(const CompilerInstance &CI) : CI(CI) {}

  bool HandleTopLevelDecl(DeclGroupRef) override {
    if (Exporter *E = exporterFor(CI))
      E->declarationFinished();
    return true;
  }

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

PartExporter &
PartExporter::install(CompilerInstance &CI,
                      std::function<void(PushRequest &&)> Deliver) {
  Preprocessor &PP = CI.getPreprocessor();
  auto Tracker = std::make_unique<ASTCacheDependencyTracker>(PP);
  ASTCacheDependencyTracker &T = *Tracker;
  auto E = std::make_unique<Exporter>(CI, T, std::move(Deliver));
  Exporter &Ex = *E;
  T.trackOnly([&Ex](FileID FID) { return Ex.tracks(FID); });
  PP.setExpansionCandidateObserver(T);
  // Callbacks added later run first: the exporter marks a header as tracked
  // before the tracker decides whether to open a frame for it.
  PP.addPPCallbacks(std::move(Tracker));
  PP.addPPCallbacks(std::move(E));
  return Ex;
}
