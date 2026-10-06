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
#include "clang/ASTCache/SharedRegion.h"
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
#include "llvm/Testing/Support/Error.h"
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
  CachedAction(std::unique_ptr<CacheClient> Client, std::set<std::string> &Out,
               std::string JobsDir = "", bool ExportParts = false)
      : Client(std::move(Client)), Out(Out), JobsDir(std::move(JobsDir)),
        ExportParts(ExportParts) {}

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

    CompilerInstance CI(std::move(Invocation));
    CI.setDiagnostics(Diags);
    CI.setVerboseOutputStream(llvm::nulls());
    CI.createVirtualFileSystem(VFS);
    CI.createFileManager();

    CachedAction Action(client(), Entered, RecordJobsTo, ExportParts);
    return CI.ExecuteAction(Action) && !Diags->hasErrorOccurred();
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
