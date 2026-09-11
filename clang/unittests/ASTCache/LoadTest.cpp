//===- unittests/ASTCache/LoadTest.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Loading a saved copy the way the hit path will: from bytes in memory, with
// no file on disk, partway through a compile that has already parsed part of
// its .c file.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Pragma.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Options/OptionUtils.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ModuleManager.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// `#pragma hc_load` stands in for the hit path at an #include: it hands the
/// saved copy's bytes to the module reader under a name no file has, then
/// loads them, in the middle of the compile.
class LoadPragma : public PragmaHandler {
public:
  LoadPragma(CompilerInstance &CI, std::string Name, llvm::StringRef Bytes,
             bool &Loaded)
      : PragmaHandler("hc_load"), CI(CI), Name(std::move(Name)), Bytes(Bytes),
        Loaded(Loaded) {}

  void HandlePragma(Preprocessor &, PragmaIntroducer, Token &) override {
    if (!CI.getASTReader())
      CI.createASTReader();
    // Not copied: in the hit path the bytes stay in the daemon's shared
    // memory for the life of the compile.
    CI.getASTReader()->getModuleManager().addInMemoryBuffer(
        Name, llvm::MemoryBuffer::getMemBuffer(Bytes, Name,
                                               /*RequiresNullTerminator=*/false));
    serialization::ModuleFile *MF = nullptr;
    Loaded = CI.loadModuleFile(Name, MF);
  }

private:
  CompilerInstance &CI;
  std::string Name;
  llvm::StringRef Bytes;
  bool &Loaded;
};

class LoadingAction : public SyntaxOnlyAction {
public:
  LoadingAction(std::string Name, llvm::StringRef Bytes, bool &Loaded)
      : Name(std::move(Name)), Bytes(Bytes), Loaded(Loaded) {}

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    CI.getPreprocessor().AddPragmaHandler(
        new LoadPragma(CI, Name, Bytes, Loaded));
    return true;
  }

private:
  std::string Name;
  llvm::StringRef Bytes;
  bool &Loaded;
};

class ErrorText : public DiagnosticConsumer {
public:
  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info);
    if (Level < DiagnosticsEngine::Error)
      return;
    llvm::SmallString<256> T;
    Info.FormatDiagnostic(T);
    Text += T.str().str() + "\n";
  }
  std::string Text;
};

struct LoadResult {
  bool Loaded = false;
  bool Compiled = false;
  std::string Errors;
};

/// Compiles \p MainPath as a loading compile would be run: the same
/// -fmodules flags and the same resource directory as the producer's compile.
LoadResult compile(const std::string &MainPath, const std::string &Name,
                   llvm::StringRef Bytes) {
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS =
      llvm::vfs::createPhysicalFileSystem();
  DiagnosticOptions DiagOpts;
  auto *Errors = new ErrorText();
  llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(*VFS, DiagOpts, Errors);
  CreateInvocationOptions CIOpts;
  CIOpts.VFS = VFS;
  CIOpts.Diags = Diags;

  static int Anchor = 0;
  std::string ResourceDir =
      GetResourcesPath("clang", reinterpret_cast<void *>(&Anchor));
  std::vector<std::string> ArgStrings = {
      "clang",   "-resource-dir",           ResourceDir, "-fsyntax-only",
      "-fmodules", "-fno-implicit-module-maps", "-std=c17", "-x",
      "c",       MainPath};
  std::vector<const char *> Args;
  for (const std::string &A : ArgStrings)
    Args.push_back(A.c_str());

  LoadResult R;
  std::shared_ptr<CompilerInvocation> Invocation =
      createInvocation(Args, CIOpts);
  if (!Invocation) {
    R.Errors = "could not build a compiler invocation";
    return R;
  }
  Invocation->getFrontendOpts().DisableFree = false;
  CompilerInstance CI(std::move(Invocation));
  CI.setDiagnostics(Diags);
  CI.setVerboseOutputStream(llvm::nulls());
  CI.createVirtualFileSystem(VFS);
  CI.createFileManager();

  LoadingAction Action(Name, Bytes, R.Loaded);
  R.Compiled = CI.ExecuteAction(Action) && !Diags->hasErrorOccurred();
  R.Errors = Errors->Text;
  return R;
}

class LoadTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-load", D));
    Dir = std::string(D);
  }
  void TearDown() override { llvm::sys::fs::remove_directories(Dir); }

  std::string write(llvm::StringRef Name, llvm::StringRef Text) {
    llvm::SmallString<256> P(Dir);
    llvm::sys::path::append(P, Name);
    std::error_code EC;
    llvm::raw_fd_ostream OS(P, EC);
    EXPECT_FALSE(EC);
    OS << Text;
    return std::string(P);
  }

  std::string Dir;
};

TEST_F(LoadTest, ASavedCopyLoadsFromMemoryPartwayThroughACompile) {
  std::string Types =
      write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                       "struct point { int x, y; };\n"
                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "void draw(struct point *p);\n"
                                   "u32 count(void);\n"
                                   "#define USE_OK 1\n#endif\n");

  ProduceRequest Req;
  Req.Header.Path = Use;
  // The producer deletes the files it generates, and a copy otherwise needs
  // them on disk to load (DECISIONS.md section 4). Embedding every input
  // sidesteps that here; this test is about loading from memory.
  Req.Args = {"-std=c17", "-Xclang", "-fmodules-embed-all-files"};
  Req.Context = {IncludeRef{"", Types}};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;

  // A name no file has, as for bytes in the daemon's shared memory.
  std::string Name = "/__hc_memory/" + P.ModuleName + ".pcm";
  ASSERT_FALSE(llvm::sys::fs::exists(Name));

  // The compile parses types.h itself, then loads use.h's copy where the
  // #include "use.h" would be, then uses use.h's declarations and macro with
  // its own struct point.
  std::string Body =
      "void f(void) { struct point p; draw(&p); u32 n = count(); (void)n;\n"
      "               int ok = USE_OK; (void)ok; }\n";
  std::string Main = write("main.c", "#include \"" + Types + "\"\n"
                                     "#pragma hc_load\n"
                                     "#pragma clang module import " +
                                         P.ModuleName + ".target\n" + Body);
  LoadResult R = compile(Main, Name, P.Push.Data);
  EXPECT_TRUE(R.Loaded) << R.Errors;
  EXPECT_TRUE(R.Compiled) << R.Errors;

  // Control: without the load, the import has nothing to find.
  std::string NoLoad = write("noload.c", "#include \"" + Types + "\"\n"
                                         "#pragma clang module import " +
                                             P.ModuleName + ".target\n" +
                                             Body);
  LoadResult N = compile(NoLoad, Name, P.Push.Data);
  EXPECT_FALSE(N.Compiled);
}

} // namespace
