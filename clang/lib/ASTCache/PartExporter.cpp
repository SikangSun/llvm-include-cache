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
#include "clang/ASTCache/MacroTokens.h"
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
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <chrono>
#include <map>
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
           std::function<void(PushRequest &&)> Deliver, bool ByName,
           const WarningPragmaTracker *Pragmas)
      : CI(CI), SM(CI.getSourceManager()), Tracker(Tracker),
        Deliver(std::move(Deliver)), ByName(ByName), Pragmas(Pragmas) {
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

  void served() override {
    for (Part &P : Open)
      if (P.Decline.empty())
        P.Decline = "a saved copy was served inside the header";
  }

  bool writing() const override {
    return llvm::any_of(Open, [](const Part &P) { return P.Decline.empty(); });
  }

  void exportWhenEntered(std::string Path) override {
    ExpectedPath = std::move(Path);
  }

  void diagnostic(DiagnosticsEngine::Level Level,
                  const Diagnostic &Info) override {
    if (Level == DiagnosticsEngine::Ignored)
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
    if (Reason != ExitFile || Open.empty() || Open.back().FID != PrevFID)
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

  void translationUnitFinished() { writeReady(); }

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

    bool contains(SourceLocation::UIntTy Offset) const {
      return Begin <= Offset && Offset < End;
    }
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
    return P.contains(SM.getExpansionLoc(L).getRawEncoding());
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
        warningState(warningFlagsHash(CI.getDiagnosticOpts()), Pragmas);
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
    if (modulesLoaded() > P.ModulesAtBegin && P.Decline.empty())
      P.Decline = "a saved copy was loaded inside the header";
    Ready.push_back(std::move(P));
  }

  void writeReady() {
    if (Ready.empty())
      return;
    auto Start = std::chrono::steady_clock::now();
    llvm::TimeTraceScope Scope("HeaderCache Export");
    std::vector<Part> Parts;
    Parts.swap(Ready);
    for (Part &P : Parts)
      write(P);
    Counts.Micros += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - Start)
                         .count();
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

    Preprocessor &PP = CI.getPreprocessor();
    ModuleMap &MM = PP.getHeaderSearchInfo().getModuleMap();
    if (MM.findModule(Name))
      return decline(P, "the same part was already exported in this compile");
    Module *M = MM.createModule(Name, /*Parent=*/nullptr,
                                /*IsFramework=*/false, /*IsExplicit=*/false);
    M->PresumedModuleMapFile = "<header-cache-part>";

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
      if (P.ModulesAtBegin)
        return decline(P, "cannot refer by name to " + W.Failure);
      std::string Why = std::move(W.Failure);
      W = writePart(P, M, /*ByName=*/false);
      ++Counts.Copied;
      Counts.Copies.push_back(P.Stored + ": " + Why);
    }
    Deps.Declarations = std::move(W.Declarations);
    if (!W.DiagnosticFailure.empty())
      return decline(P, W.DiagnosticFailure);
    Deps.Diagnostics = std::move(W.Diagnostics);
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
    dump(P, R, W.Sizes);
    llvm::TimeTraceScope PushScope("HeaderCache Push");
    Deliver(std::move(R));
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

  struct Written {
    SmallVector<char, 0> Buffer;
    long long Micros = 0;
    /// Outside declarations referred to by name.
    size_t ByName = 0;
    unsigned DroppedLocations = 0;
    /// The outside declarations a hit requires, by name and fingerprint.
    std::vector<DeclarationDependency> Declarations;
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
    Writer.setPartFilter(
        [&](const Decl *D) { return inPart(P, D->getLocation()); },
        [&](const MacroDirective *MD) {
          return P.contains(MD->getLocation().getRawEncoding());
        },
        [&](SourceLocation::UIntTy Offset) { return P.contains(Offset); },
        [&](FileEntryRef F) { return P.Files.contains(&F.getFileEntry()); });
    Writer.setPartOutside(Named,
                          [&](const Decl *D) { return madeAfter(P, D); });
    // What the writer needs to look at: the part's own entries, macros and
    // declarations, not the whole compile's (REPORT.md I5).
    std::vector<const IdentifierInfo *> MacroNames(P.Touched.begin(),
                                                   P.Touched.end());
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
      for (const NamedDecl *ND : Writer.getPartNameReferences())
        if (W.Failure.empty() && !Require(*ND))
          W.Failure = std::string(ND->getDeclKindName()) + " '" +
                      ND->getNameAsString() + "': it has no fingerprint";
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
  llvm::DenseSet<FileID> Tracked;
  bool ObserverSet = false;
  /// See memoryBuffers.
  std::vector<unsigned> Buffers;
  unsigned BuffersSeen = 1;
  /// The latest top-level declaration the parser finished.
  const Decl *LastTopLevel = nullptr;
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
  T.trackOnly([&Ex](FileID FID) { return Ex.tracks(FID); });
  PP.setExpansionCandidateObserver(T);
  // Callbacks added later run first: the exporter marks a header as tracked
  // before the tracker decides whether to open a frame for it.
  PP.addPPCallbacks(std::move(Tracker));
  PP.addPPCallbacks(std::move(E));
  return Ex;
}
