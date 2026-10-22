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

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/HeaderCacheServer.h"
#include "clang/ASTCache/SharedRegion.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Options/OptionUtils.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"
#include <chrono>
#include <functional>
#include <optional>
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

/// Records the variables a compile generating code would be told to emit
/// that a syntax-only one never sees emitted: a module's initializers, at
/// each import, and the tentative definitions completed at the end.
class EmitRecorder : public ASTConsumer {
public:
  EmitRecorder(std::set<std::string> &Out,
               std::function<void(ASTContext &)> AtEnd)
      : Out(Out), AtEnd(std::move(AtEnd)) {}

  void HandleTranslationUnit(ASTContext &Ctx) override {
    if (AtEnd)
      AtEnd(Ctx);
  }

  void HandleImplicitImportDecl(ImportDecl *D) override {
    for (Decl *I :
         D->getASTContext().getModuleInitializers(D->getImportedModule()))
      if (auto *ND = dyn_cast<NamedDecl>(I))
        Out.insert(ND->getName().str());
  }

  void CompleteTentativeDefinition(VarDecl *D) override {
    Out.insert(D->getName().str());
  }

private:
  std::set<std::string> &Out;
  std::function<void(ASTContext &)> AtEnd;
};

/// Installs the hit path and the file recorder on the compile.
class CachedAction : public SyntaxOnlyAction {
public:
  CachedAction(std::unique_ptr<CacheClient> Client, std::set<std::string> &Out,
               std::string JobsDir = "", bool ExportParts = false,
               std::set<std::string> *Emitted = nullptr,
               std::function<void(ASTContext &)> AtEnd = nullptr)
      : Client(std::move(Client)), Out(Out), JobsDir(std::move(JobsDir)),
        ExportParts(ExportParts), Emitted(Emitted), AtEnd(std::move(AtEnd)) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef File) override {
    if (Emitted)
      return std::make_unique<EmitRecorder>(*Emitted, AtEnd);
    return SyntaxOnlyAction::CreateASTConsumer(CI, File);
  }

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<EnteredFiles>(CI.getSourceManager(), Out));
    installHeaderCacheHook(CI, std::move(Client), /*ReportStats=*/false,
                           JobsDir,
                           ExportParts ? ExportMode::Misses : ExportMode::None);
    return true;
  }

private:
  std::unique_ptr<CacheClient> Client;
  std::set<std::string> &Out;
  std::string JobsDir;
  bool ExportParts;
  std::set<std::string> *Emitted;
  std::function<void(ASTContext &)> AtEnd;
};

class HitTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-hit", D));
    Dir = std::string(D);
    SocketPath = Dir + "/sock";
    RegionPath = Dir + "/region";

    auto Made = CacheDaemon::create(RegionPath, 16u << 20, daemonOptions());
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

  /// These tests are about serving, not about which headers are asked about,
  /// so every stored header is listed.
  virtual CacheDaemon::Options daemonOptions() {
    CacheDaemon::Options O;
    O.ListStored = true;
    return O;
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
    ArgStrings.insert(ArgStrings.end() - 1, ExtraArgs.begin(), ExtraArgs.end());
    std::vector<const char *> Args;
    for (const std::string &A : ArgStrings)
      Args.push_back(A.c_str());

    std::shared_ptr<CompilerInvocation> Invocation =
        createInvocation(Args, CIOpts);
    if (!Invocation)
      return false;
    Invocation->getFrontendOpts().DisableFree = false;
    // Diags was made before the command line was read; its warning flags
    // (-Wall, -Werror) are the compile's.
    ProcessWarningOptions(*Diags, Invocation->getDiagnosticOpts(), *VFS,
                          /*ReportDiags=*/false);

    CompilerInstance CI(std::move(Invocation));
    CI.setDiagnostics(Diags);
    CI.setVerboseOutputStream(llvm::nulls());
    CI.createVirtualFileSystem(VFS);
    CI.createFileManager();

    Emitted.clear();
    std::unique_ptr<CacheClient> C = client();
    if (C)
      C->setBuild(Build);
    CachedAction Action(std::move(C), Entered, RecordJobsTo, ExportParts,
                        &Emitted, AtEnd);
    bool Ok = CI.ExecuteAction(Action) && !Diags->hasErrorOccurred();
    Warnings = Diags->getNumWarnings();
    return Ok;
  }

  /// Builds \p Header after the files in \p Context, as a compile that
  /// included them first would, and stores the result.
  void store(const std::string &Header,
             const std::vector<std::string> &Context = {}) {
    ProduceRequest Req;
    Req.Header.Path = Header;
    Req.Args = {"-std=c17"};
    for (const std::string &C : Context)
      Req.Context.push_back(IncludeRef{"", C});
    ProduceResult P = produceHeader(Req);
    ASSERT_TRUE(P.produced()) << P.Reason;
    auto Writer = client();
    ASSERT_TRUE(Writer);
    ASSERT_TRUE(Writer->push(P.Push));
  }

  /// Whether compiling \p Main read \p Header rather than taking it from the
  /// cache.
  bool reads(const std::string &Main, const std::string &Header) {
    std::set<std::string> Entered;
    EXPECT_TRUE(compile(Main, Entered)) << Main << " did not compile";
    return Entered.count(Header) != 0;
  }

  std::string Dir, SocketPath, RegionPath;
  /// Flags added to every compile, before the file.
  std::vector<std::string> ExtraArgs;
  /// When set, compiles export each listed header they read (PartExporter).
  bool ExportParts = false;
  /// When set, compiles record their jobs in this directory.
  std::string RecordJobsTo;
  /// The variables the last compile would have been told to emit, beyond
  /// what it parsed: module initializers and tentative definitions.
  std::set<std::string> Emitted;
  /// Run on each compile's AST when its translation unit ends.
  std::function<void(ASTContext &)> AtEnd;
  /// How many warnings the last compile printed.
  unsigned Warnings = 0;
  /// The build compiles belong to (CacheClient::setBuild); 0 for none.
  uint64_t Build = 0;
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

TEST_F(HitTest, AHeaderUsingFileIsServedOnlyUnderTheNameItWasBuiltBy) {
  // __FILE__ is the name the compile opened the file by. The producer opens
  // where.h by its absolute path, so the copy holds that path; a compile run
  // from the directory names it "./where.h" and must read the file.
  std::string Where =
      write("where.h", "#ifndef WHERE_H\n#define WHERE_H\n"
                       "static const char *where_file = __FILE__;\n#endif\n");
  store(Where);

  std::string Abs =
      write("abs.c", "#include \"" + Where +
                         "\"\n"
                         "const char *f(void) { return where_file; }\n");
  EXPECT_FALSE(reads(Abs, Where)) << "the same name should have been served";

  write("rel.c", "#include \"where.h\"\n"
                 "const char *f(void) { return where_file; }\n");
  std::set<std::string> Entered;
  ASSERT_TRUE(compile("rel.c", Entered, /*WorkingDir=*/Dir));
  bool Read = false;
  for (const std::string &E : Entered)
    Read |= llvm::StringRef(E).ends_with("where.h");
  EXPECT_TRUE(Read) << "a copy holding another name for where.h was served";
}

TEST_F(HitTest, AFileBesideTheHeaderIsCheckedUnderTheNameTheHeaderGivesIt) {
  // outer.h finds inner.h beside itself, and inner.h uses __FILE__. Its
  // name follows from outer.h's: "<outer.h's directory>/inner.h".
  llvm::SmallString<256> Sub(Dir);
  llvm::sys::path::append(Sub, "sub");
  ASSERT_FALSE(llvm::sys::fs::create_directory(Sub));
  std::string Outer = write("sub/outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                                           "#include \"inner.h\"\n#endif\n");
  std::string Inner =
      write("sub/inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                           "static const char *inner_file = __FILE__;\n"
                           "#endif\n");
  store(Outer);

  std::string Abs =
      write("abs.c", "#include \"" + Outer +
                         "\"\n"
                         "const char *f(void) { return inner_file; }\n");
  EXPECT_FALSE(reads(Abs, Outer)) << "the same name should have been served";

  write("rel.c", "#include \"sub/outer.h\"\n"
                 "const char *f(void) { return inner_file; }\n");
  std::set<std::string> Entered;
  ASSERT_TRUE(compile("rel.c", Entered, /*WorkingDir=*/Dir));
  bool Read = false;
  for (const std::string &E : Entered)
    Read |= llvm::StringRef(E).ends_with("inner.h");
  EXPECT_TRUE(Read) << "a copy holding another name for inner.h was served";
}

TEST_F(HitTest, AHeaderIsServedOnlyWhereItsIncludesFindTheSameFiles) {
  // h.h includes <cfg.h>, and the include path decides which cfg.h: qemu's
  // kvm.h and "cpu.h" under -I../target/arm or -I../target/i386. Nothing in
  // cfg.h is outside h.h's region, so no other dependency tells them apart.
  for (const char *D : {"a", "b"}) {
    llvm::SmallString<256> Sub(Dir);
    llvm::sys::path::append(Sub, D);
    ASSERT_FALSE(llvm::sys::fs::create_directory(Sub));
  }
  write("a/cfg.h", "typedef int cfg_t;\n");
  write("b/cfg.h", "typedef long cfg_t;\n");
  std::string H = write("h.h", "#ifndef H_H\n#define H_H\n#include <cfg.h>\n"
                               "cfg_t get(void);\n#endif\n");
  ProduceRequest Req;
  Req.Header.Path = H;
  Req.Args = {"-std=c17", "-I" + Dir + "/a"};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;
  auto Writer = client();
  ASSERT_TRUE(Writer);
  ASSERT_TRUE(Writer->push(P.Push));

  std::string Main = write(
      "main.c", "#include \"" + H + "\"\nint f(void) { return (int)get(); }\n");
  ExtraArgs = {"-I" + Dir + "/a"};
  EXPECT_FALSE(reads(Main, H))
      << "the same include path should have been served";
  ExtraArgs = {"-I" + Dir + "/b"};
  EXPECT_TRUE(reads(Main, H)) << "a copy holding a/cfg.h was served where "
                                 "<cfg.h> finds b/cfg.h";
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

// The next four are the silent miscompiles the syntactic record exists for.
// In each, the stored parse was built where a name was not a macro, and the
// includer has since made it one. No macro is tested inside the header, and
// in the first three none is expanded there either, so before the record
// existed every one of them was served, and the program called the wrong
// function. Each first checks that the header is served without the #define,
// so that "read" below cannot be down to some other mismatch.

TEST_F(HitTest, NotServedWhenTheIncluderMakesANameAMacro) {
  std::string Lib = write("lib.h", "#ifndef LIB_H\n#define LIB_H\n"
                                   "void *malloc(unsigned long n);\n"
                                   "static inline void *grab(unsigned long n) "
                                   "{ return malloc(n); }\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Lib));

  std::string Plain =
      write("plain.c", "#include \"" + Lib +
                           "\"\n"
                           "void *f(void) { return grab(8); }\n");
  EXPECT_FALSE(reads(Plain, Lib)) << "control: nothing redefines malloc";

  std::string Renamed =
      write("renamed.c", "void *my_malloc(unsigned long n);\n"
                         "#define malloc my_malloc\n"
                         "#include \"" +
                             Lib +
                             "\"\n"
                             "void *f(void) { return grab(8); }\n");
  EXPECT_TRUE(reads(Renamed, Lib))
      << "grab must call my_malloc, which only reading lib.h gives";
}

TEST_F(HitTest, NotServedWhenTheIncluderMakesAKeywordAMacro) {
  std::string Kw = write("kw.h", "#ifndef KW_H\n#define KW_H\n"
                                 "static inline unsigned width(void) "
                                 "{ return sizeof(long); }\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Kw));

  std::string Plain =
      write("plain.c", "#include \"" + Kw +
                           "\"\n"
                           "unsigned f(void) { return width(); }\n");
  EXPECT_FALSE(reads(Plain, Kw)) << "control: long is still long";

  std::string Redefined = write("redefined.c", "#define long int\n"
                                               "#include \"" +
                                                   Kw +
                                                   "\"\n"
                                                   "unsigned f(void) "
                                                   "{ return width(); }\n");
  EXPECT_TRUE(reads(Redefined, Kw)) << "width must return sizeof(int) here";
}

TEST_F(HitTest, NotServedWhenTheIncluderMakesAPastedNameAMacro) {
  // paste.h never spells malloc; JOIN builds it.
  std::string Decl =
      write("decl.h", "#ifndef DECL_H\n#define DECL_H\n"
                      "void *malloc(unsigned long n);\n#endif\n");
  std::string Paste =
      write("paste.h", "#ifndef PASTE_H\n#define PASTE_H\n"
                       "#define JOIN(a, b) a##b\n"
                       "static inline void *grab2(unsigned long n) "
                       "{ return JOIN(mal, loc)(n); }\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Paste, {Decl}));

  std::string Plain =
      write("plain.c", "#include \"" + Decl +
                           "\"\n"
                           "#include \"" +
                           Paste +
                           "\"\n"
                           "void *f(void) { return grab2(8); }\n");
  EXPECT_FALSE(reads(Plain, Paste)) << "control: nothing redefines malloc";

  std::string Renamed =
      write("renamed.c", "#include \"" + Decl +
                             "\"\n"
                             "void *my_malloc(unsigned long n);\n"
                             "#define malloc my_malloc\n"
                             "#include \"" +
                             Paste +
                             "\"\n"
                             "void *f(void) { return grab2(8); }\n");
  EXPECT_TRUE(reads(Renamed, Paste))
      << "the pasted malloc is rescanned and becomes my_malloc";
}

TEST_F(HitTest, NotServedWhenANameFromAnOutsideMacroBecomesAMacro) {
  // ALLOC is defined outside get.h and still expands to exactly grab_impl, so
  // the check on ALLOC passes. What changed is grab_impl itself, which get.h
  // never spells but which is rescanned inside it.
  std::string Outside =
      write("outside.h", "#ifndef OUTSIDE_H\n#define OUTSIDE_H\n"
                         "void *grab_impl(unsigned long n);\n"
                         "#define ALLOC grab_impl\n#endif\n");
  std::string Get = write("get.h", "#ifndef GET_H\n#define GET_H\n"
                                   "static inline void *get(unsigned long n) "
                                   "{ return ALLOC(n); }\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Get, {Outside}));

  std::string Plain =
      write("plain.c", "#include \"" + Outside +
                           "\"\n"
                           "#include \"" +
                           Get +
                           "\"\n"
                           "void *f(void) { return get(8); }\n");
  EXPECT_FALSE(reads(Plain, Get)) << "control: grab_impl is not a macro";

  std::string Renamed =
      write("renamed.c", "#include \"" + Outside +
                             "\"\n"
                             "void *other(unsigned long n);\n"
                             "#define grab_impl other\n"
                             "#include \"" +
                             Get +
                             "\"\n"
                             "void *f(void) { return get(8); }\n");
  EXPECT_TRUE(reads(Renamed, Get)) << "get must call other here";
}

TEST_F(HitTest, ServedWhenTheHeaderDefinesTheNameBeforeUsingIt) {
  // The includer's BUF can never reach buf.h's use of it: buf.h's own #define
  // replaces it first. Rejecting here would cost a hit for nothing.
  std::string Buf = write("buf.h", "#ifndef BUF_H\n#define BUF_H\n"
                                   "#define BUF 16\nextern char buf[BUF];\n"
                                   "#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Buf));

  std::string Main =
      write("main.c", "#define BUF 8\n"
                      "#include \"" +
                          Buf +
                          "\"\n"
                          "int f(void) { return (int)sizeof buf; }\n");
  EXPECT_FALSE(reads(Main, Buf));
}

// A hit loads a module, and a module that defines a name the compile has
// already seen leaves that name's identifier out of date: its macro bit still
// says "not a macro" until something updates it. Lexing does that on the way
// in. The checks on the next #include ask about names without lexing them, so
// they have to do it themselves, or a later entry is matched against the
// state from before the hit.

TEST_F(HitTest, AConditionalSeesAMacroFromAnEarlierHit) {
  std::string A = write("a.h", "#ifndef A_H\n#define A_H\n#define FEATURE 1\n"
                               "int a_decl;\n#endif\n");
  std::string B = write("b.h", "#ifndef B_H\n#define B_H\n#ifdef FEATURE\n"
                               "int with_feature(void);\n#endif\n"
                               "int always(void);\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(A));
  ASSERT_NO_FATAL_FAILURE(store(B));

  std::string Plain =
      write("plain.c", "#include \"" + B +
                           "\"\n"
                           "int g(void) { return always(); }\n");
  EXPECT_FALSE(reads(Plain, B)) << "control: FEATURE is defined nowhere";

  // main.c mentions FEATURE first, so its identifier exists before a.h loads.
  std::string Main =
      write("main.c", "#ifdef FEATURE\n#endif\n"
                      "#include \"" +
                          A +
                          "\"\n"
                          "#include \"" +
                          B +
                          "\"\n"
                          "int g(void) { return with_feature(); }\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered))
      << "with_feature is declared only if b.h is read with FEATURE defined";
  EXPECT_FALSE(Entered.count(A)) << "a.h must come from the cache, or this "
                                    "test is not testing a hit";
  EXPECT_TRUE(Entered.count(B));
}

TEST_F(HitTest, ASyntacticCheckSeesAMacroFromAnEarlierHit) {
  std::string A = write("wrap.h", "#ifndef WRAP_H\n#define WRAP_H\n"
                                  "void *my_malloc(unsigned long n);\n"
                                  "#define malloc my_malloc\n#endif\n");
  std::string Lib = write("lib.h", "#ifndef LIB_H\n#define LIB_H\n"
                                   "void *malloc(unsigned long n);\n"
                                   "static inline void *grab(unsigned long n) "
                                   "{ return malloc(n); }\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(A));
  ASSERT_NO_FATAL_FAILURE(store(Lib));

  // main.c declares malloc first, so its identifier exists before wrap.h
  // loads and makes it a macro.
  std::string Main = write("main.c", "void *malloc(unsigned long n);\n"
                                     "#include \"" +
                                         A +
                                         "\"\n"
                                         "#include \"" +
                                         Lib +
                                         "\"\n"
                                         "void *f(void) { return grab(8); }\n");
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(A)) << "wrap.h must come from the cache";
  EXPECT_TRUE(Entered.count(Lib)) << "grab must call my_malloc";
}

// A served header must leave the compile as entering it would have: its
// macros defined or undefined, and a header it read skipped when included
// again.

TEST_F(HitTest, ARedefinitionInTheHeaderTakesEffect) {
  std::string Config = write("config.h", "#define LIMIT 10\n");
  std::string Lim = write("lim.h", "#undef LIMIT\n#define LIMIT 20\n");
  ASSERT_NO_FATAL_FAILURE(store(Lim, {Config}));
  std::string Main =
      write("main.c", "#include \"" + Config +
                          "\"\n"
                          "#include \"" +
                          Lim +
                          "\"\n"
                          "_Static_assert(LIMIT == 20, \"\");\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered)) << "LIMIT must be 20 after lim.h";
  EXPECT_FALSE(Entered.count(Lim));
}

TEST_F(HitTest, AnUndefInTheHeaderTakesEffect) {
  // A module's #undef of a macro its importer defined does not hide the
  // importer's definition; only replaying the #undef does.
  std::string Config = write("config.h", "#define LIMIT 10\n");
  std::string Lim = write("lim.h", "#undef LIMIT\nint lim_loaded;\n");
  ASSERT_NO_FATAL_FAILURE(store(Lim, {Config}));
  std::string Main = write(
      "main.c", "#include \"" + Config +
                    "\"\n"
                    "#include \"" +
                    Lim +
                    "\"\n"
                    "#ifdef LIMIT\n#error LIMIT survived lim.h\n#endif\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered)) << "LIMIT must be gone after lim.h";
  EXPECT_FALSE(Entered.count(Lim));
}

TEST_F(HitTest, OfTwoServedHeadersDefiningAMacroTheLaterWins) {
  // Two loaded modules that define one name differently leave it ambiguous;
  // included as text, the later definition wins.
  std::string A = write("a.h", "#define MODE 1\n");
  std::string B = write("b.h", "#undef MODE\n#define MODE 2\n");
  ASSERT_NO_FATAL_FAILURE(store(A));
  ASSERT_NO_FATAL_FAILURE(store(B, {A}));
  std::string Main = write("main.c", "#include \"" + A +
                                         "\"\n"
                                         "#include \"" +
                                         B +
                                         "\"\n"
                                         "_Static_assert(MODE == 2, \"\");\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(A));
  EXPECT_FALSE(Entered.count(B));
}

TEST_F(HitTest, AFlagFromOneServedHeaderIsSeenByTheCompile) {
  std::string Fm = write("fm.h", "#ifndef FM_H\n#define FM_H\n"
                                 "#define FEATURE_X 1\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Fm +
                          "\"\n"
                          "#if FEATURE_X != 1\n#error\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Fm));
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Fm));
}

TEST_F(HitTest, AServedPragmaOnceHeaderIsSkippedWhenIncludedAgain) {
  std::string Once = write("once.h", "#pragma once\nstruct s { int a; };\n");
  ASSERT_NO_FATAL_FAILURE(store(Once));
  std::string Main = write("main.c", "#include \"" + Once +
                                         "\"\n"
                                         "#include \"" +
                                         Once +
                                         "\"\n"
                                         "struct s v;\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered)) << "a second struct s would not compile";
  EXPECT_FALSE(Entered.count(Once));
}

TEST_F(HitTest, APragmaOnceHeaderInsideAServedHeaderIsSkippedLater) {
  // once.h was never entered by this compile -- it came inside use.h's saved
  // copy -- and must still count as included.
  std::string Once = write("once.h", "#pragma once\nstruct s { int a; };\n");
  std::string Use = write("use.h", "#pragma once\n#include \"" + Once +
                                       "\"\nvoid take(struct s *);\n");
  ASSERT_NO_FATAL_FAILURE(store(Use));
  std::string Main = write("main.c", "#include \"" + Use +
                                         "\"\n"
                                         "#include \"" +
                                         Once +
                                         "\"\n"
                                         "struct s v;\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Use));
  EXPECT_FALSE(Entered.count(Once));
}

TEST_F(HitTest, AGuardedHeaderInsideAServedHeaderIsSkippedLater) {
  // Its guard macro is one of the served header's effects, so the second
  // #include is skipped without opening the file.
  std::string Sub = write("sub.h", "#ifndef SUB_H\n#define SUB_H\n"
                                   "struct t { int a; };\n#endif\n");
  std::string Use =
      write("use.h", "#ifndef USE_H\n#define USE_H\n#include \"" + Sub +
                         "\"\nvoid take(struct t *);\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(Use));
  std::string Main = write("main.c", "#include \"" + Use +
                                         "\"\n"
                                         "#include \"" +
                                         Sub +
                                         "\"\n"
                                         "struct t v;\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Use));
  EXPECT_FALSE(Entered.count(Sub)) << "sub.h's guard is already defined";
}

TEST_F(HitTest, AServedGuardedHeaderIsSkippedWhenIncludedAgain) {
  std::string G = write("g.h", "#ifndef G_H\n#define G_H\n"
                               "struct g { int a; };\n#endif\n");
  ASSERT_NO_FATAL_FAILURE(store(G));
  std::string Main = write("main.c", "#include \"" + G +
                                         "\"\n"
                                         "#include \"" +
                                         G +
                                         "\"\n"
                                         "struct g v;\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(G));
}

TEST_F(HitTest, ACopyBuiltWithOtherOptionsIsPassedOverNotAnError) {
  // The daemon stores entries by header path alone, so copies built with
  // other compiler options are candidates too, and their dependencies can
  // all match. The module reader refuses them. That must cost a miss -- or
  // the next candidate -- and never fail a compile that could parse the
  // header itself: -Wmodule-file-config-mismatch is an error by default.
  std::string H = write("h.h", "enum small { A, B };\nint h_loaded;\n");
  auto StoreWith = [&](std::vector<std::string> Args) {
    ProduceRequest Req;
    Req.Header.Path = H;
    Req.Args = std::move(Args);
    ProduceResult P = produceHeader(Req);
    ASSERT_TRUE(P.produced()) << P.Reason;
    auto Writer = client();
    ASSERT_TRUE(Writer);
    ASSERT_TRUE(Writer->push(P.Push));
  };
  ASSERT_NO_FATAL_FAILURE(StoreWith({"-std=c17", "-fshort-enums"}));
  ASSERT_NO_FATAL_FAILURE(StoreWith({"-std=c11"}));
  std::string Main = write("main.c", "#include \"" + H + "\"\n");
  EXPECT_TRUE(reads(Main, H)) << "no stored copy fits this compile";

  // Stored after the two that do not fit, and still found.
  ASSERT_NO_FATAL_FAILURE(StoreWith({"-std=c17"}));
  EXPECT_FALSE(reads(Main, H));
}

/// The ask-list as a build uses it: a header is asked about only once some
/// compile has reported reading it slowly. The test headers parse in a few
/// microseconds, so a measured hit would drop them from the list; these
/// tests are about asking and serving, so dropping is off.
class AskListTest : public HitTest {
protected:
  CacheDaemon::Options daemonOptions() override {
    CacheDaemon::Options O;
    O.AskCutoffMicros = Cutoff;
    O.DropCostly = false;
    return O;
  }
  uint64_t Cutoff = 0;
};

TEST_F(AskListTest, AStoredHeaderIsServedOnlyOnceACompileHasTimedIt) {
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "int used(void);\n#endif\n");
  std::string Main = write("main.c", "#include \"" + Use + "\"\n");
  ASSERT_NO_FATAL_FAILURE(store(Use));

  // Stored, but no compile has reported a time for it: not asked about.
  EXPECT_TRUE(reads(Main, Use)) << "unlisted headers must not be asked about";
  EXPECT_EQ(Daemon->stats().Lookups, 0u);
  EXPECT_EQ(Daemon->stats().HeadersListed, 1u)
      << "that compile read use.h and reported it; any time meets a cutoff "
         "of zero";

  EXPECT_FALSE(reads(Main, Use));
  EXPECT_EQ(Daemon->stats().Lookups, 1u);
}

TEST_F(AskListTest, AHeaderReadFasterThanTheCutoffIsNeverAskedAbout) {
  Daemon->stop();
  if (Server.joinable())
    Server.join();
  llvm::sys::fs::remove(SocketPath);
  Cutoff = 60ull * 1000 * 1000; // a minute: no header takes that long
  SetUp();

  std::string Use = write("use.h", "int used(void);\n");
  std::string Main = write("main.c", "#include \"" + Use + "\"\n");
  ASSERT_NO_FATAL_FAILURE(store(Use));
  EXPECT_TRUE(reads(Main, Use));
  EXPECT_TRUE(reads(Main, Use));
  EXPECT_EQ(Daemon->stats().HeadersTimed, 1u);
  EXPECT_EQ(Daemon->stats().HeadersListed, 0u);
  EXPECT_EQ(Daemon->stats().Lookups, 0u);
}

TEST_F(HitTest, AHitLeavesTheCounterWhereTheHeaderFoundIt) {
  // plain.h is saved after ctx.h, whose two uses of __COUNTER__ the module
  // file records as the count it ended with. Loading it must not set the live
  // counter back to 2 after other.h has taken it to 5.
  std::string Ctx =
      write("ctx.h", "#ifndef CTX_H\n#define CTX_H\n"
                     "enum { C0 = __COUNTER__, C1 = __COUNTER__ };\n"
                     "#endif\n");
  std::string Plain = write("plain.h", "#ifndef PLAIN_H\n#define PLAIN_H\n"
                                       "struct plain { int a; };\n#endif\n");
  std::string Other =
      write("other.h", "enum { O0 = __COUNTER__, "
                       "O1 = __COUNTER__, O2 = __COUNTER__ };\n");
  ASSERT_NO_FATAL_FAILURE(store(Plain, {Ctx}));
  std::string Main = write("main.c", "#include \"" + Ctx + "\"\n#include \"" +
                                         Other + "\"\n#include \"" + Plain +
                                         "\"\n_Static_assert(__COUNTER__ == 5, "
                                         "\"\");\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Plain));
}

TEST_F(HitTest, AHeaderIncludedFromAHeaderIsServed) {
  // inner.h refuses to compile unless outer.h's own text came first, the
  // shape of glibc's bits/ headers. Its copy is built after outer.h's lines,
  // and served at outer.h's #include of it.
  std::string Inner = write("inner.h", "#ifndef OUTER_H\n#error\n#endif\n"
                                       "enum { LIM = LEN };\n");
  std::string Outer = write("outer.h", "#define OUTER_H 1\n#define LEN 4096\n"
                                       "#include \"" +
                                           Inner + "\"\n");
  ProduceRequest Req;
  Req.Header.Path = Inner;
  Req.Args = {"-std=c17"};
  Req.Context = {ContextEntry::define("OUTER_H 1"),
                 ContextEntry::define("LEN 4096")};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;
  ASSERT_TRUE(client()->push(P.Push));

  std::string Main =
      write("main.c", "#include \"" + Outer +
                          "\"\n"
                          "_Static_assert(LIM == 4096, \"\");\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_TRUE(Entered.count(Outer));
  EXPECT_FALSE(Entered.count(Inner));
}

TEST_F(HitTest, ARecordedJobBuildsACopyTheNextCompileServes) {
  std::string Inner = write("inner.h", "#ifndef OUTER_H\n#error\n#endif\n"
                                       "enum { LIM = LEN };\n");
  std::string Outer = write("outer.h", "#define OUTER_H 1\n#define LEN 4096\n"
                                       "#include \"" +
                                           Inner + "\"\n");
  std::string Main =
      write("main.c", "#define FROM_MAIN 1\n#include \"" + Outer +
                          "\"\n"
                          "_Static_assert(LIM == 4096, \"\");\n");
  RecordJobsTo = Dir + "/jobs";
  ASSERT_FALSE(llvm::sys::fs::create_directory(RecordJobsTo));
  ASSERT_TRUE(reads(Main, Inner)) << "nothing is stored yet";

  // One job file per compile, with a job for each header it read.
  std::vector<ProduceRequest> Jobs;
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator It(RecordJobsTo, EC), End;
       !EC && It != End; It.increment(EC)) {
    llvm::Expected<std::vector<ProduceRequest>> Read = readJobFile(It->path());
    ASSERT_THAT_EXPECTED(Read, llvm::Succeeded());
    Jobs.insert(Jobs.end(), Read->begin(), Read->end());
  }
  ASSERT_EQ(Jobs.size(), 2u);
  const ProduceRequest *ForOuter = nullptr, *ForInner = nullptr;
  for (const ProduceRequest &J : Jobs)
    (J.Header.Path == Outer ? ForOuter : ForInner) = &J;
  ASSERT_TRUE(ForOuter && ForInner);
  EXPECT_EQ(ForInner->Header.Path, Inner);
  // main.c's line before #include "outer.h", then outer.h's lines before
  // #include "inner.h" -- never outer.h itself.
  std::vector<ContextEntry> Want = {ContextEntry::define("FROM_MAIN 1"),
                                    ContextEntry::define("OUTER_H 1"),
                                    ContextEntry::define("LEN 4096")};
  EXPECT_EQ(ForInner->Context, Want);
  EXPECT_EQ(ForOuter->Context,
            std::vector<ContextEntry>{ContextEntry::define("FROM_MAIN 1")});

  // The flags come from the build, as the populate tool adds them.
  ProduceRequest Req = *ForInner;
  Req.Args = {"-std=c17"};
  ProduceResult P = produceHeader(Req);
  ASSERT_TRUE(P.produced()) << P.Reason;
  ASSERT_TRUE(client()->push(P.Push));
  RecordJobsTo.clear();
  EXPECT_FALSE(reads(Main, Inner));
}

TEST_F(HitTest, ARecordedIncludeIsReplayedTheWayItWasFound) {
  // Beside its includer, a quoted #include is replayed by path: from the
  // producer's wrapper, in a directory of its own, "first.h" finds nothing.
  std::string Sub = Dir + "/sub";
  ASSERT_FALSE(llvm::sys::fs::create_directory(Sub));
  std::string First = write("sub/first.h", "typedef int first_t;\n");
  std::string Second = write("sub/second.h", "first_t second(void);\n");
  std::string Outer = write("sub/outer.h", "#include \"first.h\"\n"
                                           "#include \"second.h\"\n");
  std::string Main = write("main.c", "#include \"sub/outer.h\"\n");
  RecordJobsTo = Dir + "/jobs";
  ASSERT_FALSE(llvm::sys::fs::create_directory(RecordJobsTo));
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(Main, Entered));

  std::error_code EC;
  llvm::sys::fs::directory_iterator It(RecordJobsTo, EC);
  ASSERT_FALSE(EC);
  llvm::Expected<std::vector<ProduceRequest>> Jobs = readJobFile(It->path());
  ASSERT_THAT_EXPECTED(Jobs, llvm::Succeeded());
  const ProduceRequest *ForSecond = nullptr;
  for (const ProduceRequest &J : *Jobs)
    if (J.Header.Path == Second)
      ForSecond = &J;
  ASSERT_NE(ForSecond, nullptr);
  EXPECT_EQ(ForSecond->Header.Spelled, "");
  EXPECT_EQ(ForSecond->Context,
            std::vector<ContextEntry>{ContextEntry(IncludeRef{"", First})});

  ProduceRequest Req = *ForSecond;
  Req.Args = {"-std=c17"};
  ProduceResult P = produceHeader(Req);
  EXPECT_TRUE(P.produced()) << P.Reason;
}

TEST_F(HitTest, AClientAttachesOnlyToTheDaemonsOwnRegion) {
  // Without a path, the client attaches to the file the daemon names.
  auto Asked = CacheClient::connect(SocketPath);
  EXPECT_TRUE(bool(Asked)) << llvm::toString(Asked.takeError());

  // Another region file -- a leftover, or the default path while this daemon
  // writes elsewhere -- holds other bytes at the offsets the daemon names.
  std::string Other = Dir + "/other-region";
  auto Stale = SharedRegion::create(Other, 1u << 20);
  ASSERT_TRUE(bool(Stale)) << llvm::toString(Stale.takeError());
  auto Wrong = CacheClient::connect(SocketPath, Other);
  EXPECT_FALSE(bool(Wrong));
  llvm::consumeError(Wrong.takeError());
}

TEST_F(HitTest, AnUnguardedHeaderServedTwiceLoadsItsCopyOnce) {
  // stddef.h's shape: no include guard, so it is entered again each time,
  // and the same copy fits both times. A second module of the same name is a
  // fatal error, so the second hit reuses the first load.
  std::string Twice = write("twice.h", "typedef int twice_t;\n");
  ASSERT_NO_FATAL_FAILURE(store(Twice));
  std::string Main = write("main.c", "#include \"" + Twice + "\"\n#include \"" +
                                         Twice + "\"\ntwice_t x;\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Twice));
}

TEST_F(HitTest, AHeaderThatSetsAFlagForItsOwnIncludeIsServed) {
  // fmacros.h's shape. Before dependencies faced strictly outward, fm.h's
  // copy required FM_SOURCE defined at #include "fm.h", which no compile
  // ever has, so the copy was never used.
  std::string Feat =
      write("feat.h", "#ifdef FM_SOURCE\nint feature_on;\n#endif\n");
  std::string Fm =
      write("fm.h", "#define FM_SOURCE 1\n#include \"" + Feat + "\"\n");
  ASSERT_NO_FATAL_FAILURE(store(Fm));
  std::string Main = write("main.c", "#include \"" + Fm +
                                         "\"\nint use(void) { return "
                                         "feature_on; }\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Fm));
  EXPECT_FALSE(Entered.count(Feat));
}

TEST_F(HitTest, AMacroFromAServedCopyComparesByItsText) {
  // A saved copy stores a macro's tokens without their text, so 64 loaded
  // from wordsize.h's copy is a number with no digits. Compared that way it
  // never equals the 64 that size.h's copy recorded from text, and size.h
  // was never served once wordsize.h was.
  std::string Word = write("wordsize.h", "#define WORDSIZE 64\n");
  std::string Size = write("size.h", "enum { BITS = WORDSIZE };\n");
  ASSERT_NO_FATAL_FAILURE(store(Word));
  ASSERT_NO_FATAL_FAILURE(store(Size, {Word}));
  std::string Main =
      write("main.c", "#include \"" + Word + "\"\n#include \"" + Size +
                          "\"\n_Static_assert(BITS == 64, \"\");\n");
  std::set<std::string> Entered;
  EXPECT_TRUE(compile(Main, Entered));
  EXPECT_FALSE(Entered.count(Word));
  EXPECT_FALSE(Entered.count(Size));
}

} // namespace

/// Compiles that export their misses, against a daemon that lists every
/// header a compile has timed.
class ExportTest : public AskListTest {
protected:
  void SetUp() override {
    AskListTest::SetUp();
    ExportParts = true;
  }
};

TEST_F(ExportTest, AMissExportsAPartTheNextCompileServes) {
  // The first compile only times the headers, which puts them on the
  // ask-list. The second asks, misses, and exports each header's part. The
  // third is served from the parts: use.h's declarations, its macro, its
  // static inline function, and the u32 it takes from types.h.
  std::string Types = write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "#define USE_OK 1\n"
                                   "u32 count(void);\n"
                                   "static inline u32 twice(u32 x) {\n"
                                   "  return 2 * x;\n}\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Types +
                          "\"\n"
                          "#include \"" +
                          Use +
                          "\"\n"
                          "u32 f(void) { return twice(count()) + "
                          "USE_OK; }\n");
  EXPECT_TRUE(reads(Main, Use)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, Use)) << "nothing is stored yet";
  EXPECT_GE(Daemon->stats().Entries, 2u) << "the misses were not exported";
  EXPECT_FALSE(reads(Main, Use)) << "use.h's part was not served";
}

TEST_F(ExportTest, APartIsExportedAfterTheCompileLoadsOne) {
  // A part refers to an outside declaration by name, even one a loaded part
  // holds. main.c's third compile is served types.h, then misses use.h and
  // exports it: use.h's part names u32, which types.h's part declares.
  std::string Types = write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "u32 count(void);\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Types + "\"\n#include \"" + Use +
                          "\"\n"
                          "u32 f(void) { return count(); }\n");
  std::string OnlyTypes =
      write("t.c", "#include \"" + Types + "\"\nu32 g(void) { return 1; }\n");
  EXPECT_TRUE(reads(Main, Use)) << "nothing is listed yet";
  EXPECT_TRUE(reads(OnlyTypes, Types)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 1u) << "types.h was not exported alone";
  EXPECT_TRUE(reads(Main, Use)) << "use.h is not stored yet";
  EXPECT_FALSE(reads(Main, Types)) << "types.h was not served";
  EXPECT_EQ(Daemon->stats().Entries, 2u)
      << "use.h was not exported after types.h was loaded";
  EXPECT_FALSE(reads(Main, Use)) << "use.h's part was not served";
}

/// nul.h is Clang's __stddef_null.h under a name of its own, with \p Right
/// as the right side of its #if.
static std::string nullHeader(llvm::StringRef Right) {
  return ("#ifndef NUL_H\n#define NUL_H\n#if !defined(MYNULL) || " + Right +
          "\n#undef MYNULL\n#define MYNULL ((void *)0)\n#endif\n"
          "void *nul_get(void);\n#endif\n")
      .str();
}

TEST_F(ExportTest, APartIsServedWhereAMacroItsResultDidNotNeedDiffers) {
  // The #if is true in any compile not building Clang's stddef module, so
  // the part a.c exports with MYNULL defined fits b.c, where it is not. b.c
  // compiles only if the hit leaves MYNULL defined, as the text would.
  std::string Nul =
      write("nul.h", nullHeader("!__building_module(_Builtin_stddef)"));
  std::string A = write("a.c", "#define MYNULL 0\n#include \"" + Nul +
                                   "\"\nvoid *f(void) { return MYNULL; }\n");
  std::string B = write("b.c", "#include \"" + Nul +
                                   "\"\nvoid *g(void) { return MYNULL; }\n");
  EXPECT_TRUE(reads(A, Nul)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Nul)) << "nothing is stored yet";
  EXPECT_FALSE(reads(B, Nul)) << "the part was rejected over MYNULL";
}

TEST_F(ExportTest, APartIsNotServedWhereAMacroItsResultNeededDiffers) {
  // With a right side that is false, MYNULL decides: a.c skips the lines
  // that b.c needs.
  std::string Nul = write("nul.h", nullHeader("0"));
  std::string A = write("a.c", "#define MYNULL 0\n#include \"" + Nul +
                                   "\"\nvoid *f(void) { return 0; }\n");
  std::string B = write("b.c", "#include \"" + Nul +
                                   "\"\nvoid *g(void) { return MYNULL; }\n");
  EXPECT_TRUE(reads(A, Nul)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Nul)) << "nothing is stored yet";
  EXPECT_GE(Daemon->stats().Entries, 1u) << "a.c did not export nul.h";
  EXPECT_TRUE(reads(B, Nul)) << "a part without MYNULL's lines was served";
}

TEST_F(ExportTest, AServedPartEmitsItsVariableDefinitions) {
  // def.h defines def_value and tentatively defines tent_value, which every
  // compile that includes it must emit. A compile served def.h's part emits
  // def_value as an initializer of the part's module, at the import, as
  // Clang does for any module -- the part must list it, since Clang lists a
  // module's initializers only for a module it builds -- and tent_value at
  // the end, as a tentative definition the part handed over. postgres's
  // kwlist_d.h lost `const ScanKeywordList ScanKeywords` this way.
  std::string Def = write("def.h", "#ifndef DEF_H\n#define DEF_H\n"
                                   "int def_value = 42;\nint tent_value;\n"
                                   "#endif\n");
  std::string A = write("a.c", "#include \"" + Def + "\"\n");
  std::string B = write("b.c", "#include \"" + Def + "\"\n");
  EXPECT_TRUE(reads(A, Def)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Def)) << "nothing is stored yet";
  EXPECT_FALSE(reads(B, Def)) << "def.h's part was not served";
  EXPECT_TRUE(Emitted.count("def_value"))
      << "the served part does not emit def_value";
  EXPECT_TRUE(Emitted.count("tent_value"))
      << "the served part does not emit tent_value";
}

TEST_F(ExportTest, AHeaderEndingInAFunctionIsSavedWithItsBody) {
  // last.h ends with a function definition. The parser reads one token past
  // its last `}`, and here that token is next.h's #include, served: loading
  // next.h's part hands its definition of next_value, which must be
  // emitted, to the compile's consumers -- before last_fn's body is
  // attached. last.h must not be written then, or it is saved with last_fn
  // declared but not defined, and a compile served it calls a function it
  // never emits. Every compile has the flag that makes that an error: a copy
  // is served only under the warning flags it was written with.
  ExtraArgs = {"-Werror=undefined-internal"};
  std::string Next = write("next.h", "#ifndef NEXT_H\n#define NEXT_H\n"
                                     "int next_value(void) { return 4; }\n"
                                     "#endif\n");
  std::string Last = write("last.h", "#ifndef LAST_H\n#define LAST_H\n"
                                     "static inline int last_fn(int x) {\n"
                                     "  return x + 1;\n}\n#endif\n");
  std::string N = write("n.c", "#include \"" + Next + "\"\n");
  std::string X = write("x.c", "#include \"" + Last + "\"\n#include \"" + Next +
                                   "\"\nint x(void) { return last_fn(1); }\n");
  std::string Y = write("y.c", "#include \"" + Last +
                                   "\"\nint y(void) { return last_fn(2); }\n");
  EXPECT_TRUE(reads(N, Next)) << "nothing is listed yet";
  EXPECT_TRUE(reads(N, Next)) << "nothing is stored yet";
  EXPECT_FALSE(reads(X, Next)) << "next.h's part was not served";
  EXPECT_TRUE(reads(X, Last)) << "last.h is not stored yet";
  EXPECT_FALSE(reads(Y, Last)) << "last.h's part was not served";
}

TEST_F(ExportTest, APartServesACompileInAnotherDirectory) {
  // one/first.c writes shared.h's part; two/second.c, compiled from a
  // directory with no first.c in it, is served the part and lays out the
  // struct, which looks up the diagnostic state along shared.h's include
  // chain. A part keeps only the files entered inside it: had it kept
  // first.c, under the relative name the compile opened it by, that walk
  // would stop the compile, since the name finds nothing from two/.
  ASSERT_FALSE(llvm::sys::fs::create_directories(Dir + "/one"));
  ASSERT_FALSE(llvm::sys::fs::create_directories(Dir + "/two"));
  std::string Shared = write("shared.h", "#ifndef SHARED_H\n#define SHARED_H\n"
                                         "struct shared { char c; int a; };\n"
                                         "#endif\n");
  write("one/first.c",
        "#include \"" + Shared + "\"\nint one(void) { return 1; }\n");
  write("two/second.c", "#include \"" + Shared +
                            "\"\nint two(void) { return sizeof(struct "
                            "shared); }\n");
  std::set<std::string> Entered;
  ASSERT_TRUE(compile("first.c", Entered, Dir + "/one"))
      << "nothing is listed yet";
  ASSERT_TRUE(compile("first.c", Entered, Dir + "/one"))
      << "nothing is stored yet";
  EXPECT_GE(Daemon->stats().Entries, 1u) << "shared.h was not exported";

  Entered.clear();
  ASSERT_TRUE(compile("second.c", Entered, Dir + "/two"));
  EXPECT_FALSE(Entered.count(Shared)) << "shared.h was not served";
}

TEST_F(ExportTest, AHeaderIsFoldedWithTheHeadersInsideItReadAsText) {
  // inner.h has a copy that fits inside outer.h. main.c misses outer.h and
  // writes its part, a folded copy, so it reads inner.h as text there
  // instead of being served it: a served inner.h would leave its
  // declarations in its own PCM, which outer.h's part cannot hold. other.c
  // is then served outer.h, inner.h included, opening neither file.
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "typedef unsigned long my_size;\n"
                                       "#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                       "#include \"" +
                           Inner +
                           "\"\n"
                           "my_size outer_len(void);\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Outer +
                          "\"\n"
                          "my_size f(void) { return outer_len(); }\n");
  std::string OnlyInner = write(
      "i.c", "#include \"" + Inner + "\"\nmy_size g(void) { return 1; }\n");
  std::string Other =
      write("other.c", "#include \"" + Outer +
                           "\"\n"
                           "my_size h(void) { return outer_len(); }\n");
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(OnlyInner, Inner)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 1u) << "inner.h was not exported alone";

  std::set<std::string> Entered;
  ASSERT_TRUE(compile(Main, Entered));
  EXPECT_TRUE(Entered.count(Inner))
      << "inner.h was served inside outer.h, which was being written";
  EXPECT_EQ(Daemon->stats().Entries, 2u) << "outer.h was not exported";

  Entered.clear();
  ASSERT_TRUE(compile(Other, Entered));
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's folded copy was not served";
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was opened after all";
}

TEST_F(ExportTest, AServedHeaderIsFoldedIntoTheCopyBeingWritten) {
  // Fold everything (CLANG_HEADER_CACHE_FOLD_SERVED): b.c writes outer.h's
  // copy, and inner.h inside it is served from inner.h's own copy instead of
  // read as text. outer.h's copy still holds inner.h's declarations and
  // macros, so c.c is served outer.h alone and never opens inner.h.
  setenv("CLANG_HEADER_CACHE_FOLD_SERVED", "1", 1);
  auto Unset = llvm::make_scope_exit(
      [] { unsetenv("CLANG_HEADER_CACHE_FOLD_SERVED"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "typedef unsigned long my_size;\n"
                                       "struct pair { int a, b; };\n"
                                       "static inline int twice(int x) "
                                       "{ return 2 * x; }\n"
                                       "#define INNER_MAX 42\n"
                                       "my_size inner_len(void);\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Inner +
                           "\"\nmy_size outer_len(struct pair *p);\n"
                           "#define OUTER_MAX (INNER_MAX + 1)\n#endif\n");
  std::string A = write("a.c", "#include \"" + Inner + "\"\n"
                               "int a(void) { return twice(INNER_MAX); }\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n"
                               "int b(void) { return OUTER_MAX; }\n");
  std::string C = write(
      "c.c", "#include \"" + Outer +
                 "\"\nint c(void) {\n  struct pair p = {1, 2};\n"
                 "  my_size s = INNER_MAX + OUTER_MAX + outer_len(&p);\n"
                 "  return twice(p.a) + (int)s + (int)inner_len();\n}\n");
  EXPECT_TRUE(reads(A, Inner)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Inner)) << "nothing is stored yet";
  EXPECT_FALSE(reads(A, Inner)) << "inner.h's copy was not served";
  EXPECT_TRUE(reads(B, Outer)) << "outer.h is not listed yet";
  size_t Before = Daemon->stats().Entries;
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(B, Entered));
  EXPECT_TRUE(Entered.count(Outer)) << "outer.h's text was not read";
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was read as text";
  ASSERT_EQ(Daemon->stats().Entries, Before + 1) << "outer.h was not written";

  Entered.clear();
  ASSERT_TRUE(compile(C, Entered)) << "c.c did not compile";
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's copy was not served";
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was opened";
}

TEST_F(ExportTest, AFoldedHeadersDependenciesAreTheCopys) {
  // inner.h's text tests FAST. b.c writes outer.h's copy with inner.h served
  // and folded in, so outer.h's copy depends on FAST as inner.h's did: d.c,
  // which defines FAST, reads outer.h as text, and gets inner.h's other
  // branch.
  setenv("CLANG_HEADER_CACHE_FOLD_SERVED", "1", 1);
  auto Unset = llvm::make_scope_exit(
      [] { unsetenv("CLANG_HEADER_CACHE_FOLD_SERVED"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "#ifdef FAST\nint speed(void);\n"
                                       "#else\nlong speed(void);\n#endif\n"
                                       "#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Inner + "\"\nint outer(void);\n#endif\n");
  std::string A = write("a.c", "#include \"" + Inner + "\"\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string D = write("d.c", "#define FAST 1\n#include \"" + Outer +
                                   "\"\nint d(void) { return speed(); }\n");
  EXPECT_TRUE(reads(A, Inner)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Inner)) << "nothing is stored yet";
  EXPECT_TRUE(reads(B, Outer)) << "outer.h is not listed yet";
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(B, Entered));
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was read as text";
  std::string Returns;
  AtEnd = [&](ASTContext &Ctx) {
    for (NamedDecl *Dl :
         Ctx.getTranslationUnitDecl()->lookup(&Ctx.Idents.get("speed")))
      if (auto *F = dyn_cast<FunctionDecl>(Dl))
        Returns = F->getReturnType().getAsString();
  };
  EXPECT_TRUE(reads(D, Outer)) << "outer.h's copy was served under FAST";
  AtEnd = nullptr;
  EXPECT_EQ(Returns, "int");
}

TEST_F(ExportTest, ACopyIsWrittenTheSecondTimeItWouldBe) {
  // CLANG_HEADER_CACHE_SECOND_MISS: b.c's second compile misses outer.h and
  // holds its copy back; the third misses in the same state and writes it;
  // the fourth is served it. c.c, in another state, never gets a copy
  // written.
  setenv("CLANG_HEADER_CACHE_SECOND_MISS", "1", 1);
  auto Unset = llvm::make_scope_exit(
      [] { unsetenv("CLANG_HEADER_CACHE_SECOND_MISS"); });
  std::string Outer = write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                                       "#ifdef WIDE\nlong f(void);\n#else\n"
                                       "int f(void);\n#endif\n#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string C = write("c.c", "#define WIDE 1\n#include \"" + Outer + "\"\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  size_t Before = Daemon->stats().Entries;
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  EXPECT_EQ(Daemon->stats().Entries, Before) << "written the first time";
  EXPECT_TRUE(reads(C, Outer)) << "c.c's state has no copy";
  EXPECT_EQ(Daemon->stats().Entries, Before) << "c.c's copy was written";
  EXPECT_TRUE(reads(B, Outer)) << "the copy is not written until now";
  EXPECT_EQ(Daemon->stats().Entries, Before + 1)
      << "not written the second time";
  EXPECT_FALSE(reads(B, Outer)) << "the copy written was not served";
}

TEST_F(ExportTest, ACopyBeingWrittenLinksAServedHeader) {
  // CLANG_HEADER_CACHE_LINK: b.c writes outer.h's copy with inner.h served
  // inside it. outer.h's copy links inner.h's instead of holding it: c.c is
  // served outer.h, inner.h's copy loads first, and neither file is opened.
  setenv("CLANG_HEADER_CACHE_LINK", "1", 1);
  auto Unset =
      llvm::make_scope_exit([] { unsetenv("CLANG_HEADER_CACHE_LINK"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "typedef unsigned long my_size;\n"
                                       "struct pair { int a, b; };\n"
                                       "static inline int twice(int x) "
                                       "{ return 2 * x; }\n"
                                       "#define INNER_MAX 42\n"
                                       "my_size inner_len(void);\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Inner +
                           "\"\nmy_size outer_len(struct pair *p);\n"
                           "#define OUTER_MAX (INNER_MAX + 1)\n#endif\n");
  std::string A = write("a.c", "#include \"" + Inner + "\"\n"
                               "int a(void) { return twice(INNER_MAX); }\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n"
                               "int b(void) { return OUTER_MAX; }\n");
  std::string C = write(
      "c.c", "#include \"" + Outer +
                 "\"\nint c(void) {\n  struct pair p = {1, 2};\n"
                 "  my_size s = INNER_MAX + OUTER_MAX + outer_len(&p);\n"
                 "  return twice(p.a) + (int)s + (int)inner_len();\n}\n");
  EXPECT_TRUE(reads(A, Inner)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Inner)) << "nothing is stored yet";
  EXPECT_FALSE(reads(A, Inner)) << "inner.h's copy was not served";
  EXPECT_TRUE(reads(B, Outer)) << "outer.h is not listed yet";
  size_t Before = Daemon->stats().Entries;
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(B, Entered));
  EXPECT_TRUE(Entered.count(Outer)) << "outer.h's text was not read";
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was read as text";
  ASSERT_EQ(Daemon->stats().Entries, Before + 1) << "outer.h was not written";

  Entered.clear();
  ASSERT_TRUE(compile(C, Entered)) << "c.c did not compile";
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's copy was not served";
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was opened";
}

TEST_F(ExportTest, AHeaderEndingInAFunctionBeforeALinkIsSavedWithItsBody) {
  // As AHeaderEndingInAFunctionIsSavedWithItsBody, with the token past
  // last_fn's `}` an #include served a copy that links another: loading the
  // link hands the compile's consumers an import, and last.h must not be
  // written then either (git's progress.h, saved without stop_progress's
  // body).
  setenv("CLANG_HEADER_CACHE_LINK", "1", 1);
  auto Unset =
      llvm::make_scope_exit([] { unsetenv("CLANG_HEADER_CACHE_LINK"); });
  ExtraArgs = {"-Werror=undefined-internal"};
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "struct pair { int a, b; };\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Inner +
                           "\"\nint outer_sum(struct pair *p);\n#endif\n");
  std::string Last = write("last.h", "#ifndef LAST_H\n#define LAST_H\n"
                                     "static inline int last_fn(int x) {\n"
                                     "  return x + 1;\n}\n#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string X = write("x.c", "#include \"" + Last + "\"\n#include \"" +
                                   Outer +
                                   "\"\nint x(void) { return last_fn(1); }\n");
  std::string Y = write("y.c", "#include \"" + Last +
                                   "\"\nint y(void) { return last_fn(2); }\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  size_t Before = Daemon->stats().Entries;
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, Before + 2)
      << "outer.h and inner.h were not both written";
  EXPECT_FALSE(reads(X, Outer)) << "outer.h's copy was not served";
  EXPECT_TRUE(reads(X, Last)) << "last.h is not stored yet";
  EXPECT_FALSE(reads(Y, Last)) << "last.h's copy was not served";
}

TEST_F(ExportTest, AHeaderWrittenInsideOneBeingWrittenIsLinked) {
  // Both miss in b.c's second compile: inner.h's copy is written inside
  // outer.h's, and outer.h's links it. c.c is served both.
  setenv("CLANG_HEADER_CACHE_LINK", "1", 1);
  auto Unset =
      llvm::make_scope_exit([] { unsetenv("CLANG_HEADER_CACHE_LINK"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "struct pair { int a, b; };\n"
                                       "#define INNER_MAX 42\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Inner +
                           "\"\nint outer_sum(struct pair *p);\n#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string C =
      write("c.c", "#include \"" + Outer +
                       "\"\nint c(void) { struct pair p = {1, INNER_MAX}; "
                       "return outer_sum(&p); }\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  size_t Before = Daemon->stats().Entries;
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, Before + 2)
      << "outer.h and inner.h were not both written";
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(C, Entered)) << "c.c did not compile";
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's copy was not served";
  EXPECT_FALSE(Entered.count(Inner)) << "inner.h was opened";
}

TEST_F(ExportTest, ALinkIsSkippedWhereItsHeaderWasRead) {
  // inner.h reads nothing outer.h's lines set, and outer.h's lines read
  // nothing inner.h sets, so the link is skipped where inner.h was read:
  // d.c reads inner.h first, and is still served outer.h.
  setenv("CLANG_HEADER_CACHE_LINK", "1", 1);
  auto Unset =
      llvm::make_scope_exit([] { unsetenv("CLANG_HEADER_CACHE_LINK"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "typedef unsigned long my_size;\n"
                                       "my_size inner_len(void);\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Inner + "\"\nmy_size outer_len(void);\n#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string D = write("d.c", "#include \"" + Inner + "\"\n#include \"" +
                                   Outer +
                                   "\"\nint d(void) { return (int)(inner_len()"
                                   " + outer_len()); }\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(D, Entered)) << "d.c did not compile";
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's copy was not served";
}

TEST_F(ExportTest, ALinkReadingTheLinesBeforeItIsNotSkipped) {
  // inner.h tests WIDE, which outer.h defines before including it: inner.h
  // depends on outer.h's lines, so where inner.h was read first outer.h's
  // copy is not served -- outer.h's #include of inner.h is skipped there,
  // and plain clang's speed() is d.c's, returning int.
  setenv("CLANG_HEADER_CACHE_LINK", "1", 1);
  auto Unset =
      llvm::make_scope_exit([] { unsetenv("CLANG_HEADER_CACHE_LINK"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "#ifdef WIDE\nlong speed(void);\n"
                                       "#else\nint speed(void);\n#endif\n"
                                       "#endif\n");
  std::string Outer = write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                                       "#define WIDE 1\n#include \"" +
                                           Inner +
                                           "\"\nint outer(void);\n#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string D = write("d.c", "#include \"" + Inner + "\"\n#include \"" +
                                   Outer +
                                   "\"\nint d(void) { return speed(); }\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  std::string Returns;
  AtEnd = [&](ASTContext &Ctx) {
    for (NamedDecl *Dl :
         Ctx.getTranslationUnitDecl()->lookup(&Ctx.Idents.get("speed")))
      if (auto *F = dyn_cast<FunctionDecl>(Dl))
        Returns = F->getReturnType().getAsString();
  };
  EXPECT_TRUE(reads(D, Outer)) << "outer.h's copy was served";
  AtEnd = nullptr;
  EXPECT_EQ(Returns, "int");
}

TEST_F(ExportTest, ALinkWhoseMacroTheLinesReadIsNotSkipped) {
  // outer.h's lines test INNER_FAST, which inner.h defines: where inner.h
  // was read with another INNER_FAST, outer.h's copy, made with inner.h's,
  // does not stand in.
  setenv("CLANG_HEADER_CACHE_LINK", "1", 1);
  auto Unset =
      llvm::make_scope_exit([] { unsetenv("CLANG_HEADER_CACHE_LINK"); });
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "#ifndef NO_FAST\n#define INNER_FAST\n"
                                       "#endif\nint inner(void);\n#endif\n");
  std::string Outer = write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                                       "#include \"" +
                                           Inner +
                                           "\"\n#ifdef INNER_FAST\n"
                                           "int fast(void);\n#else\n"
                                           "long fast(void);\n#endif\n"
                                           "#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string D = write("d.c", "#define NO_FAST\n#include \"" + Inner +
                                   "\"\n#undef NO_FAST\n#include \"" + Outer +
                                   "\"\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  std::string Returns;
  AtEnd = [&](ASTContext &Ctx) {
    for (NamedDecl *Dl :
         Ctx.getTranslationUnitDecl()->lookup(&Ctx.Idents.get("fast")))
      if (auto *F = dyn_cast<FunctionDecl>(Dl))
        Returns = F->getReturnType().getAsString();
  };
  EXPECT_TRUE(reads(D, Outer)) << "outer.h's copy was served";
  AtEnd = nullptr;
  EXPECT_EQ(Returns, "long");
}

TEST_F(ExportTest, OneCopyServesAHeaderWhetherOrNotItsInnerHeaderWasRead) {
  // inner.h reads nothing outer.h's lines set, so outer.h's copy holds it as
  // a submodule (REPORT.md T17). After.c reads inner.h first: the copy is
  // served with the submodule left hidden, its my_size checked against the
  // compile's own. Other.c reads outer.h alone: the submodule loads. One
  // copy serves both, and a later #include of inner.h is skipped.
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "typedef unsigned long my_size;\n"
                                       "my_size inner_len(void);\n"
                                       "#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                       "#include \"" +
                           Inner +
                           "\"\n"
                           "my_size outer_len(void);\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Outer +
                          "\"\n"
                          "my_size f(void) { return outer_len(); }\n");
  std::string After = write(
      "after.c", "#include \"" + Inner + "\"\n#include \"" + Outer +
                     "\"\n"
                     "my_size g(void) { return outer_len() + inner_len(); }\n");
  std::string Other =
      write("other.c", "#include \"" + Outer + "\"\n#include \"" + Inner +
                           "\"\n"
                           "my_size h(void) { return outer_len() + "
                           "inner_len(); }\n");
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is stored yet";
  size_t Stored = Daemon->stats().Entries;
  ASSERT_GE(Stored, 1u) << "outer.h was not exported";

  // After.c reads inner.h, which has no copy of its own yet, and writes
  // one; outer.h it is served.
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(After, Entered));
  EXPECT_FALSE(Entered.count(Outer))
      << "outer.h's copy was not served after inner.h was read";
  EXPECT_LE(Daemon->stats().Entries, Stored + 1)
      << "a second copy of outer.h was written for the other include order";

  Stored = Daemon->stats().Entries;
  Entered.clear();
  ASSERT_TRUE(compile(Other, Entered));
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's copy was not served";
  EXPECT_FALSE(Entered.count(Inner))
      << "inner.h was opened though its submodule loaded";
  EXPECT_EQ(Daemon->stats().Entries, Stored) << "a copy was written";
}

TEST_F(ExportTest, ASkippedSubmodulesMacroTheCopyReadsIsChecked) {
  // inner.h defines INNER_FAST and outer.h's lines after it test it.
  // Outer.h's copy holds inner.h as a submodule (REPORT.md T17). d.c reads
  // inner.h, then undefines INNER_FAST: the submodule would be skipped, but
  // outer.h's lines were parsed with INNER_FAST defined, so the copy does
  // not stand in, and fast returns long, as with plain clang.
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "#define INNER_FAST\n"
                                       "int inner(void);\n#endif\n");
  std::string Outer = write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                                       "#include \"" +
                                           Inner +
                                           "\"\n#ifdef INNER_FAST\n"
                                           "int fast(void);\n#else\n"
                                           "long fast(void);\n#endif\n"
                                           "#endif\n");
  std::string B = write("b.c", "#include \"" + Outer + "\"\n");
  std::string C = write("c.c", "#include \"" + Inner + "\"\n#include \"" +
                                   Outer + "\"\n");
  std::string D = write("d.c", "#include \"" + Inner +
                                   "\"\n#undef INNER_FAST\n#include \"" +
                                   Outer + "\"\n");
  EXPECT_TRUE(reads(B, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(B, Outer)) << "nothing is stored yet";
  EXPECT_FALSE(reads(C, Outer))
      << "the copy was not served with inner.h's submodule skipped";
  std::string Returns;
  AtEnd = [&](ASTContext &Ctx) {
    for (NamedDecl *Dl :
         Ctx.getTranslationUnitDecl()->lookup(&Ctx.Idents.get("fast")))
      if (auto *F = dyn_cast<FunctionDecl>(Dl))
        Returns = F->getReturnType().getAsString();
  };
  EXPECT_TRUE(reads(D, Outer)) << "outer.h's copy was served";
  AtEnd = nullptr;
  EXPECT_EQ(Returns, "long");
}

TEST_F(ExportTest, AGuardDefinedWithoutItsHeaderReadIsNotServed) {
  // fake.c defines inner.h's guard without reading inner.h, with a my_size
  // of its own. Plain clang reads outer.h's text with inner.h left out; the
  // copy was made with inner.h read, so it is not served (REPORT.md T17):
  // fake.c reads outer.h as text, and outer_len returns int, as with plain
  // clang.
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "typedef unsigned long my_size;\n"
                                       "#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                       "#include \"" +
                           Inner +
                           "\"\n"
                           "my_size outer_len(void);\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Outer +
                          "\"\n"
                          "my_size f(void) { return outer_len(); }\n");
  std::string Fake =
      write("fake.c", "#define INNER_H\ntypedef int my_size;\n#include \"" +
                          Outer + "\"\nint g(void) { return outer_len(); }\n");
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is stored yet";
  ASSERT_FALSE(reads(Main, Outer)) << "outer.h's copy was not served";
  std::string Returns;
  AtEnd = [&](ASTContext &Ctx) {
    for (NamedDecl *D :
         Ctx.getTranslationUnitDecl()->lookup(&Ctx.Idents.get("outer_len")))
      if (auto *F = dyn_cast<FunctionDecl>(D))
        Returns = F->getReturnType().getCanonicalType().getAsString();
  };
  EXPECT_TRUE(reads(Fake, Outer)) << "outer.h's copy was served";
  AtEnd = nullptr;
  EXPECT_EQ(Returns, "int");
}

TEST_F(ExportTest, ASubmodulesDependenciesAreTheCopys) {
  // val.h's body depends on VAL, which main.c and other.c define
  // differently. Its dependencies are folded into outer.h's copy (REPORT.md
  // T17), so where VAL differs the copy does not fit, whether or not the
  // compile read val.h; other.c reads outer.h as text, and Clang never
  // compares two bodies of get_val.
  std::string Val = write("val.h", "#ifndef VAL_H\n#define VAL_H\n"
                                   "static inline int get_val(void) {\n"
                                   "  return VAL;\n}\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                       "#include \"" +
                           Val +
                           "\"\n"
                           "int outer(void);\n#endif\n");
  std::string Main =
      write("main.c", "#define VAL 1\n#include \"" + Outer +
                          "\"\n"
                          "int f(void) { return get_val() + outer(); }\n");
  std::string Other =
      write("other.c", "#define VAL 2\n#include \"" + Val +
                           "\"\n#include \"" + Outer +
                           "\"\n"
                           "int g(void) { return get_val() + outer(); }\n");
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is stored yet";
  EXPECT_FALSE(reads(Main, Outer)) << "outer.h's copy was not served";
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(Other, Entered)) << "two bodies of get_val met";
  EXPECT_TRUE(Entered.count(Outer)) << "a copy made where VAL is 1 was served";
}

TEST_F(ExportTest, ADefinitionInASubmoduleThatLoadsSurvivesALaterSkippedOne) {
  // In outer.h's copy, def.h defines struct S and fwd.h, after it, declares
  // it again; the copy's identifier table lists only fwd.h's. other.c reads
  // fwd.h first, so that submodule is skipped while def.h's loads, and the
  // definition must still complete struct S.
  std::string Def = write("def.h", "#ifndef DEF_H\n#define DEF_H\n"
                                   "struct S { int a; };\n#endif\n");
  std::string Fwd = write("fwd.h", "#ifndef FWD_H\n#define FWD_H\n"
                                   "struct S;\nint use(struct S *);\n"
                                   "#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"" +
                           Def + "\"\n#include \"" + Fwd +
                           "\"\nint outer(void);\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Outer +
                          "\"\n"
                          "int f(void) { struct S s = {1}; return s.a; }\n");
  std::string Other =
      write("other.c", "#include \"" + Fwd + "\"\n#include \"" + Outer +
                           "\"\n"
                           "int g(void) { struct S s = {1}; return s.a + "
                           "use(&s); }\n");
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, Outer)) << "nothing is stored yet";
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(Other, Entered)) << "struct S lost its definition";
  EXPECT_FALSE(Entered.count(Outer)) << "outer.h's copy was not served";
}

TEST_F(ExportTest, APartThatDefinesAnOutsideStructIsServed) {
  // a.c declares struct pt before use.h, and use.h defines it. A hit is
  // checked where use.h begins, when struct pt is only declared, so the
  // part's metadata must say "declared", not use.h's own definition.
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "struct pt { int x, y; };\n"
                                   "int area(struct pt *p);\n#endif\n");
  std::string A = write("a.c", "struct pt;\n#include \"" + Use +
                                   "\"\nint f(struct pt *p) { return "
                                   "area(p) + p->x; }\n");
  EXPECT_TRUE(reads(A, Use));
  EXPECT_TRUE(reads(A, Use));
  ASSERT_GE(Daemon->stats().Entries, 1u) << "use.h was not exported";
  EXPECT_FALSE(reads(A, Use)) << "use.h's part was not served";
}

/// Moves \p Path's modification time \p Seconds later, leaving its text.
void moveModificationTime(const std::string &Path, int Seconds) {
  int FD;
  ASSERT_FALSE(llvm::sys::fs::openFileForReadWrite(
      Path, FD, llvm::sys::fs::CD_OpenExisting, llvm::sys::fs::OF_None));
  llvm::sys::fs::file_status Status;
  ASSERT_FALSE(llvm::sys::fs::status(FD, Status));
  EXPECT_FALSE(llvm::sys::fs::setLastAccessAndModificationTime(
      FD, Status.getLastModificationTime() + std::chrono::seconds(Seconds)));
  llvm::sys::fs::closeFile(FD);
}

TEST_F(ExportTest, APartIsNotServedAfterAFileItHoldsChanged) {
  // A part holds the parse of its header, and of every file the header
  // included, as they were when the part was saved. A plain compile reads
  // them again; a hit requires each one's size and modification time
  // unchanged.
  std::string Alone = write("alone.h", "#ifndef ALONE_H\n#define ALONE_H\n"
                                       "int alone(void);\n#endif\n");
  std::string Inner = write("inner.h", "#ifndef INNER_H\n#define INNER_H\n"
                                       "int inner(void);\n#endif\n");
  std::string Outer =
      write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n"
                       "#include \"" +
                           Inner + "\"\nint outer(void);\n#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Alone + "\"\n#include \"" + Outer +
                          "\"\nint f(void) { return alone() + "
                          "outer() + inner(); }\n");
  EXPECT_TRUE(reads(Main, Alone)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, Alone)) << "nothing is stored yet";
  EXPECT_FALSE(reads(Main, Alone)) << "alone.h's part was not served";
  EXPECT_FALSE(reads(Main, Outer)) << "outer.h's part was not served";

  // The same text, a later time: the part saved before does not fit. The
  // compile that reads alone.h saves a new part, which the next one is
  // served.
  moveModificationTime(Alone, 100);
  EXPECT_TRUE(reads(Main, Alone)) << "served after alone.h was touched";
  EXPECT_FALSE(reads(Main, Alone)) << "the new part was not served";

  // inner.h gains a line, and so another size. outer.h's part holds
  // inner.h's old text, so it does not fit.
  write("inner.h", "#ifndef INNER_H\n#define INNER_H\n/* edited */\n"
                   "int inner(void);\n#endif\n");
  EXPECT_TRUE(reads(Main, Outer)) << "served after inner.h changed";
}

TEST_F(ExportTest, APartIsNotServedWhereAnOutsideEnumeratorDiffers) {
  // struct buf's array bound comes from the enumerator BUFN, which the part
  // refers to by name. The bound is a constant in the part, so a compile
  // whose BUFN has another value cannot take it.
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "struct buf { char data[BUFN]; };\n"
                                   "int fill(struct buf *b);\n#endif\n");
  std::string A = write("a.c", "enum { BUFN = 4 };\n#include \"" + Use +
                                   "\"\nint f(struct buf *b) { return "
                                   "fill(b); }\n");
  std::string B = write("b.c", "enum { BUFN = 8 };\n#include \"" + Use +
                                   "\"\nint f(struct buf *b) { return "
                                   "fill(b); }\n");
  EXPECT_TRUE(reads(A, Use));
  EXPECT_TRUE(reads(A, Use));
  ASSERT_GE(Daemon->stats().Entries, 1u) << "use.h was not exported";
  EXPECT_FALSE(reads(A, Use)) << "the same BUFN should have been served";
  EXPECT_TRUE(reads(B, Use)) << "a part built with another BUFN was served";
}

TEST_F(ExportTest, APartIsNotServedWhereAnOutsideStructIsPacked) {
  // struct holder's array bound is sizeof(struct pt), a constant in the
  // part. A packed struct pt has the same members, and so the same ODR
  // hash, but another size.
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "struct holder { char raw[sizeof(struct "
                                   "pt)]; };\n"
                                   "int keep(struct holder *h);\n#endif\n");
  std::string A =
      write("a.c", "struct pt { char c; int i; };\n#include \"" + Use +
                       "\"\nint f(struct holder *h) { return keep(h); }\n");
  std::string B = write("b.c", "struct __attribute__((packed)) pt { char c; "
                               "int i; };\n#include \"" +
                                   Use +
                                   "\"\nint f(struct holder *h) { return "
                                   "keep(h); }\n");
  EXPECT_TRUE(reads(A, Use));
  EXPECT_TRUE(reads(A, Use));
  ASSERT_GE(Daemon->stats().Entries, 1u) << "use.h was not exported";
  EXPECT_FALSE(reads(A, Use)) << "the same struct pt should have been served";
  EXPECT_TRUE(reads(B, Use)) << "a part built with an unpacked pt was served";
}

TEST_F(ExportTest, APartIsNotServedWhereAnOutsideStructDiffers) {
  // use.h's part refers to struct pt by name, and use.h's declarations were
  // compiled against its members. A compile whose struct pt has other
  // members cannot take the part.
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "int area(struct pt *p);\n#endif\n");
  std::string A = write("a.c", "struct pt { int x, y; };\n"
                               "#include \"" +
                                   Use +
                                   "\"\n"
                                   "int f(struct pt *p) { return area(p); }\n");
  std::string B = write("b.c", "struct pt { long x, y, z; };\n"
                               "#include \"" +
                                   Use +
                                   "\"\n"
                                   "int f(struct pt *p) { return area(p); }\n");
  EXPECT_TRUE(reads(A, Use));
  EXPECT_TRUE(reads(A, Use));
  ASSERT_GE(Daemon->stats().Entries, 1u) << "use.h was not exported";
  EXPECT_FALSE(reads(A, Use)) << "the same struct pt should have been served";
  EXPECT_TRUE(reads(B, Use)) << "a part carrying another struct pt was served";
}

TEST_F(ExportTest, APartIsNotServedUnderAnotherName) {
  // A part holds use.h under the name its compile opened it by, which
  // __FILE__ and debug information repeat. A compile that names use.h
  // otherwise reads it.
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "int used(void);\n#endif\n");
  write("rel.c", "#include \"use.h\"\nint f(void) { return used(); }\n");
  std::string Abs = write("abs.c", "#include \"" + Use +
                                       "\"\nint f(void) { return used(); }\n");
  std::set<std::string> E1, E2, E3;
  ASSERT_TRUE(compile("rel.c", E1, /*WorkingDir=*/Dir));
  ASSERT_TRUE(compile("rel.c", E2, /*WorkingDir=*/Dir));
  ASSERT_GE(Daemon->stats().Entries, 1u) << "use.h was not exported";
  EXPECT_TRUE(reads(Abs, Use)) << "a part holding ./use.h served /.../use.h";
}

TEST_F(ExportTest, ASecondCopyOfAHeaderIsNotTakenForTheFirst) {
  // h.h is unguarded and refers to f, declared outside it, by name. a.c's
  // part has f without a prototype, b.c's has f(int), and nothing else tells
  // them apart, so they share a name. c.c includes h.h after each
  // declaration. Taking a.c's part again would leave fp without a
  // prototype; loading b.c's too would declare a second fp. c.c reads h.h
  // the second time, and its fp has f(int)'s prototype.
  std::string H = write("h.h", "extern __typeof__(f) *fp;\n");
  std::string A = write("a.c", "int f();\n#include \"" + H + "\"\n");
  std::string B = write("b.c", "int f(int);\n#include \"" + H + "\"\n");
  std::string C = write(
      "c.c",
      "int f();\n#include \"" + H + "\"\nint f(int);\n#include \"" + H +
          "\"\n_Static_assert(!__builtin_types_compatible_p("
          "__typeof__(fp), int (*)(double)), \"fp has no prototype\");\n");
  EXPECT_TRUE(reads(A, H)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, H)) << "nothing is stored yet";
  EXPECT_TRUE(reads(B, H)) << "a.c's part fits b.c";
  ASSERT_EQ(Daemon->stats().Entries, 2u) << "h.h was not exported twice";
  EXPECT_TRUE(reads(C, H)) << "c.c was served both parts";
}

TEST_F(ExportTest, ACompileWithAnErrorWritesNoPart) {
  // e.h does not compile. A part written after the error would be marked as
  // holding errors, and the next compile offered it would refuse it with an
  // error of its own, on top of e.h's.
  std::string E = write("e.h", "#ifndef E_H\n#define E_H\n"
                               "static inline int broken(void) {\n"
                               "  return undeclared;\n}\n"
                               "int fine(int x);\n#endif\n");
  std::string A = write("a.c", "#include \"" + E +
                                   "\"\nint a(int x) { return fine(x); }\n");
  std::set<std::string> Entered;
  EXPECT_FALSE(compile(A, Entered)) << "e.h compiled";
  EXPECT_FALSE(compile(A, Entered)) << "e.h compiled";
  EXPECT_TRUE(Entered.count(E)) << "e.h was not read";
  EXPECT_EQ(Daemon->stats().Entries, 0u) << "a part was written after an error";
}

TEST_F(ExportTest, AHeaderIncludedUnderTwoMacroStatesIsServedBothTimes) {
  // name.h is included twice, as expat includes xmltok_impl.c three times,
  // each time declaring other names. Its two parts have names of their own,
  // and both are served in one compile.
  std::string Name = write("name.h", "int NAME(void);\n");
  std::string A = write("a.c", "#define NAME first\n#include \"" + Name +
                                   "\"\n#undef NAME\n#define NAME second\n"
                                   "#include \"" +
                                   Name +
                                   "\"\nint f(void) { return first() + "
                                   "second(); }\n");
  EXPECT_TRUE(reads(A, Name)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Name)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 2u) << "name.h was not exported twice";
  EXPECT_FALSE(reads(A, Name)) << "a part of name.h was not served";
}

TEST_F(ExportTest, ACallInACopyFindsTheDeclarationBeforeTheInclude) {
  // check.h calls fatal, which b.c declares before the #include and again
  // after it, as _Noreturn. Plain clang ties the call to the declaration it
  // found where it read check.h, the first; so must a compile served
  // check.h's copy, though it reads check()'s body at the end.
  std::string H = write("check.h", "#ifndef CHECK_H\n#define CHECK_H\n"
                                   "static inline int check(int x) {\n"
                                   "  if (x < 0)\n    fatal(\"negative\");\n"
                                   "  return x * 2;\n}\n#endif\n");
  std::string A = write("a.c", "void fatal(const char *msg);\n#include \"" + H +
                                   "\"\nint a(int x) { return check(x); }\n");
  std::string B =
      write("b.c", "void fatal(const char *msg);\n#include \"" + H +
                       "\"\n_Noreturn void fatal(const char *msg);\n"
                       "int b(int x) { return check(x); }\n");
  EXPECT_TRUE(reads(A, H)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, H)) << "nothing is stored yet";
  std::optional<bool> CallsNoreturn;
  AtEnd = [&](ASTContext &Ctx) {
    DeclContextLookupResult Found =
        Ctx.getTranslationUnitDecl()->lookup(&Ctx.Idents.get("check"));
    auto *Check =
        Found.empty() ? nullptr : dyn_cast<FunctionDecl>(Found.front());
    if (!Check || !Check->getBody())
      return;
    std::function<void(const Stmt *)> Visit = [&](const Stmt *S) {
      if (const auto *Call = dyn_cast<CallExpr>(S))
        if (const FunctionDecl *Callee = Call->getDirectCallee())
          CallsNoreturn = Callee->hasAttr<C11NoReturnAttr>();
      for (const Stmt *Child : S->children())
        if (Child)
          Visit(Child);
    };
    Visit(Check->getBody());
  };
  EXPECT_FALSE(reads(B, H)) << "check.h's copy was not served";
  ASSERT_TRUE(CallsNoreturn.has_value()) << "no call to fatal in check()";
  EXPECT_FALSE(*CallsNoreturn)
      << "the call found the _Noreturn declaration after the #include";
}

/// w.h warns about an unused variable under -Wall.
static const char *const WarningHeader =
    "#ifndef W_H\n#define W_H\n"
    "static inline int w(int x) { int unused_local; return x; }\n#endif\n";

TEST_F(ExportTest, AServedHeaderPrintsItsWarningAgain) {
  ExtraArgs = {"-Wall"};
  std::string W = write("w.h", WarningHeader);
  std::string A = write("a.c", "#include \"" + W + "\"\n");
  std::string B = write("b.c", "#include \"" + W + "\"\n");
  EXPECT_TRUE(reads(A, W)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, W)) << "nothing is stored yet";
  EXPECT_EQ(Warnings, 1u) << "a.c did not warn";
  EXPECT_FALSE(reads(B, W)) << "w.h's copy was not served";
  EXPECT_EQ(Warnings, 1u) << "the served header's warning was not printed";
}

TEST_F(ExportTest, UnderWerrorAServedHeaderWithAWarningFailsTheCompile) {
  // The compile that writes the copy fails too: a warning -Werror made an
  // error does not stop the copy, which is correct, and its warning comes
  // back as an error.
  ExtraArgs = {"-Wall", "-Werror"};
  std::string W = write("w.h", WarningHeader);
  std::string A = write("a.c", "#include \"" + W + "\"\n");
  std::string B = write("b.c", "#include \"" + W + "\"\n");
  std::set<std::string> E1, E2, E3;
  EXPECT_FALSE(compile(A, E1)) << "a.c compiled";
  EXPECT_FALSE(compile(A, E2)) << "a.c compiled";
  ASSERT_EQ(Daemon->stats().Entries, 1u) << "w.h was not exported";
  EXPECT_FALSE(compile(B, E3)) << "b.c compiled, served w.h's copy";
  EXPECT_FALSE(E3.count(W)) << "w.h's copy was not served";
}

TEST_F(ExportTest, ACopyIsNotServedUnderOtherWarningFlags) {
  // Written without -Wall, w.h's copy holds no warning; b.c, with -Wall,
  // must print one, and reads w.h.
  std::string W = write("w.h", WarningHeader);
  std::string A = write("a.c", "#include \"" + W + "\"\n");
  std::string B = write("b.c", "#include \"" + W + "\"\n");
  EXPECT_TRUE(reads(A, W)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, W)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 1u) << "w.h was not exported";
  ExtraArgs = {"-Wall"};
  EXPECT_TRUE(reads(B, W)) << "a copy written without -Wall was served";
  EXPECT_EQ(Warnings, 1u);
}

TEST_F(ExportTest, NotServedWhereAWarningPragmaIsInForce) {
  // b.c silences the warning before the #include: the copy, which holds it,
  // does not fit, and b.c reads w.h.
  ExtraArgs = {"-Wall"};
  std::string W = write("w.h", WarningHeader);
  std::string A = write("a.c", "#include \"" + W + "\"\n");
  std::string B = write("b.c", "#pragma GCC diagnostic ignored "
                               "\"-Wunused-variable\"\n#include \"" +
                                   W + "\"\n");
  EXPECT_TRUE(reads(A, W)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, W)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 1u) << "w.h was not exported";
  EXPECT_TRUE(reads(B, W)) << "served under a warning pragma";
  EXPECT_EQ(Warnings, 0u) << "the pragma did not silence the warning";
}

TEST_F(ExportTest, ServedWhereAWarningPragmaChangesNothing) {
  // b.c ignores -Wsign-compare, which -Wall leaves off in C, as git's files
  // do through git-compat-util.h: no warning's level changes, so a.c's copy
  // fits b.c, and the other way round.
  ExtraArgs = {"-Wall"};
  std::string W = write("w.h", WarningHeader);
  std::string A = write("a.c", "#include \"" + W + "\"\n");
  std::string B = write("b.c", "#pragma GCC diagnostic ignored "
                               "\"-Wsign-compare\"\n#include \"" +
                                   W + "\"\n");
  EXPECT_TRUE(reads(A, W)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, W)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 1u) << "w.h was not exported";
  EXPECT_FALSE(reads(B, W)) << "not served under a pragma that changes nothing";
  EXPECT_EQ(Warnings, 1u) << "the copy did not print w.h's warning";
}

TEST_F(ExportTest, AHeaderLeavingAWarningPragmaInForceIsServedWithIt) {
  // quiet.h silences -Wunused-variable for the rest of the compile, as git's
  // git-compat-util.h does -Wsign-compare. A compile served it must stay
  // silent after it; used.h, read after it, prints with the pragma in force.
  ExtraArgs = {"-Wall"};
  std::string Q = write("quiet.h", "#ifndef QUIET_H\n#define QUIET_H\n"
                                   "#pragma GCC diagnostic ignored "
                                   "\"-Wunused-variable\"\nint quiet(void);\n"
                                   "#endif\n");
  std::string U = write("used.h", "#ifndef USED_H\n#define USED_H\n"
                                  "static inline int used(void) {\n"
                                  "  int unused_local;\n  return 1;\n}\n"
                                  "#endif\n");
  std::string A = write("a.c", "#include \"" + Q + "\"\n#include \"" + U +
                                   "\"\nint f(void) { int unused; return 0; "
                                   "}\n");
  EXPECT_TRUE(reads(A, Q)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Q)) << "nothing is stored yet";
  EXPECT_EQ(Warnings, 0u) << "the pragma did not silence the warnings";
  ASSERT_EQ(Daemon->stats().Entries, 2u) << "quiet.h or used.h not exported";
  EXPECT_FALSE(reads(A, Q)) << "quiet.h's copy was not served";
  EXPECT_FALSE(reads(A, U)) << "used.h's copy was not served";
  EXPECT_EQ(Warnings, 0u) << "the served pragma did not silence a warning";
}

TEST_F(ExportTest, AHeaderLeavingAWarningPragmaPushedIsNotWritten) {
  std::string Q = write("pushed.h", "#ifndef PUSHED_H\n#define PUSHED_H\n"
                                    "#pragma GCC diagnostic push\n"
                                    "int pushed(void);\n#endif\n");
  std::string A = write("a.c", "#include \"" + Q + "\"\n");
  EXPECT_TRUE(reads(A, Q)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Q)) << "nothing is stored yet";
  EXPECT_EQ(Daemon->stats().Entries, 0u) << "pushed.h was exported";
}

/// s.h saves X, defines its own X for sx(), and puts X back.
static const char *const SaveRestoreHeader =
    "#ifndef S_H\n#define S_H\n#pragma push_macro(\"X\")\n#undef X\n"
    "#define X 2\nstatic inline int sx(void) { return X; }\n"
    "#pragma pop_macro(\"X\")\n#endif\n";

TEST_F(ExportTest, AHeaderThatPutsAMacroBackLeavesItAsItWas) {
  // a.c has no X. s.h's copy fits b.c, which defines X, and must leave X as
  // b.c defined it, as the text would (REPORT.md Q11).
  std::string S = write("s.h", SaveRestoreHeader);
  std::string A =
      write("a.c", "#include \"" + S + "\"\nint a(void) { return sx(); }\n");
  std::string B = write("b.c", "#define X 7\n#include \"" + S +
                                   "\"\n_Static_assert(X == 7, \"b.c's X\");\n");
  std::string C = write("c.c", "#include \"" + S +
                                   "\"\n#ifdef X\n#error X is defined\n#endif\n");
  EXPECT_TRUE(reads(A, S)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, S)) << "nothing is stored yet";
  EXPECT_FALSE(reads(B, S)) << "s.h's copy was not served";
  EXPECT_EQ(Warnings, 0u);
  EXPECT_FALSE(reads(C, S)) << "s.h's copy was not served";
}

TEST_F(ExportTest, AHeaderThatPopsWhatItsIncluderPushedNeedsTheSameThere) {
  // p.h's pop takes the X a.c pushed. b.c pushed the same, c.c another.
  std::string P = write("p.h", "#ifndef P_H\n#define P_H\n"
                               "#pragma pop_macro(\"X\")\n"
                               "static inline int px(void) { return X; }\n"
                               "#endif\n");
  auto Main = [&](const char *Name, int Pushed, const std::string &After) {
    return write(Name, "#define X " + std::to_string(Pushed) +
                           "\n#pragma push_macro(\"X\")\n#undef X\n"
                           "#define X 3\n#include \"" +
                           P + "\"\n" + After);
  };
  std::string A = Main("a.c", 1, "");
  std::string B = Main("b.c", 1, "_Static_assert(X == 1, \"popped\");\n");
  std::string C = Main("c.c", 5, "_Static_assert(X == 5, \"popped\");\n");
  EXPECT_TRUE(reads(A, P)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, P)) << "nothing is stored yet";
  EXPECT_FALSE(reads(B, P)) << "p.h's copy was not served";
  EXPECT_EQ(Warnings, 0u);
  EXPECT_TRUE(reads(C, P)) << "p.h's copy was served, popping 5";
}

TEST_F(ExportTest, APushLeftForAnotherHeaderIsReplayed) {
  // begin.h saves X and defines its own; end.h puts X back.
  std::string Begin = write("begin.h", "#pragma push_macro(\"X\")\n#undef X\n"
                                       "#define X 2\n"
                                       "static inline int in_begin(void) "
                                       "{ return 7; }\n");
  std::string End = write("end.h", "#pragma pop_macro(\"X\")\n"
                                   "static inline int in_end(void) "
                                   "{ return 8; }\n");
  auto Main = [&](const char *Name, int Outer) {
    std::string X = std::to_string(Outer);
    return write(Name, "#define X " + X + "\n#include \"" + Begin +
                           "\"\n_Static_assert(X == 2, \"begin.h's X\");\n"
                           "#include \"" +
                           End + "\"\n_Static_assert(X == " + X +
                           ", \"put back\");\n");
  };
  std::string A = Main("a.c", 1);
  std::string B = Main("b.c", 1);
  std::string C = Main("c.c", 5);
  EXPECT_TRUE(reads(A, Begin)) << "nothing is listed yet";
  EXPECT_TRUE(reads(A, Begin)) << "nothing is stored yet";
  ASSERT_EQ(Daemon->stats().Entries, 2u) << "begin.h or end.h not exported";
  // One compile each: a compile that reads end.h writes a copy of its own.
  std::set<std::string> Entered;
  ASSERT_TRUE(compile(B, Entered)) << "b.c did not compile";
  EXPECT_FALSE(Entered.count(Begin)) << "begin.h's copy was not served";
  EXPECT_FALSE(Entered.count(End)) << "end.h's copy was not served";
  EXPECT_EQ(Warnings, 0u);
  Entered.clear();
  ASSERT_TRUE(compile(C, Entered)) << "c.c did not compile";
  EXPECT_FALSE(Entered.count(Begin)) << "begin.h's copy was not served";
  EXPECT_TRUE(Entered.count(End)) << "end.h's copy was served, popping 5";
}

TEST_F(ExportTest, SearchesFoundInABuildAreNotTakenUnderAnotherIncludePath) {
  // h.h's <inner.h> finds a/inner.h under -Ia, b/inner.h under -Ib. Within
  // one build, what a compile under -Ia found must not stand in for a
  // compile under -Ib (REPORT.md I9).
  Build = 7;
  for (const char *D : {"a", "b"}) {
    llvm::SmallString<256> Sub(Dir);
    llvm::sys::path::append(Sub, D);
    ASSERT_FALSE(llvm::sys::fs::create_directory(Sub));
  }
  write("a/inner.h", "#define INNER 1\n");
  write("b/inner.h", "#define INNER 2\n");
  std::string H = write("h.h", "#ifndef H_H\n#define H_H\n#include <inner.h>\n"
                               "int h(void);\n#endif\n");
  std::string Main = write("main.c", "#include \"" + H +
                                         "\"\n_Static_assert(INNER == "
                                         "INNER_WANTED, \"inner.h\");\n");
  ExtraArgs = {"-I" + Dir + "/a", "-DINNER_WANTED=1"};
  EXPECT_TRUE(reads(Main, H)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, H)) << "nothing is stored yet";
  EXPECT_FALSE(reads(Main, H)) << "h.h's copy was not served under -Ia";
  EXPECT_FALSE(reads(Main, H)) << "nor again in the same build";
  ExtraArgs = {"-I" + Dir + "/b", "-DINNER_WANTED=2"};
  EXPECT_TRUE(reads(Main, H)) << "a/inner.h's copy was served under -Ib";
}

TEST_F(ExportTest, AFileChangedIsCaughtByTheNextBuild) {
  // The searches' outcome and the files' sizes and times are shared within
  // a build, which assumes its files do not change while it runs (REPORT.md
  // I9); the next build looks again.
  Build = 7;
  std::string H = write("h.h", "#ifndef H_H\n#define H_H\nint h(void);\n"
                               "#endif\n");
  std::string Main = write("main.c", "#include \"" + H + "\"\n");
  EXPECT_TRUE(reads(Main, H)) << "nothing is listed yet";
  EXPECT_TRUE(reads(Main, H)) << "nothing is stored yet";
  EXPECT_FALSE(reads(Main, H)) << "h.h's copy was not served";
  write("h.h", "#ifndef H_H\n#define H_H\nint h(void); int h2(void);\n"
               "#endif\n");
  moveModificationTime(H, 10);
  Build = 8;
  EXPECT_TRUE(reads(Main, H)) << "a copy of the old h.h was served";
}
