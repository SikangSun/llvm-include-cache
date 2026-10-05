//===- unittests/ASTCache/PartExportTest.cpp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exporting one header's own part from an ordinary compile
// (ASTWriter::setPartFilter, DECISIONS.md 2.17), through the experiment's
// CLANG_HEADER_CACHE_EXPORT and CLANG_HEADER_CACHE_LOAD, and serving the
// header from it in a second compile of the same file.
//
//===----------------------------------------------------------------------===//

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
#include <cstdlib>
#include <set>

using namespace clang;

namespace {

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

/// Installs whatever the environment asks for, as cc1 does, and records the
/// files the compile enters.
class EnvAction : public SyntaxOnlyAction {
public:
  explicit EnvAction(std::set<std::string> &Out) : Out(Out) {}

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<EnteredFiles>(CI.getSourceManager(), Out));
    ast_cache::installHeaderCacheHook(CI);
    return true;
  }

private:
  std::set<std::string> &Out;
};

class PartExportTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-part", D));
    Dir = std::string(D);
  }
  void TearDown() override {
    unsetenv("CLANG_HEADER_CACHE_EXPORT");
    unsetenv("CLANG_HEADER_CACHE_LOAD");
    llvm::sys::fs::remove_directories(Dir);
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

  /// Compiles \p Main with the hooks the environment names. Returns whether
  /// it compiled without errors; \p Entered receives the files it opened.
  bool compile(const std::string &Main, std::set<std::string> &Entered) {
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
    std::vector<std::string> ArgStrings = {
        "clang",     "-resource-dir",
        ResourceDir, "-fsyntax-only",
        "-fmodules", "-fno-implicit-module-maps",
        "-std=c17",  "-x",
        "c",         Main};
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
    EnvAction Action(Entered);
    return CI.ExecuteAction(Action) && !Diags->hasErrorOccurred();
  }

  std::string Dir;
};

TEST_F(PartExportTest, AHeadersPartServesTheCompileItCameFrom) {
  // outside.h declares outside_t through a macro, so outside_t's location is
  // in a macro expansion before h.h: one the export drops from its
  // source-location table, and writes as the place the macro was used.
  // h.h has a function body and a macro of its own.
  std::string Outside =
      write("outside.h", "#define MAKE_TYPE(n) typedef int n;\n"
                         "MAKE_TYPE(outside_t)\n"
                         "#define OUTSIDE_ONLY 7\n");
  std::string H = write("h.h", "#ifndef H_H\n#define H_H\n"
                               "#define H_ONE 1\n"
                               "outside_t h_get(void);\n"
                               "static inline int h_twice(int x) {\n"
                               "  return 2 * x;\n}\n"
                               "struct h_pair { outside_t a, b; };\n"
                               "#endif\n");
  std::string Main =
      write("main.c", "#include \"" + Outside + "\"\n#include \"" + H +
                          "\"\n"
                          "int use(void) {\n"
                          "  struct h_pair p = {h_get(), OUTSIDE_ONLY};\n"
                          "  return h_twice(p.a) + H_ONE + p.b;\n}\n");
  std::string Part = Dir + "/h.part.pcm";

  setenv("CLANG_HEADER_CACHE_EXPORT", (H + "=" + Part).c_str(), 1);
  std::set<std::string> First;
  ASSERT_TRUE(compile(Main, First));
  unsetenv("CLANG_HEADER_CACHE_EXPORT");
  ASSERT_TRUE(llvm::sys::fs::exists(Part)) << "nothing was exported";

  setenv("CLANG_HEADER_CACHE_LOAD", (H + "=" + Part).c_str(), 1);
  std::set<std::string> Second;
  EXPECT_TRUE(compile(Main, Second))
      << "the compile failed with h.h served from its part";
  EXPECT_FALSE(Second.count(H)) << "h.h was read instead of served";
  EXPECT_TRUE(Second.count(Outside)) << "outside.h is the compile's own";
}

TEST_F(PartExportTest, APartHoldsOnlyItsOwnDeclarations) {
  // Served into a file that never includes outside.h, the part's h_get
  // still declares its return type -- outside_t came along as a copy -- but
  // outside.h's other names did not: OUTSIDE_ONLY is not a macro there.
  // (In the design, the metadata refuses this hit, since outside_t must
  // already be declared; the experiment's loader checks nothing, which is
  // what lets this test look inside the part.)
  std::string Outside = write("outside.h", "typedef int outside_t;\n"
                                           "int outside_unused(void);\n"
                                           "#define OUTSIDE_ONLY 7\n");
  std::string H = write("h.h", "outside_t h_get(void);\n");
  std::string Main =
      write("main.c", "#include \"" + Outside + "\"\n#include \"" + H + "\"\n");
  std::string Part = Dir + "/h.part.pcm";
  setenv("CLANG_HEADER_CACHE_EXPORT", (H + "=" + Part).c_str(), 1);
  std::set<std::string> First;
  ASSERT_TRUE(compile(Main, First));
  unsetenv("CLANG_HEADER_CACHE_EXPORT");
  ASSERT_TRUE(llvm::sys::fs::exists(Part));

  std::string Other =
      write("other.c",
            "#include \"" + H +
                "\"\n"
                "#ifdef OUTSIDE_ONLY\n#error outside.h's macro came along\n"
                "#endif\n"
                "int f(void) { return h_get(); }\n");
  setenv("CLANG_HEADER_CACHE_LOAD", (H + "=" + Part).c_str(), 1);
  std::set<std::string> Second;
  EXPECT_TRUE(compile(Other, Second));
  EXPECT_FALSE(Second.count(H));
}

} // namespace
