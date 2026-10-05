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

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
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
#include <functional>
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

using AtEndFn = std::function<void(ASTContext &)>;

/// Installs whatever the environment asks for, as cc1 does, and records the
/// files the compile enters. \p AtEnd, if set, looks at the finished AST.
class EnvAction : public SyntaxOnlyAction {
public:
  EnvAction(std::set<std::string> &Out, AtEndFn AtEnd)
      : Out(Out), AtEnd(std::move(AtEnd)) {}

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<EnteredFiles>(CI.getSourceManager(), Out));
    ast_cache::installHeaderCacheHook(CI);
    return true;
  }

  void EndSourceFileAction() override {
    if (AtEnd && getCompilerInstance().hasASTContext())
      AtEnd(getCompilerInstance().getASTContext());
    SyntaxOnlyAction::EndSourceFileAction();
  }

private:
  std::set<std::string> &Out;
  AtEndFn AtEnd;
};

/// The file-scope declaration of kind \p T named \p Name, or null.
template <typename T> T *find(ASTContext &Ctx, llvm::StringRef Name) {
  for (Decl *D : Ctx.getTranslationUnitDecl()->decls())
    if (auto *ND = dyn_cast<T>(D))
      if (ND->getIdentifier() && ND->getName() == Name)
        return ND;
  return nullptr;
}

/// The first expression of kind \p T in \p S, depth first, or null.
template <typename T> const T *first(const Stmt *S) {
  if (!S)
    return nullptr;
  if (const auto *E = dyn_cast<T>(S))
    return E;
  for (const Stmt *Child : S->children())
    if (const T *E = first<T>(Child))
      return E;
  return nullptr;
}

/// The first enumerator \p S refers to, depth first, or null.
const EnumConstantDecl *firstEnumerator(const Stmt *S) {
  if (!S)
    return nullptr;
  if (const auto *Ref = dyn_cast<DeclRefExpr>(S))
    if (const auto *E = dyn_cast<EnumConstantDecl>(Ref->getDecl()))
      return E;
  for (const Stmt *Child : S->children())
    if (const EnumConstantDecl *E = firstEnumerator(Child))
      return E;
  return nullptr;
}

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
    unsetenv("CLANG_HEADER_CACHE_OUTSIDE");
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
  bool compile(const std::string &Main, std::set<std::string> &Entered,
               AtEndFn AtEnd = nullptr) {
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
    EnvAction Action(Entered, std::move(AtEnd));
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
  // Written with deep copies and served into a file that never includes
  // outside.h, the part's h_get still declares its return type -- outside_t
  // came along as a copy -- but outside.h's other names did not:
  // OUTSIDE_ONLY is not a macro there. (In the design, the metadata refuses
  // this hit, since outside_t must already be declared; the experiment's
  // loader checks nothing, which is what lets this test look inside the
  // part.)
  std::string Outside = write("outside.h", "typedef int outside_t;\n"
                                           "int outside_unused(void);\n"
                                           "#define OUTSIDE_ONLY 7\n");
  std::string H = write("h.h", "outside_t h_get(void);\n");
  std::string Main =
      write("main.c", "#include \"" + Outside + "\"\n#include \"" + H + "\"\n");
  std::string Part = Dir + "/h.part.pcm";
  setenv("CLANG_HEADER_CACHE_EXPORT", (H + "=" + Part).c_str(), 1);
  setenv("CLANG_HEADER_CACHE_OUTSIDE", "copy", 1);
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

TEST_F(PartExportTest, AnOutsideDeclarationIsTheLoadingCompilesOwn) {
  // h.h's part refers to outside_t, struct pt, its field x and the
  // enumerator RED by name. Loaded, its declarations use this compile's own
  // declarations of them, not copies from the part.
  std::string Outside = write("outside.h", "typedef int outside_t;\n"
                                           "struct pt { int x, y; };\n"
                                           "enum { RED = 3 };\n");
  std::string H = write("h.h", "outside_t h_get(void);\n"
                               "static inline int h_x(struct pt *p) {\n"
                               "  return p->x + RED;\n}\n");
  std::string Main = write(
      "main.c", "#include \"" + Outside + "\"\n#include \"" + H +
                    "\"\n"
                    "int use(struct pt *p) { return h_get() + h_x(p); }\n");
  std::string Part = Dir + "/h.part.pcm";
  setenv("CLANG_HEADER_CACHE_EXPORT", (H + "=" + Part).c_str(), 1);
  std::set<std::string> First;
  ASSERT_TRUE(compile(Main, First));
  unsetenv("CLANG_HEADER_CACHE_EXPORT");
  ASSERT_TRUE(llvm::sys::fs::exists(Part));

  setenv("CLANG_HEADER_CACHE_LOAD", (H + "=" + Part).c_str(), 1);
  std::set<std::string> Second;
  bool Checked = false;
  EXPECT_TRUE(compile(Main, Second, [&](ASTContext &Ctx) {
    auto *Get = find<FunctionDecl>(Ctx, "h_get");
    auto *X = find<FunctionDecl>(Ctx, "h_x");
    ASSERT_TRUE(Get && X);
    EXPECT_TRUE(Get->isFromASTFile()) << "h.h was not served";
    const auto *T = Get->getReturnType()->getAs<TypedefType>();
    ASSERT_TRUE(T);
    EXPECT_FALSE(T->getDecl()->isFromASTFile())
        << "outside_t is a copy from the part";
    RecordDecl *Pt =
        X->getParamDecl(0)->getType()->getPointeeType()->getAsRecordDecl();
    ASSERT_TRUE(Pt);
    EXPECT_FALSE(Pt->isFromASTFile()) << "struct pt is a copy from the part";
    const auto *Member = first<MemberExpr>(X->getBody());
    ASSERT_TRUE(Member);
    EXPECT_FALSE(Member->getMemberDecl()->isFromASTFile())
        << "the field x is a copy from the part";
    const Decl *Red = firstEnumerator(X->getBody());
    ASSERT_TRUE(Red);
    EXPECT_FALSE(Red->isFromASTFile()) << "RED is a copy from the part";
    Checked = true;
  })) << "the compile failed with h.h served from its part";
  EXPECT_TRUE(Checked);
  EXPECT_FALSE(Second.count(H)) << "h.h was read instead of served";
}

TEST_F(PartExportTest, AnOutsideDeclarationIsADeepCopyWhenAskedFor) {
  // CLANG_HEADER_CACHE_OUTSIDE=copy, the safety net: the part holds a copy
  // of outside_t, which the loading compile merges with its own.
  std::string Outside = write("outside.h", "typedef int outside_t;\n");
  std::string H = write("h.h", "outside_t h_get(void);\n");
  std::string Main =
      write("main.c", "#include \"" + Outside + "\"\n#include \"" + H + "\"\n");
  std::string Part = Dir + "/h.part.pcm";
  setenv("CLANG_HEADER_CACHE_EXPORT", (H + "=" + Part).c_str(), 1);
  setenv("CLANG_HEADER_CACHE_OUTSIDE", "copy", 1);
  std::set<std::string> First;
  ASSERT_TRUE(compile(Main, First));
  unsetenv("CLANG_HEADER_CACHE_EXPORT");
  ASSERT_TRUE(llvm::sys::fs::exists(Part));

  setenv("CLANG_HEADER_CACHE_LOAD", (H + "=" + Part).c_str(), 1);
  std::set<std::string> Second;
  bool Checked = false;
  EXPECT_TRUE(compile(Main, Second, [&](ASTContext &Ctx) {
    auto *Get = find<FunctionDecl>(Ctx, "h_get");
    ASSERT_TRUE(Get);
    const auto *T = Get->getReturnType()->getAs<TypedefType>();
    ASSERT_TRUE(T);
    EXPECT_TRUE(T->getDecl()->isFromASTFile()) << "outside_t is not a copy";
    Checked = true;
  }));
  EXPECT_TRUE(Checked);
}

TEST_F(PartExportTest, ADefinitionAfterTheHeaderIsNotInThePart) {
  // main.c defines later() right after h.h declares it, so the definition
  // is finished when h.h's part is written. The part holds h.h's
  // declaration only: other.c, served from it, does not define later().
  std::string H = write("h.h", "int later(void);\n");
  std::string Main = write("main.c", "#include \"" + H +
                                         "\"\n"
                                         "int later(void) { return 1; }\n"
                                         "int g(void) { return later(); }\n");
  std::string Other = write("other.c", "#include \"" + H +
                                           "\"\n"
                                           "int f(void) { return later(); }\n");
  for (const char *Outside : {"name", "copy"}) {
    SCOPED_TRACE(Outside);
    std::string Part = Dir + "/h." + Outside + ".pcm";
    setenv("CLANG_HEADER_CACHE_OUTSIDE", Outside, 1);
    setenv("CLANG_HEADER_CACHE_EXPORT", (H + "=" + Part).c_str(), 1);
    std::set<std::string> First;
    ASSERT_TRUE(compile(Main, First));
    unsetenv("CLANG_HEADER_CACHE_EXPORT");
    ASSERT_TRUE(llvm::sys::fs::exists(Part));

    setenv("CLANG_HEADER_CACHE_LOAD", (H + "=" + Part).c_str(), 1);
    std::set<std::string> Second;
    bool Checked = false;
    EXPECT_TRUE(compile(Other, Second, [&](ASTContext &Ctx) {
      auto *Later = find<FunctionDecl>(Ctx, "later");
      ASSERT_TRUE(Later);
      EXPECT_TRUE(Later->isFromASTFile()) << "h.h was not served";
      EXPECT_FALSE(Later->isDefined()) << "main.c's definition came along";
      Checked = true;
    }));
    unsetenv("CLANG_HEADER_CACHE_LOAD");
    EXPECT_TRUE(Checked);
  }
}

} // namespace
