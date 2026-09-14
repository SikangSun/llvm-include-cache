//===- unittests/ASTCache/HitTest.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The hit path, end to end and in one process: a header is produced, pushed
// to a daemon, and then a compile that includes that header never opens it.
//
// "Never opens it" is the assertion that matters. The header's declarations
// would be available either way -- the point of a hit is that they arrive
// without the file being read -- so the test watches file entry rather than
// the declarations.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/HeaderCacheServer.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Options/OptionUtils.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <chrono>
#include <set>
#include <thread>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Records every real file the compile enters.
class EnteredFiles : public PPCallbacks {
public:
  EnteredFiles(SourceManager &SM, std::set<std::string> &Out)
      : SM(SM), Out(Out) {}

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID) override {
    if (Reason != EnterFile)
      return;
    if (OptionalFileEntryRef FE = SM.getFileEntryRefForID(SM.getFileID(Loc)))
      Out.insert(FE->getName().str());
  }

private:
  SourceManager &SM;
  std::set<std::string> &Out;
};

/// Installs the hit path and the file recorder on the compile.
class CachedAction : public SyntaxOnlyAction {
public:
  CachedAction(std::unique_ptr<CacheClient> Client, std::set<std::string> &Out)
      : Client(std::move(Client)), Out(Out) {}

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<EnteredFiles>(CI.getSourceManager(), Out));
    installHeaderCacheHook(CI, std::move(Client));
    return true;
  }

private:
  std::unique_ptr<CacheClient> Client;
  std::set<std::string> &Out;
};

class HitTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-hit", D));
    Dir = std::string(D);
    SocketPath = Dir + "/sock";
    RegionPath = Dir + "/region";

    auto Made = CacheDaemon::create(RegionPath, 16u << 20);
    ASSERT_TRUE(bool(Made)) << llvm::toString(Made.takeError());
    Daemon = std::move(*Made);
    Server = std::thread([this] {
      if (llvm::Error E = Daemon->run(SocketPath))
        llvm::consumeError(std::move(E));
    });
    for (int I = 0; I < 500 && !llvm::sys::fs::exists(SocketPath); ++I)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(llvm::sys::fs::exists(SocketPath)) << "daemon never bound";
  }

  void TearDown() override {
    Daemon->stop();
    if (Server.joinable())
      Server.join();
    llvm::sys::fs::remove_directories(Dir);
  }

  std::unique_ptr<CacheClient> client() {
    auto C = CacheClient::connect(SocketPath, RegionPath);
    EXPECT_TRUE(bool(C)) << llvm::toString(C.takeError());
    return C ? std::move(*C) : nullptr;
  }

  std::string write(llvm::StringRef Name, llvm::StringRef Text) {
    llvm::SmallString<256> P(Dir);
    llvm::sys::path::append(P, Name);
    std::error_code EC;
    llvm::raw_fd_ostream OS(P, EC);
    EXPECT_FALSE(EC);
    OS << Text;
    return std::string(P);
  }

  /// Compiles \p MainPath with the hit path installed. Returns whether it
  /// compiled; \p Entered receives every file the compile opened.
  bool compile(const std::string &MainPath, std::set<std::string> &Entered,
               const std::string &WorkingDir = "") {
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS =
        llvm::vfs::createPhysicalFileSystem();
    DiagnosticOptions DiagOpts;
    llvm::IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
        CompilerInstance::createDiagnostics(*VFS, DiagOpts);
    CreateInvocationOptions CIOpts;
    CIOpts.VFS = VFS;
    CIOpts.Diags = Diags;

    static int Anchor = 0;
    std::string ResourceDir =
        GetResourcesPath("clang", reinterpret_cast<void *>(&Anchor));
    std::vector<std::string> ArgStrings = {"clang",
                                           "-resource-dir",
                                           ResourceDir,
                                           "-fsyntax-only",
                                           "-fmodules",
                                           "-fno-implicit-module-maps",
                                           "-std=c17",
                                           "-x",
                                           "c",
                                           MainPath};
    if (!WorkingDir.empty())
      ArgStrings.insert(ArgStrings.begin() + 1, {"-working-directory", WorkingDir});
    std::vector<const char *> Args;
    for (const std::string &A : ArgStrings)
      Args.push_back(A.c_str());

    std::shared_ptr<CompilerInvocation> Invocation =
        createInvocation(Args, CIOpts);
    if (!Invocation)
      return false;
    Invocation->getFrontendOpts().DisableFree = false;

    CompilerInstance CI(std::move(Invocation));
    CI.setDiagnostics(Diags);
    CI.setVerboseOutputStream(llvm::nulls());
    CI.createVirtualFileSystem(VFS);
    CI.createFileManager();

    CachedAction Action(client(), Entered);
    return CI.ExecuteAction(Action) && !Diags->hasErrorOccurred();
  }

  std::string Dir, SocketPath, RegionPath;
  std::unique_ptr<CacheDaemon> Daemon;
  std::thread Server;
};

TEST_F(HitTest, AStoredHeaderIsServedWithoutOpeningTheFile) {
  std::string Types =
      write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                       "struct point { int x, y; };\n"
                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "void draw(struct point *p);\n"
                                   "u32 count(void);\n"
                                   "#define USE_OK 1\n#endif\n");
  std::string Main = write("main.c", "#include \"" + Types + "\"\n"
                                     "#include \"" + Use + "\"\n"
                                     "void f(void) { struct point p; draw(&p);\n"
                                     "               u32 n = count(); (void)n;\n"
                                     "               int ok = USE_OK; (void)ok; }\n");

  // Nothing is stored yet, so the compile reads use.h itself.
  std::set<std::string> Cold;
  ASSERT_TRUE(compile(Main, Cold));
  EXPECT_TRUE(Cold.count(Use)) << "with an empty cache the file must be read";

  // Store use.h, built in the context main.c had when it reached the
  // #include: types.h, and nothing else.
  ProduceRequest Req;
  Req.Header.Path = Use;
  Req.Args = {"-std=c17"};
  Req.Context = {IncludeRef{"", Types}};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;

  PushRequest Push = P.Push;
  auto Writer = client();
  ASSERT_TRUE(Writer);
  ASSERT_TRUE(Writer->push(Push));

  // The same compile again: use.h is never opened, and its declarations and
  // macro are still there.
  std::set<std::string> Warm;
  ASSERT_TRUE(compile(Main, Warm));
  EXPECT_FALSE(Warm.count(Use)) << "use.h was parsed instead of being served";
  EXPECT_TRUE(Warm.count(Types)) << "types.h is the compile's own, not cached";
}

TEST_F(HitTest, OnlyAnIncludeBetweenDeclarationsIsServed) {
  // The parser reads one token ahead, so an #include right after a
  // declaration is processed while the ';' or '}' that ends the declaration
  // is being consumed. That point is between declarations and must be
  // served. A point inside a function body must not be.
  std::string Types =
      write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                       "struct point { int x, y; };\n"
                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "void draw(struct point *p);\n"
                                   "u32 count(void);\n#endif\n");
  ProduceRequest Req;
  Req.Header.Path = Use;
  Req.Args = {"-std=c17"};
  Req.Context = {IncludeRef{"", Types}};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;
  auto Writer = client();
  ASSERT_TRUE(Writer);
  ASSERT_TRUE(Writer->push(P.Push));

  std::string After = write("after.c", "#include \"" + Types + "\"\n"
                                       "int before;\n"
                                       "void g(void) {}\n"
                                       "#include \"" + Use + "\"\n"
                                       "int f(void) { return (int)count(); }\n");
  std::set<std::string> A;
  ASSERT_TRUE(compile(After, A));
  EXPECT_FALSE(A.count(Use)) << "right after a function definition is a "
                                "boundary, and should have been served";

  std::string Inside = write("inside.c", "#include \"" + Types + "\"\n"
                                         "void g(void) {\n"
                                         "#include \"" + Use + "\"\n"
                                         "}\n");
  std::set<std::string> B;
  ASSERT_TRUE(compile(Inside, B));
  EXPECT_TRUE(B.count(Use)) << "inside a function body is not a boundary";
}

TEST_F(HitTest, AHeaderTheCompileNamesRelativelyIsStillServed) {
  // The producer stores absolute paths. redis compiles anet.c from its own
  // directory, so "anet.h" next to it is named relatively by the compile.
  // The first run on redis missed every lookup for exactly this reason.
  std::string Types =
      write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "u32 count(void);\n#endif\n");
  ProduceRequest Req;
  Req.Header.Path = Use;
  Req.Args = {"-std=c17"};
  Req.Context = {IncludeRef{"", Types}};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;
  auto Writer = client();
  ASSERT_TRUE(Writer);
  ASSERT_TRUE(Writer->push(P.Push));

  write("rel.c", "#include \"types.h\"\n#include \"use.h\"\n"
                 "int f(void) { return (int)count(); }\n");
  std::set<std::string> Entered;
  ASSERT_TRUE(compile("rel.c", Entered, /*WorkingDir=*/Dir));
  for (const std::string &E : Entered)
    EXPECT_FALSE(llvm::StringRef(E).ends_with("use.h"))
        << "use.h was opened as " << E;
  bool ReadTypes = false;
  for (const std::string &E : Entered)
    ReadTypes |= llvm::StringRef(E).ends_with("types.h");
  EXPECT_TRUE(ReadTypes) << "types.h is the compile's own and must be read";
}

TEST_F(HitTest, AHeaderIsNotServedWhenTheRecordDoesNotMatch) {
  // The stored entry was built with FEATURE defined. A compile without it
  // must read the file rather than take the entry.
  std::string Feat = write("feat.h", "#define FEATURE 1\n");
  std::string H = write("h.h", "#ifndef H_H\n#define H_H\n#ifdef FEATURE\n"
                               "int with_feature(void);\n#endif\n"
                               "int always(void);\n#endif\n");
  ProduceRequest Req;
  Req.Header.Path = H;
  Req.Args = {"-std=c17"};
  Req.Context = {IncludeRef{"", Feat}};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;
  auto Writer = client();
  ASSERT_TRUE(Writer);
  ASSERT_TRUE(Writer->push(P.Push));

  std::string Main = write("nofeature.c", "#include \"" + H + "\"\n"
                                          "int g(void) { return always(); }\n");
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(Main, Entered));
  EXPECT_TRUE(Entered.count(H))
      << "FEATURE is undefined here, so the stored entry must not be used";
}

} // namespace
