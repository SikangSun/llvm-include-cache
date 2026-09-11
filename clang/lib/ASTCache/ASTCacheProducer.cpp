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
};

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
/// wrapper.
class TargetWatcher : public PPCallbacks {
public:
  TargetWatcher(SourceManager &SM, const FileEntry *Target,
                const FileEntry *TargetWrapper, ProducerObserver &Obs)
      : SM(SM), Target(Target), TargetWrapper(TargetWrapper), Obs(Obs) {}

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
    }
  }

private:
  SourceManager &SM;
  const FileEntry *Target;
  const FileEntry *TargetWrapper;
  ProducerObserver &Obs;
  bool InTargetPart = false;
};

/// Builds a module from the generated module map, with the tracker, the Sema
/// observer, and the target watcher attached.
class ProducerAction : public GenerateModuleFromModuleMapAction {
public:
  ProducerAction(std::string TargetPath, std::string TargetWrapperPath)
      : TargetPath(std::move(TargetPath)),
        TargetWrapperPath(std::move(TargetWrapperPath)) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) override {
    std::unique_ptr<ASTConsumer> Writer =
        GenerateModuleFromModuleMapAction::CreateASTConsumer(CI, InFile);
    if (!Writer)
      return nullptr;

    FileManager &FM = CI.getFileManager();
    OptionalFileEntryRef Target = FM.getOptionalFileRef(TargetPath);
    OptionalFileEntryRef Wrapper = FM.getOptionalFileRef(TargetWrapperPath);
    if (!Target || !Wrapper)
      return nullptr;

    // Attached here rather than in BeginSourceFileAction, which the base class
    // keeps private. This still runs during BeginSourceFile, before the parser
    // reads anything, so no event is missed.
    Preprocessor &PP = CI.getPreprocessor();
    auto OwnedTracker = std::make_unique<ASTCacheDependencyTracker>(PP);
    ASTCacheDependencyTracker &Tracker = *OwnedTracker;
    PP.addPPCallbacks(std::move(OwnedTracker));

    auto Obs = std::make_unique<ProducerObserver>(Tracker, Outcome);
    PP.addPPCallbacks(std::make_unique<TargetWatcher>(
        CI.getSourceManager(), &Target->getFileEntry(),
        &Wrapper->getFileEntry(), *Obs));

    std::vector<std::unique_ptr<ASTConsumer>> Consumers;
    // The observer goes first so that it reads Sema's state before the writer
    // runs and the compile is torn down.
    Consumers.push_back(std::move(Obs));
    Consumers.push_back(std::move(Writer));
    return std::make_unique<MultiplexConsumer>(std::move(Consumers));
  }

  ProduceOutcome Outcome;

private:
  std::string TargetPath;
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
  add("header", Req.HeaderPath);
  for (const std::string &A : Req.Args)
    add("arg", A);
  for (const std::string &C : Req.Context)
    add("context", C);

  std::string Name;
  {
    llvm::raw_string_ostream OS(Name);
    OS << "__hc_" << llvm::format_hex_no_prefix(llvm::xxh3_64bits(Key), 16);
  }
  return Name;
}

ProduceResult clang::ast_cache::produceHeader(const ProduceRequest &In) {
  ProduceRequest Req = In;
  if (std::error_code EC = normalize(Req.HeaderPath))
    return failed("resolving header path: " + EC.message());
  if (!llvm::sys::fs::exists(Req.HeaderPath))
    return failed("no such file: " + Req.HeaderPath);
  if (!writableInInclude(Req.HeaderPath))
    return failed("path cannot be written into an #include: " +
                  Req.HeaderPath);
  for (std::string &C : Req.Context) {
    if (std::error_code EC = normalize(C))
      return failed("resolving context path: " + EC.message());
    if (!llvm::sys::fs::exists(C))
      return failed("no such context file: " + C);
    if (!writableInInclude(C))
      return failed("path cannot be written into an #include: " + C);
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
  std::string ContextText;
  for (const std::string &C : Req.Context)
    ContextText += "#include \"" + C + "\"\n";
  std::string TargetText = "#include \"" + Req.HeaderPath + "\"\n";

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

  ProducerAction Action(Req.HeaderPath, std::string(TargetWrapper));
  bool Ok = CI.ExecuteAction(Action);
  if (!Ok || Diags->hasErrorOccurred())
    return declined("does not compile in this context: " +
                    (Collector->First.empty() ? std::string("(no diagnostic)")
                                              : Collector->First));

  const ProduceOutcome &O = Action.Outcome;
  if (!O.TargetEntered)
    return failed("the header was never entered");
  if (O.ReachedThroughContext)
    return declined("already included by its own context -- a header reached "
                    "from inside another is saved as part of that one's copy");
  if (!O.Unrepresentable.empty())
    return declined("compiled under, or leaves, #pragma " + O.Unrepresentable +
                    " in a state a record cannot carry");
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
  R.Push.Filename = Req.HeaderPath;
  R.Push.Deps = O.Deps;
  R.Push.Deps.Entry = O.Entry;
  R.Push.Deps.Exit = O.Exit;
  R.Push.Data = (*Buf)->getBuffer().str();
  return R;
}

//===----------------------------------------------------------------------===//
// Job files
//===----------------------------------------------------------------------===//

static constexpr llvm::StringLiteral JobFileMagic = "clang-header-cache-jobs 1";

static llvm::Error jobError(const llvm::Twine &Msg) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Msg);
}

llvm::Expected<std::string>
clang::ast_cache::encodeJobs(llvm::ArrayRef<ProduceRequest> Jobs) {
  auto oneLine = [](llvm::StringRef V) {
    return V.find_first_of("\n\r") == llvm::StringRef::npos;
  };

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << JobFileMagic << "\n";
  for (const ProduceRequest &J : Jobs) {
    if (J.HeaderPath.empty())
      return jobError("a job has no header");
    if (!oneLine(J.HeaderPath))
      return jobError("header path contains a line break: " + J.HeaderPath);
    for (const std::string &A : J.Args)
      if (!oneLine(A))
        return jobError("argument contains a line break: " + A);
    for (const std::string &C : J.Context)
      if (!oneLine(C))
        return jobError("context path contains a line break: " + C);

    OS << "job\n" << "header " << J.HeaderPath << "\n";
    for (const std::string &A : J.Args)
      OS << "arg " << A << "\n";
    for (const std::string &C : J.Context)
      OS << "context " << C << "\n";
    OS << "end\n";
  }
  return Out;
}

llvm::Expected<std::vector<ProduceRequest>>
clang::ast_cache::decodeJobs(llvm::StringRef Text) {
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  Text.split(Lines, '\n', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  if (Lines.empty() || Lines[0] != JobFileMagic)
    return jobError("not a header cache job file");

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
      if (Cur->HeaderPath.empty())
        return jobError("line " + llvm::Twine(I + 1) + ": job has no header");
      Jobs.push_back(std::move(*Cur));
      Cur.reset();
      continue;
    }
    if (!Cur)
      return jobError("line " + llvm::Twine(I + 1) + ": field outside a job");
    if (L.consume_front("header ")) {
      if (!Cur->HeaderPath.empty())
        return jobError("line " + llvm::Twine(I + 1) + ": second header");
      Cur->HeaderPath = L.str();
    } else if (L.consume_front("arg ")) {
      Cur->Args.push_back(L.str());
    } else if (L.consume_front("context ")) {
      Cur->Context.push_back(L.str());
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
