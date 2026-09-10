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
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Options/OptionUtils.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Rides alongside the module writer to observe Sema, which the dependency
/// tracker cannot: the tracker is a PPCallbacks and sees only the
/// preprocessor.
///
/// Three jobs. It registers the tracker as the name-classification observer,
/// so symbol dependencies are recorded. It snapshots pragma state at the
/// start and at the end, which become the record's Entry and Exit. And it
/// checks, while Sema is still alive, whether any pragma state was left in a
/// form a snapshot cannot express.
class ProducerObserver : public SemaConsumer {
public:
  ProducerObserver(ASTCacheDependencyTracker &Tracker) : Tracker(Tracker) {}

  void InitializeSema(Sema &S) override {
    TheSema = &S;
    S.setNameClassificationObserver(Tracker);
    Entry = capturePragmaState(S);
  }

  void HandleTranslationUnit(ASTContext &) override {
    if (!TheSema)
      return;
    // Runs before the module writer's own HandleTranslationUnit, because this
    // consumer is first in the multiplex -- so Sema's pragma stacks still
    // hold what the header left them at.
    if (const char *P = findNonDefaultPragmaState(*TheSema))
      Unrepresentable = P;
    Exit = capturePragmaState(*TheSema);
    Tracker.finalize();
    TheSema->clearNameClassificationObserver();
    TheSema = nullptr;
  }

  PragmaSnapshot Entry, Exit;
  /// Non-null names a pragma whose state cannot be carried in a record.
  const char *Unrepresentable = nullptr;

private:
  ASTCacheDependencyTracker &Tracker;
  Sema *TheSema = nullptr;
};

/// Builds a module from the synthesized map, with the tracker and the Sema
/// observer attached.
class ProducerAction : public GenerateModuleFromModuleMapAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) override {
    std::unique_ptr<ASTConsumer> Writer =
        GenerateModuleFromModuleMapAction::CreateASTConsumer(CI, InFile);
    if (!Writer)
      return nullptr;

    // Attached here rather than in BeginSourceFileAction, which the base
    // class keeps private. This still runs during BeginSourceFile, before
    // the parser reads anything, so no event is missed.
    auto Owned =
        std::make_unique<ASTCacheDependencyTracker>(CI.getPreprocessor());
    Tracker = Owned.get();
    CI.getPreprocessor().addPPCallbacks(std::move(Owned));

    auto Obs = std::make_unique<ProducerObserver>(*Tracker);
    Observer = Obs.get();

    std::vector<std::unique_ptr<ASTConsumer>> Consumers;
    // The observer goes first so it reads Sema's pragma state before the
    // writer runs and the state is torn down.
    Consumers.push_back(std::move(Obs));
    Consumers.push_back(std::move(Writer));
    return std::make_unique<MultiplexConsumer>(std::move(Consumers));
  }

  ASTCacheDependencyTracker *Tracker = nullptr;
  ProducerObserver *Observer = nullptr;
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
/// instead of only that something did. Without this, "does not compile on its
/// own" is indistinguishable from a bug in this file.
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

/// Where implicitly-built modules for Clang's own builtin headers are kept.
/// Shared across every header this process produces, and across runs, so the
/// builtin modules are built once rather than per header.
std::string moduleCachePath() {
  llvm::SmallString<128> P;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, P);
  llvm::sys::path::append(P, "clang-header-cache-modules");
  return std::string(P);
}

/// Removes a directory and everything under it when the scope ends.
struct ScopedDir {
  llvm::SmallString<128> Path;
  ~ScopedDir() {
    if (!Path.empty())
      llvm::sys::fs::remove_directories(Path);
  }
};

} // namespace

ProduceResult
clang::ast_cache::produceHeader(llvm::StringRef HeaderPath,
                                llvm::ArrayRef<std::string> ExtraArgs) {
  llvm::SmallString<256> AbsHeader(HeaderPath);
  if (std::error_code EC = llvm::sys::fs::make_absolute(AbsHeader))
    return failed("resolving header path: " + EC.message());
  if (!llvm::sys::fs::exists(AbsHeader))
    return failed("no such file: " + std::string(AbsHeader));

  ScopedDir Work;
  if (std::error_code EC =
          llvm::sys::fs::createUniqueDirectory("clang-header-cache", Work.Path))
    return failed("creating work directory: " + EC.message());

  // A module map is how a plain C header becomes a module. The module name is
  // internal and never observed by a consumer -- the cache is keyed on the
  // header's path, not on this name -- so a fixed one is fine.
  llvm::SmallString<256> MapPath(Work.Path);
  llvm::sys::path::append(MapPath, "module.modulemap");
  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(MapPath, EC);
    if (EC)
      return failed("writing module map: " + EC.message());
    OS << "module __clang_header_cache {\n"
       << "  header \"" << AbsHeader << "\"\n"
       << "  export *\n"
       << "}\n";
  }

  llvm::SmallString<256> OutPath(Work.Path);
  llvm::sys::path::append(OutPath, "out.pcm");

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS =
      llvm::vfs::createPhysicalFileSystem();
  DiagnosticOptions DiagOpts;
  // Diagnostics are collected rather than printed: a header that fails to
  // compile alone is an ordinary decline, not something to spray at stderr.
  auto *Collector = new FirstErrorCollector();
  llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(*VFS, DiagOpts, Collector);

  CreateInvocationOptions CIOpts;
  CIOpts.VFS = VFS;
  CIOpts.Diags = Diags;

  std::string MapStr(MapPath);

  // Without this the compile has no path to Clang's own headers, so a header
  // that includes <stddef.h> fails and looks like it is not self-contained.
  // The driver normally derives this from argv[0], which is this tool rather
  // than clang, so it has to be supplied.
  static int Anchor = 0;
  std::string ResourceDir =
      GetResourcesPath("clang", reinterpret_cast<void *>(&Anchor));

  std::vector<std::string> ArgStrings = {
      "clang",
      "-resource-dir",
      ResourceDir,
      "-fmodules",
      "-fmodule-name=__clang_header_cache",
      // Implicit modules stay enabled: Clang's own builtin headers
      // (stddef.h, stdarg.h) live in module maps of their own, so a header
      // that includes one needs those modules built. They land in the shared
      // cache below and are built once, not per header.
      "-fmodules-cache-path=" + moduleCachePath(),
      "-Xclang", "-emit-module",
      // Without -c the driver appends a link step and fails on a .pcm.
      "-c",
      "-x", "c",
  };
  ArgStrings.insert(ArgStrings.end(), ExtraArgs.begin(), ExtraArgs.end());
  ArgStrings.push_back(MapStr);

  std::vector<const char *> Args;
  Args.reserve(ArgStrings.size());
  for (const std::string &A : ArgStrings)
    Args.push_back(A.c_str());

  std::shared_ptr<CompilerInvocation> Invocation = createInvocation(Args, CIOpts);
  if (!Invocation)
    return failed("could not build a compiler invocation");
  Invocation->getFrontendOpts().DisableFree = false;
  Invocation->getFrontendOpts().OutputFile = std::string(OutPath);

  CompilerInstance CI(std::move(Invocation));
  CI.setDiagnostics(Diags);
  CI.createVirtualFileSystem(VFS);
  CI.createFileManager();

  ProducerAction Action;
  bool Ok = CI.ExecuteAction(Action);
  if (!Ok || Diags->hasErrorOccurred())
    return declined("does not compile on its own: " +
                    (Collector->First.empty() ? std::string("(no diagnostic)")
                                              : Collector->First));

  if (!Action.Observer)
    return failed("the compile produced no Sema observer");
  if (const char *P = Action.Observer->Unrepresentable)
    return declined(std::string("leaves #pragma ") + P +
                    " in a state a record cannot carry");

  // A frame is dropped when the tracker could not describe it soundly, so a
  // missing record means the header used something untracked -- currently a
  // C++ construct. That must cost a miss, never a wrong hit.
  const HeaderDependencySet *Deps = Action.Tracker->findByPath(AbsHeader);
  if (!Deps)
    return declined("used a construct the recorder does not track");

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
      llvm::MemoryBuffer::getFile(OutPath, /*IsText=*/false);
  if (!Buf)
    return failed("reading the produced module: " + Buf.getError().message());

  ProduceResult R;
  R.Stat = ProduceResult::Status::Produced;
  R.Push.Filename = std::string(AbsHeader);
  R.Push.Deps = *Deps;
  R.Push.Deps.Entry = Action.Observer->Entry;
  R.Push.Deps.Exit = Action.Observer->Exit;
  R.Push.Data = (*Buf)->getBuffer().str();
  return R;
}
