//===- ASTCacheProducer.cpp -------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Options/OptionUtils.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <optional>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// The text a replayed #include writes after `#include`.
std::string replayText(const IncludeRef &I) {
  return I.Spelled.empty() ? "\"" + I.Path + "\"" : I.Spelled;
}

/// `<name>` or `"name"`, as it appears after #include. The closing character
/// cannot occur inside the name: the lexer ends the name there.
bool validSpelling(llvm::StringRef S) {
  if (S.size() < 3 || S.find_first_of("\n\r") != llvm::StringRef::npos)
    return false;
  char Close = S.front() == '<' ? '>' : S.front() == '"' ? '"' : '\0';
  return Close && S.back() == Close &&
         S.drop_front().drop_back().find(Close) == llvm::StringRef::npos;
}

/// What the compile learned about the target header, copied out while the
/// compile is still running. The tracker, Sema, and the AST consumers are all
/// destroyed before ExecuteAction returns, so nothing may be read from any of
/// them afterwards.
struct ProduceOutcome {
  bool TargetEntered = false;
  /// The target was first entered from inside a context file, so the target's
  /// declarations landed in the hidden context submodule.
  bool ReachedThroughContext = false;
  bool HaveRecord = false;
  HeaderDependencySet Deps;
  PragmaSnapshot Entry, Exit;
  /// Non-empty names a pragma whose state a record cannot carry.
  std::string Unrepresentable;
  /// Non-empty says which replayed #include opened a different file than the
  /// compile being reproduced did.
  std::string ReplayMismatch;
  /// Non-empty names a builtin whose value depends on the compile that
  /// includes the header, expanded inside the target.
  std::string VaryingBuiltin;
  /// Each macro the target's text defined or undefined, with its state when
  /// the target ended.
  std::vector<MacroEffect> MacroEffects;
};

/// Builtins whose value depends on the compile that includes the header
/// rather than on the header. DECISIONS.md 2.7 has the reasoning, including
/// why __LINE__ is not here. __FILE__ and __FILE_NAME__ are not either: their
/// text depends on the name the compile reaches the header by, which a hit
/// can check (FileNameDependency).
constexpr llvm::StringLiteral TranslationUnitVaryingBuiltins[] = {
    "__COUNTER__", "__INCLUDE_LEVEL__", "__BASE_FILE__",
    "__DATE__",    "__TIME__",          "__TIMESTAMP__"};

/// Rides alongside the module writer to observe Sema, which the dependency
/// tracker cannot: the tracker is a PPCallbacks and sees only the
/// preprocessor.
///
/// The observer registers the tracker as Sema's name-classification observer,
/// so symbol dependencies are recorded, and takes the pragma snapshots that
/// become the record's Entry and Exit. The snapshots are taken when the
/// target header is entered and left, not at the start and end of the
/// compile, because the context in front of the target can change pragma
/// state, and the record needs the state the target itself saw.
class ProducerObserver : public SemaConsumer {
public:
  ProducerObserver(ASTCacheDependencyTracker &Tracker, ProduceOutcome &Out)
      : Tracker(Tracker), Out(Out) {}

  void InitializeSema(Sema &S) override {
    TheSema = &S;
    S.setNameClassificationObserver(Tracker);
    // With ParseAST as it stands, Sema is initialized before the target can
    // be entered. Parser::Initialize calls Actions.Initialize(), which calls
    // InitializeSema, before Parser::Initialize's first ConsumeToken(); and
    // until the first token is lexed, no #include has been processed. So
    // targetEntered() and targetExited() normally find Sema already set.
    //
    // They cope without Sema anyway, so that a reordering upstream degrades
    // safely instead of recording garbage. If the target were entered before
    // Sema existed, no pragma could have changed Sema's state yet -- a pragma
    // that affects Sema reaches Sema as a token, and the parser would have
    // consumed none -- so the right snapshot is the initial state, which is
    // what Sema holds at this point.
    if (EnteredEarly)
      recordEntry();
    if (ExitedEarly)
      recordExit();
  }

  void HandleTranslationUnit(ASTContext &) override {
    Tracker.finalize();
    // An unguarded target reached twice has two records. The first is the
    // one the pragma snapshots belong to.
    for (const HeaderRecord &R : Tracker.completed()) {
      if (R.HeaderPath == TargetName) {
        Out.Deps = R.Deps;
        Out.HaveRecord = true;
        break;
      }
    }
    Out.Entry = Entry;
    Out.Exit = Exit;
    if (Unrepresentable)
      Out.Unrepresentable = Unrepresentable;
    if (TheSema)
      TheSema->clearNameClassificationObserver();
    TheSema = nullptr;
  }

  /// \p Name is the target's name as the SourceManager spells it, which is
  /// the same spelling the tracker records.
  void targetEntered(llvm::StringRef Name, bool ThroughContext) {
    if (Out.TargetEntered)
      return;
    Out.TargetEntered = true;
    Out.ReachedThroughContext = ThroughContext;
    TargetName = Name.str();
    if (TheSema)
      recordEntry();
    else
      EnteredEarly = true;
  }

  void targetExited() {
    if (SawExit || ExitedEarly)
      return;
    if (TheSema)
      recordExit();
    else
      ExitedEarly = true;
  }

private:
  void recordEntry() {
    Entry = capturePragmaState(*TheSema);
    noteUnrepresentable();
  }

  void recordExit() {
    SawExit = true;
    Exit = capturePragmaState(*TheSema);
    noteUnrepresentable();
  }

  // Checked on both sides. The state the target was compiled under must be
  // expressible for the record to key on it, and the state the target leaves
  // must be expressible for the client to replay it.
  void noteUnrepresentable() {
    if (!Unrepresentable)
      Unrepresentable = findNonDefaultPragmaState(*TheSema);
  }

  ASTCacheDependencyTracker &Tracker;
  ProduceOutcome &Out;
  Sema *TheSema = nullptr;
  std::string TargetName;
  PragmaSnapshot Entry, Exit;
  const char *Unrepresentable = nullptr;
  bool EnteredEarly = false, ExitedEarly = false, SawExit = false;
};

/// Tells the observer when the target header is entered and left, and whether
/// the target was reached through the context rather than through the target
/// wrapper. Also notes a translation-unit-varying builtin expanded inside the
/// target.
class TargetWatcher : public PPCallbacks {
public:
  TargetWatcher(Preprocessor &PP, const FileEntry *Target,
                const FileEntry *TargetWrapper, ProducerObserver &Obs,
                ProduceOutcome &Out)
      : PP(PP), SM(PP.getSourceManager()), Target(Target),
        TargetWrapper(TargetWrapper), Obs(Obs), Out(Out) {}

  // A macro the target part defines or undefines is part of what including
  // the header does to its includer. Which macros is collected here; their
  // state is read once the target part ends.
  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *) override {
    if (InTargetPart)
      Touched.insert(MacroNameTok.getIdentifierInfo());
  }

  void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &,
                      const MacroDirective *) override {
    if (InTargetPart)
      Touched.insert(MacroNameTok.getIdentifierInfo());
  }

  void MacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                    SourceRange, const MacroArgs *) override {
    // Only the target's text counts. The context's declarations stay hidden
    // from the loading compile, and the server puts back the counter the
    // module file's load sets, so a counter value the context used never
    // reaches it. The target part is everything from the target wrapper on,
    // headers the target includes among it.
    if (!InTargetPart || !Out.VaryingBuiltin.empty())
      return;
    const MacroInfo *MI = MD.getMacroInfo();
    if (!MI || !MI->isBuiltinMacro())
      return;
    llvm::StringRef Name = MacroNameTok.getIdentifierInfo()->getName();
    if (llvm::is_contained(TranslationUnitVaryingBuiltins, Name))
      Out.VaryingBuiltin = Name.str();
  }

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID PrevFID) override {
    if (Reason == EnterFile) {
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(SM.getFileID(Loc));
      if (!FE)
        return;
      if (&FE->getFileEntry() == TargetWrapper)
        InTargetPart = true;
      // FileManager gives one FileEntry per file on disk, so this matches the
      // target however an #include happened to spell its path.
      if (&FE->getFileEntry() == Target)
        Obs.targetEntered(FE->getName(), /*ThroughContext=*/!InTargetPart);
      return;
    }
    if (Reason == ExitFile) {
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(PrevFID);
      if (FE && &FE->getFileEntry() == Target)
        Obs.targetExited();
      // Nothing after the target wrapper belongs to the target, so every
      // touched macro now holds the state the header leaves it in.
      if (FE && &FE->getFileEntry() == TargetWrapper && InTargetPart) {
        for (const IdentifierInfo *II : Touched)
          Out.MacroEffects.push_back(
              {II->getName().str(), PP.isMacroDefined(II)});
        InTargetPart = false;
      }
    }
  }

private:
  Preprocessor &PP;
  SourceManager &SM;
  const FileEntry *Target;
  const FileEntry *TargetWrapper;
  ProducerObserver &Obs;
  ProduceOutcome &Out;
  bool InTargetPart = false;
  llvm::SetVector<const IdentifierInfo *> Touched;
};

/// Checks that each replayed #include opened the file the compile being
/// reproduced opened. A spelling such as <stdint.h> is resolved again by
/// searching, and the search can land elsewhere -- say the -I flags are in a
/// different order. A copy built from a different file would describe a
/// compile that never happened.
class ReplayChecker : public PPCallbacks {
public:
  struct Expectation {
    /// Null: not checked.
    const FileEntry *Want;
    /// The replayed text and the expected path, for the decline message.
    std::string Text;
    std::string WantPath;
  };

  ReplayChecker(SourceManager &SM, const FileEntry *ContextWrapper,
                const FileEntry *TargetWrapper,
                std::vector<Expectation> Context, Expectation Target,
                std::string &Mismatch)
      : SM(SM), ContextWrapper(ContextWrapper), TargetWrapper(TargetWrapper),
        Context(std::move(Context)), Target(std::move(Target)),
        Mismatch(Mismatch) {}

  void InclusionDirective(SourceLocation HashLoc, const Token &, StringRef,
                          bool, CharSourceRange, OptionalFileEntryRef File,
                          StringRef, StringRef, const Module *, bool,
                          SrcMgr::CharacteristicKind) override {
    OptionalFileEntryRef In = SM.getFileEntryRefForID(SM.getFileID(HashLoc));
    if (!In)
      return;
    // The context wrapper holds one #include line per context entry, in
    // order. This callback fires even when an include guard turns the
    // #include into a no-op, so the count stays in step.
    const Expectation *E = nullptr;
    if (&In->getFileEntry() == ContextWrapper) {
      if (NextContext < Context.size())
        E = &Context[NextContext++];
    } else if (&In->getFileEntry() == TargetWrapper) {
      E = &Target;
    }
    // A file that was not found at all is reported by the compile itself.
    if (!E || !E->Want || !File || !Mismatch.empty())
      return;
    if (&File->getFileEntry() != E->Want)
      Mismatch = "replaying #include " + E->Text + " opened " +
                 File->getName().str() +
                 ", but the compile being reproduced opened " + E->WantPath;
  }

private:
  SourceManager &SM;
  const FileEntry *ContextWrapper;
  const FileEntry *TargetWrapper;
  std::vector<Expectation> Context;
  Expectation Target;
  std::string &Mismatch;
  size_t NextContext = 0;
};

/// Builds a module from the generated module map, with the tracker, the Sema
/// observer, the target watcher, and the replay checker attached.
class ProducerAction : public GenerateModuleFromModuleMapAction {
public:
  ProducerAction(const ProduceRequest &Req, std::string ContextWrapperPath,
                 std::string TargetWrapperPath)
      : Req(Req), ContextWrapperPath(std::move(ContextWrapperPath)),
        TargetWrapperPath(std::move(TargetWrapperPath)) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) override {
    std::unique_ptr<ASTConsumer> Writer =
        GenerateModuleFromModuleMapAction::CreateASTConsumer(CI, InFile);
    if (!Writer)
      return nullptr;

    FileManager &FM = CI.getFileManager();
    auto entry = [&](llvm::StringRef P) -> const FileEntry * {
      OptionalFileEntryRef F = FM.getOptionalFileRef(P);
      return F ? &F->getFileEntry() : nullptr;
    };
    const FileEntry *Target = entry(Req.Header.Path);
    const FileEntry *ContextWrapper = entry(ContextWrapperPath);
    const FileEntry *TargetWrapper = entry(TargetWrapperPath);
    if (!Target || !ContextWrapper || !TargetWrapper)
      return nullptr;

    std::vector<ReplayChecker::Expectation> Context;
    for (const ContextEntry &E : Req.Context) {
      if (!E.isInclude())
        continue;
      const IncludeRef &C = E.Include;
      const FileEntry *Want = nullptr;
      if (!C.Path.empty() && !(Want = entry(C.Path)))
        return nullptr;
      Context.push_back({Want, replayText(C), C.Path});
    }

    // Attached here rather than in BeginSourceFileAction, which the base class
    // keeps private. This still runs during BeginSourceFile, before the parser
    // reads anything, so no event is missed.
    Preprocessor &PP = CI.getPreprocessor();
    auto OwnedTracker = std::make_unique<ASTCacheDependencyTracker>(PP);
    ASTCacheDependencyTracker &Tracker = *OwnedTracker;
    PP.addPPCallbacks(std::move(OwnedTracker));
    // The tracker lives as long as PP, which now owns it.
    PP.setExpansionCandidateObserver(Tracker);

    PP.addPPCallbacks(std::make_unique<ReplayChecker>(
        CI.getSourceManager(), ContextWrapper, TargetWrapper,
        std::move(Context),
        ReplayChecker::Expectation{Target, replayText(Req.Header),
                                   Req.Header.Path},
        Outcome.ReplayMismatch));

    auto Obs = std::make_unique<ProducerObserver>(Tracker, Outcome);
    PP.addPPCallbacks(std::make_unique<TargetWatcher>(PP, Target, TargetWrapper,
                                                      *Obs, Outcome));

    std::vector<std::unique_ptr<ASTConsumer>> Consumers;
    // The observer goes first so that it reads Sema's state before the writer
    // runs and the compile is torn down.
    Consumers.push_back(std::move(Obs));
    Consumers.push_back(std::move(Writer));
    return std::make_unique<MultiplexConsumer>(std::move(Consumers));
  }

  ProduceOutcome Outcome;

private:
  ProduceRequest Req;
  std::string ContextWrapperPath;
  std::string TargetWrapperPath;
};

ProduceResult declined(std::string Reason) {
  ProduceResult R;
  R.Stat = ProduceResult::Status::Declined;
  R.Reason = std::move(Reason);
  return R;
}

ProduceResult failed(std::string Reason) {
  ProduceResult R;
  R.Stat = ProduceResult::Status::Failed;
  R.Reason = std::move(Reason);
  return R;
}

/// Keeps the first error text, so a decline can say what actually went wrong
/// instead of only that something did.
class FirstErrorCollector : public DiagnosticConsumer {
public:
  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info);
    if (Level < DiagnosticsEngine::Error || !First.empty())
      return;
    llvm::SmallString<256> Text;
    Info.FormatDiagnostic(Text);
    First = Text.str().str();
  }
  std::string First;
};

/// Removes a directory and everything under it when the scope ends.
struct ScopedDir {
  llvm::SmallString<128> Path;
  ~ScopedDir() {
    if (!Path.empty())
      llvm::sys::fs::remove_directories(Path);
  }
};

/// Makes \p P absolute and removes "." and ".." components, so every file is
/// named one way in the generated wrappers and in the module-name hash.
std::error_code normalize(std::string &P) {
  llvm::SmallString<256> S(P);
  if (std::error_code EC = llvm::sys::fs::make_absolute(S))
    return EC;
  llvm::sys::path::remove_dots(S, /*remove_dot_dot=*/true);
  P = std::string(S);
  return {};
}

/// A path is written into a generated #include "..." line, so a path
/// containing a quote or a line break cannot be represented there.
bool writableInInclude(llvm::StringRef P) {
  return P.find_first_of("\"\n\r") == llvm::StringRef::npos;
}

/// The directive that replays \p E in the context wrapper.
std::string contextLine(const ContextEntry &E) {
  switch (E.K) {
  case ContextEntry::Kind::Include:
    return "#include " + replayText(E.Include);
  case ContextEntry::Kind::Define:
    return "#define " + E.Macro;
  case ContextEntry::Kind::Undef:
    return "#undef " + E.Macro;
  }
  llvm_unreachable("unknown context entry kind");
}

/// A #define or #undef line is written into the wrapper as one line, so its
/// text cannot contain a line break, and it must name a macro.
std::optional<std::string> checkMacroLine(const ContextEntry &E) {
  llvm::StringRef Text = E.Macro;
  if (Text.find_first_of("\n\r") != llvm::StringRef::npos)
    return "a context macro line contains a line break: " + E.Macro;
  llvm::StringRef Name = Text.take_while(
      [](char C) { return llvm::isAlnum(C) || C == '_' || C == '$'; });
  if (Name.empty() || llvm::isDigit(Name.front()))
    return "a context macro line does not start with a name: " + E.Macro;
  if (E.K == ContextEntry::Kind::Undef && Name.size() != Text.size())
    return "a context #undef names more than a macro: " + E.Macro;
  return std::nullopt;
}

/// Normalizes \p I's path and checks that \p I can be replayed. Returns what
/// is wrong, if anything.
std::optional<std::string> prepare(IncludeRef &I, llvm::StringRef What) {
  if (!I.Spelled.empty() && !validSpelling(I.Spelled))
    return "not an #include spelling: " + I.Spelled;
  if (I.Path.empty())
    return std::nullopt;
  if (std::error_code EC = normalize(I.Path))
    return ("resolving " + What + " path: " + EC.message()).str();
  if (!llvm::sys::fs::exists(I.Path))
    return ("no such " + What + ": " + I.Path).str();
  // The path itself goes into the replay only when there is no spelling.
  if (I.Spelled.empty() && !writableInInclude(I.Path))
    return "path cannot be written into an #include: " + I.Path;
  return std::nullopt;
}

llvm::Error writeFile(llvm::StringRef Path, llvm::StringRef Text) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC)
    return llvm::createStringError(EC, "writing '%s'", Path.str().c_str());
  OS << Text;
  return llvm::Error::success();
}

} // namespace

std::string clang::ast_cache::moduleNameFor(const ProduceRequest &Req) {
  // Each field is tagged and NUL-terminated, so ("ab", "c") and ("a", "bc")
  // cannot hash to the same key.
  std::string Key;
  auto add = [&](llvm::StringRef Tag, llvm::StringRef Value) {
    Key += Tag;
    Key.push_back('\0');
    Key += Value;
    Key.push_back('\0');
  };
  // The replayed text as well as the path: <stdint.h> and its resolved path
  // replay differently (see IncludeRef::Spelled), so they are different
  // requests.
  add("header", replayText(Req.Header));
  add("header-path", Req.Header.Path);
  for (const std::string &A : Req.Args)
    add("arg", A);
  for (const ContextEntry &E : Req.Context) {
    switch (E.K) {
    case ContextEntry::Kind::Include:
      add("context", replayText(E.Include));
      add("context-path", E.Include.Path);
      break;
    case ContextEntry::Kind::Define:
      add("define", E.Macro);
      break;
    case ContextEntry::Kind::Undef:
      add("undef", E.Macro);
      break;
    }
  }

  std::string Name;
  {
    llvm::raw_string_ostream OS(Name);
    OS << "__hc_" << llvm::format_hex_no_prefix(llvm::xxh3_64bits(Key), 16);
  }
  return Name;
}

ProduceResult clang::ast_cache::produceHeader(const ProduceRequest &In) {
  ProduceRequest Req = In;
  if (Req.Header.Path.empty())
    return failed("the request names no header file");
  if (std::optional<std::string> Problem = prepare(Req.Header, "file"))
    return failed(*Problem);
  for (ContextEntry &E : Req.Context) {
    if (!E.isInclude()) {
      if (std::optional<std::string> Problem = checkMacroLine(E))
        return failed(*Problem);
      continue;
    }
    IncludeRef &C = E.Include;
    if (C.Spelled.empty() && C.Path.empty())
      return failed("a context entry has neither a spelling nor a file");
    if (std::optional<std::string> Problem = prepare(C, "context file"))
      return failed(*Problem);
  }

  std::string Name = moduleNameFor(Req);

  ScopedDir Work;
  if (std::error_code EC =
          llvm::sys::fs::createUniqueDirectory("clang-header-cache", Work.Path))
    return failed("creating work directory: " + EC.message());

  llvm::SmallString<256> ContextWrapper(Work.Path);
  llvm::sys::path::append(ContextWrapper, "__hc_context.h");
  llvm::SmallString<256> TargetWrapper(Work.Path);
  llvm::sys::path::append(TargetWrapper, "__hc_target.h");
  llvm::SmallString<256> MapPath(Work.Path);
  llvm::sys::path::append(MapPath, "module.modulemap");

  // The context goes in as #include lines in a wrapper header, not as
  // -include flags. -include lists are among the preprocessor options the
  // AST reader validates when a saved copy is loaded; keeping context out of
  // them means the saved copy's options are exactly the compile's own.
  //
  // Each #include is replayed the way the compile being reproduced found its
  // file; IncludeRef::Spelled says why a path is not enough.
  std::string ContextText;
  for (const ContextEntry &E : Req.Context)
    ContextText += contextLine(E) + "\n";
  std::string TargetText = "#include " + replayText(Req.Header) + "\n";

  std::string MapText;
  {
    llvm::raw_string_ostream OS(MapText);
    OS << "module " << Name << " {\n"
       << "  explicit module context {\n"
       << "    header \"" << ContextWrapper << "\"\n"
       << "  }\n"
       << "  module " << TargetSubmoduleName << " {\n"
       << "    header \"" << TargetWrapper << "\"\n"
       << "    export *\n"
       << "  }\n"
       << "}\n";
  }

  for (auto [Path, Text] :
       {std::pair<llvm::StringRef, llvm::StringRef>{ContextWrapper, ContextText},
        {TargetWrapper, TargetText},
        {MapPath, MapText}})
    if (llvm::Error E = writeFile(Path, Text))
      return failed(llvm::toString(std::move(E)));

  llvm::SmallString<256> OutPath(Work.Path);
  llvm::sys::path::append(OutPath, "out.pcm");

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS =
      llvm::vfs::createPhysicalFileSystem();
  DiagnosticOptions DiagOpts;
  // Diagnostics are collected rather than printed: a header that does not
  // compile in the given context is an ordinary decline, not something to
  // spray at stderr.
  auto *Collector = new FirstErrorCollector();
  llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(*VFS, DiagOpts, Collector);

  CreateInvocationOptions CIOpts;
  CIOpts.VFS = VFS;
  CIOpts.Diags = Diags;

  // Without this the compile has no path to Clang's own headers. The driver
  // normally derives it from argv[0], which here is the populating tool, not
  // clang, so it has to be supplied.
  static int Anchor = 0;
  std::string ResourceDir =
      GetResourcesPath("clang", reinterpret_cast<void *>(&Anchor));

  std::vector<std::string> ArgStrings = {
      "clang",
      "-resource-dir",
      ResourceDir,
      // Every compile involved in caching runs in this mode: a saved copy is
      // a module, and only a compile with modules on can load one. The
      // second flag stops Clang from turning headers that have a nearby
      // module.modulemap -- Clang's own builtin headers among them -- into
      // modules, so everything except the saved copy stays textual.
      "-fmodules",
      "-fno-implicit-module-maps",
      "-fmodule-name=" + Name,
      "-Xclang",
      "-emit-module",
      // The three files below live in a directory this function deletes. A
      // saved copy records them as the files its declarations were reached
      // through, so a compile that later touches a header the copy also
      // contains needs them on disk and dies with "could not find file".
      // Embedding stores their text in the copy instead.
      "-Xclang",
      "-fmodules-embed-file=" + std::string(ContextWrapper),
      "-Xclang",
      "-fmodules-embed-file=" + std::string(TargetWrapper),
      "-Xclang",
      "-fmodules-embed-file=" + std::string(MapPath),
      // Without -c the driver appends a link step and fails on a .pcm.
      "-c",
      "-x",
      "c",
  };
  ArgStrings.insert(ArgStrings.end(), Req.Args.begin(), Req.Args.end());
  ArgStrings.push_back(std::string(MapPath));

  std::vector<const char *> Args;
  Args.reserve(ArgStrings.size());
  for (const std::string &A : ArgStrings)
    Args.push_back(A.c_str());

  std::shared_ptr<CompilerInvocation> Invocation =
      createInvocation(Args, CIOpts);
  if (!Invocation)
    return failed("could not build a compiler invocation");
  Invocation->getFrontendOpts().DisableFree = false;
  Invocation->getFrontendOpts().OutputFile = std::string(OutPath);

  CompilerInstance CI(std::move(Invocation));
  CI.setDiagnostics(Diags);
  // The collector keeps diagnostics quiet, but the "N warnings and M errors
  // generated." summary is printed separately, to this stream.
  CI.setVerboseOutputStream(llvm::nulls());
  CI.createVirtualFileSystem(VFS);
  CI.createFileManager();

  ProducerAction Action(Req, std::string(ContextWrapper),
                        std::string(TargetWrapper));
  bool Ok = CI.ExecuteAction(Action);
  const ProduceOutcome &O = Action.Outcome;
  // Checked first: when a replay opened a different file, any error that
  // followed is a consequence, and the mismatch is what to report.
  if (!O.ReplayMismatch.empty())
    return declined(O.ReplayMismatch);
  if (!Ok || Diags->hasErrorOccurred())
    return declined("does not compile in this context: " +
                    (Collector->First.empty() ? std::string("(no diagnostic)")
                                              : Collector->First));

  if (!O.TargetEntered)
    return failed("the header was never entered");
  if (O.ReachedThroughContext)
    return declined("already included by its own context: a replayed "
                    "#include reaches the header before the target does");
  if (!O.Unrepresentable.empty())
    return declined("compiled under, or leaves, #pragma " + O.Unrepresentable +
                    " in a state a record cannot carry");
  // A saved copy holding __COUNTER__'s values 0 and 1 would be wrong in a
  // compile whose counter is already at 7. Declined until the backfill, which
  // rewrites such values on load, exists.
  if (!O.VaryingBuiltin.empty())
    return declined("expands " + O.VaryingBuiltin +
                    ", whose value depends on the compile that includes the "
                    "header; not saved until the backfill exists");
  // A frame is dropped when the tracker could not describe it soundly, so a
  // missing record means the header used something untracked -- currently a
  // C++ construct. That must cost a miss, never a wrong hit.
  if (!O.HaveRecord)
    return declined("used a construct the recorder does not track");

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
      llvm::MemoryBuffer::getFile(OutPath, /*IsText=*/false);
  if (!Buf)
    return failed("reading the produced module: " + Buf.getError().message());

  ProduceResult R;
  R.Stat = ProduceResult::Status::Produced;
  R.ModuleName = Name;
  R.Push.Filename = Req.Header.Path;
  R.Push.ModuleName = Name;
  R.Push.Deps = O.Deps;
  R.Push.Deps.Entry = O.Entry;
  R.Push.Deps.Exit = O.Exit;
  R.Push.Deps.MacroEffects = O.MacroEffects;
  R.Push.Data = (*Buf)->getBuffer().str();
  return R;
}

//===----------------------------------------------------------------------===//
// Job files
//===----------------------------------------------------------------------===//

static constexpr llvm::StringLiteral JobFileMagic = "clang-header-cache-jobs 3";
/// The version before define and undef lines, still read.
static constexpr llvm::StringLiteral JobFileMagicV2 =
    "clang-header-cache-jobs 2";

static llvm::Error jobError(const llvm::Twine &Msg) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
}

/// Splits a header or context value into its optional spelling and its path.
static llvm::Expected<IncludeRef> parseRef(llvm::StringRef V, size_t LineNo) {
  IncludeRef I;
  if (!V.starts_with("<") && !V.starts_with("\"")) {
    I.Path = V.str();
    return I;
  }
  char Close = V.front() == '<' ? '>' : '"';
  size_t End = V.find(Close, 1);
  if (End == llvm::StringRef::npos)
    return jobError("line " + llvm::Twine(LineNo) + ": unterminated spelling");
  I.Spelled = V.take_front(End + 1).str();
  if (!validSpelling(I.Spelled))
    return jobError("line " + llvm::Twine(LineNo) +
                    ": not an #include spelling: " + I.Spelled);
  llvm::StringRef Rest = V.drop_front(End + 1);
  if (!Rest.empty() && !Rest.consume_front(" "))
    return jobError("line " + llvm::Twine(LineNo) +
                    ": expected a space after the spelling");
  I.Path = Rest.str();
  return I;
}

llvm::Expected<std::string>
clang::ast_cache::encodeJobs(llvm::ArrayRef<ProduceRequest> Jobs) {
  auto oneLine = [](llvm::StringRef V) {
    return V.find_first_of("\n\r") == llvm::StringRef::npos;
  };
  // parseRef tells a spelling from a path by the first character, so a path
  // cannot start the way a spelling does.
  auto ref = [&](const IncludeRef &I,
                 llvm::StringRef What) -> llvm::Expected<std::string> {
    if (I.Spelled.empty() && I.Path.empty())
      return jobError("a " + What + " entry has neither a spelling nor a file");
    if (!oneLine(I.Spelled) || !oneLine(I.Path))
      return jobError(What + " contains a line break: " + I.Spelled + " " +
                      I.Path);
    if (!I.Spelled.empty() && !validSpelling(I.Spelled))
      return jobError("not an #include spelling: " + I.Spelled);
    if (llvm::StringRef(I.Path).starts_with("<") ||
        llvm::StringRef(I.Path).starts_with("\""))
      return jobError(What + " path starts like a spelling: " + I.Path);
    if (I.Spelled.empty())
      return I.Path;
    return I.Path.empty() ? I.Spelled : I.Spelled + " " + I.Path;
  };

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << JobFileMagic << "\n";
  for (const ProduceRequest &J : Jobs) {
    if (J.Header.Path.empty())
      return jobError("a job has no header");
    llvm::Expected<std::string> Header = ref(J.Header, "header");
    if (!Header)
      return Header.takeError();
    for (const std::string &A : J.Args)
      if (!oneLine(A))
        return jobError("argument contains a line break: " + A);
    std::vector<std::string> Context;
    for (const ContextEntry &E : J.Context) {
      if (!E.isInclude()) {
        if (std::optional<std::string> Problem = checkMacroLine(E))
          return jobError(*Problem);
        Context.push_back(
            (E.K == ContextEntry::Kind::Define ? "define " : "undef ") +
            E.Macro);
        continue;
      }
      llvm::Expected<std::string> V = ref(E.Include, "context");
      if (!V)
        return V.takeError();
      Context.push_back("context " + *V);
    }

    OS << "job\n" << "header " << *Header << "\n";
    for (const std::string &A : J.Args)
      OS << "arg " << A << "\n";
    for (const std::string &C : Context)
      OS << C << "\n";
    OS << "end\n";
  }
  return Out;
}

llvm::Expected<std::vector<ProduceRequest>>
clang::ast_cache::decodeJobs(llvm::StringRef Text) {
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  Text.split(Lines, '\n', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  if (Lines.empty() || (Lines[0] != JobFileMagic && Lines[0] != JobFileMagicV2))
    return jobError("not a header cache job file");
  bool AllowMacros = Lines[0] == JobFileMagic;

  std::vector<ProduceRequest> Jobs;
  std::optional<ProduceRequest> Cur;
  for (size_t I = 1; I < Lines.size(); ++I) {
    llvm::StringRef L = Lines[I];
    if (L == "job") {
      if (Cur)
        return jobError("line " + llvm::Twine(I + 1) + ": job inside a job");
      Cur.emplace();
      continue;
    }
    if (L == "end") {
      if (!Cur)
        return jobError("line " + llvm::Twine(I + 1) + ": end with no job");
      if (Cur->Header.Path.empty())
        return jobError("line " + llvm::Twine(I + 1) + ": job has no header");
      Jobs.push_back(std::move(*Cur));
      Cur.reset();
      continue;
    }
    if (!Cur)
      return jobError("line " + llvm::Twine(I + 1) + ": field outside a job");
    if (L.consume_front("header ")) {
      if (!Cur->Header.Path.empty())
        return jobError("line " + llvm::Twine(I + 1) + ": second header");
      llvm::Expected<IncludeRef> R = parseRef(L, I + 1);
      if (!R)
        return R.takeError();
      if (R->Path.empty())
        return jobError("line " + llvm::Twine(I + 1) + ": header has no path");
      Cur->Header = std::move(*R);
    } else if (L.consume_front("arg ")) {
      Cur->Args.push_back(L.str());
    } else if (L.consume_front("context ")) {
      llvm::Expected<IncludeRef> R = parseRef(L, I + 1);
      if (!R)
        return R.takeError();
      if (R->Spelled.empty() && R->Path.empty())
        return jobError("line " + llvm::Twine(I + 1) + ": empty context");
      Cur->Context.push_back(std::move(*R));
    } else if (AllowMacros &&
               (L.starts_with("define ") || L.starts_with("undef "))) {
      ContextEntry E = L.consume_front("define ")
                           ? ContextEntry::define(L.str())
                           : ContextEntry::undef(L.drop_front(6).str());
      if (std::optional<std::string> Problem = checkMacroLine(E))
        return jobError("line " + llvm::Twine(I + 1) + ": " + *Problem);
      Cur->Context.push_back(std::move(E));
    } else {
      return jobError("line " + llvm::Twine(I + 1) + ": unrecognised: " + L);
    }
  }
  if (Cur)
    return jobError("the last job has no end line");
  return Jobs;
}

llvm::Error clang::ast_cache::writeJobFile(llvm::StringRef Path,
                                           llvm::ArrayRef<ProduceRequest> Jobs) {
  llvm::Expected<std::string> Text = encodeJobs(Jobs);
  if (!Text)
    return Text.takeError();
  return writeFile(Path, *Text);
}

llvm::Expected<std::vector<ProduceRequest>>
clang::ast_cache::readJobFile(llvm::StringRef Path) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
      llvm::MemoryBuffer::getFile(Path, /*IsText=*/true);
  if (!Buf)
    return llvm::createStringError(Buf.getError(), "reading '%s'",
                                   Path.str().c_str());
  return decodeJobs((*Buf)->getBuffer());
}
