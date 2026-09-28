//===- unittests/ASTCache/SymbolDependencyTest.cpp -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Symbol dependencies need a real parse, not just a Preprocessor: they are
// produced by Sema answering the parser's grammar-disambiguation queries.
// These tests run an actual compile over an in-memory header and check what
// the tracker files against which header.
//
// Scoped to C. C's only symbol-dependent ambiguity is whether an identifier
// names a type -- "T * p;" is a declaration or a multiplication depending on
// it. C++'s template and deduction-guide queries sit after the C early-exit
// in Parser::TryAnnotateTypeOrScopeTokenAfterScopeSpec and are unreachable
// here.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"
#include <memory>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Collects the tracker's per-header records from a real compile. The tracker
/// must be registered in two places -- on the Preprocessor for the macro-side
/// callbacks, and on Sema for the grammar-disambiguation queries -- and Sema
/// does not exist until parsing begins, hence SemaConsumer::InitializeSema.
class Collector {
public:
  std::vector<HeaderRecord> Records;
};

class SymbolConsumer : public SemaConsumer {
public:
  SymbolConsumer(ASTCacheDependencyTracker &Tracker, Collector &Out)
      : Tracker(Tracker), Out(Out) {}

  void InitializeSema(Sema &S) override {
    S.setNameClassificationObserver(Tracker);
    TheSema = &S;
  }

  void HandleTranslationUnit(ASTContext &) override {
    Out.Records.assign(Tracker.completed().begin(), Tracker.completed().end());
    if (TheSema)
      TheSema->clearNameClassificationObserver();
  }

private:
  ASTCacheDependencyTracker &Tracker;
  Collector &Out;
  Sema *TheSema = nullptr;
};

class SymbolAction : public ASTFrontendAction {
public:
  explicit SymbolAction(Collector &Out) : Out(Out) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef) override {
    auto Owned =
        std::make_unique<ASTCacheDependencyTracker>(CI.getPreprocessor());
    ASTCacheDependencyTracker *Raw = Owned.get();
    CI.getPreprocessor().addPPCallbacks(std::move(Owned));
    CI.getPreprocessor().setExpansionCandidateObserver(*Raw);
    return std::make_unique<SymbolConsumer>(*Raw, Out);
  }

private:
  Collector &Out;
};

/// Compiles \p MainSource as C, with \p Headers available for #include.
std::vector<HeaderRecord> run(
    StringRef MainSource,
    std::vector<std::pair<std::string, std::string>> Headers) {
  Collector Out;
  tooling::FileContentMappings Mappings;
  for (auto &H : Headers)
    Mappings.emplace_back(H.first, H.second);

  bool Ok = tooling::runToolOnCodeWithArgs(
      std::make_unique<SymbolAction>(Out), MainSource,
      {"-std=c17", "-I."}, "main.c", "clang-tool",
      std::make_shared<PCHContainerOperations>(), Mappings);
  EXPECT_TRUE(Ok) << "compile failed";
  return Out.Records;
}

const HeaderDependencySet *find(const std::vector<HeaderRecord> &Recs,
                                 StringRef Suffix) {
  for (const HeaderRecord &R : Recs)
    if (StringRef(R.HeaderPath).ends_with(Suffix))
      return &R.Deps;
  return nullptr;
}

const SymbolDependency *sym(const HeaderDependencySet &D, StringRef Name) {
  for (const SymbolDependency &S : D.Symbol)
    if (S.Name == Name)
      return &S;
  return nullptr;
}

TEST(SymbolDependencyTest, TypedefFromIncluderIsASymbolDependency) {
  // "T * p;" parses as a declaration only because T names a type, and T is
  // declared in the .c file -- outside the header entirely. A compile that
  // did not declare T would parse the same text as a multiplication.
  auto Recs = run("typedef int T;\n#include \"h.h\"\n",
                  {{"./h.h", "T *p;\n"}});
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  const SymbolDependency *S = sym(*D, "T");
  ASSERT_NE(S, nullptr) << "T resolved outside the header, so it is a "
                            "dependency of the header";
  EXPECT_EQ(S->Classification, SymbolClassification::Type);
}

TEST(SymbolDependencyTest, TypedefResolvedInsideHeaderIsNotADependency) {
  // Here the header declares T itself, so its own text pins the answer and
  // the classification cannot vary between compiles.
  auto Recs = run("#include \"h.h\"\n",
                  {{"./h.h", "typedef int T;\nT *p;\n"}});
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_EQ(sym(*D, "T"), nullptr);
}

TEST(SymbolDependencyTest, ResolvedTypeIdentityIsRecorded) {
  // Recording only "T is a type" is not enough: two translation units can
  // both answer yes with different types, and the cached Decl graph holds
  // whichever one populated it.
  auto Recs = run("typedef long T;\n#include \"h.h\"\n",
                  {{"./h.h", "T *p;\n"}});
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  const SymbolDependency *S = sym(*D, "T");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->ResolvedTypeName, "long")
      << "the resolved type must be recorded, not just that it is a type";
}

TEST(SymbolDependencyTest, TypedefInSubHeaderIsInternalToParent) {
  // config.h is inside parent.h's closure, so from parent.h's point of view
  // T is pinned; but child.h, which does not include config.h itself,
  // genuinely depends on it.
  auto Recs = run("#include \"parent.h\"\n",
                  {{"./config.h", "typedef int T;\n"},
                   {"./child.h", "T *p;\n"},
                   {"./parent.h",
                    "#include \"config.h\"\n#include \"child.h\"\n"}});

  const HeaderDependencySet *Parent = find(Recs, "parent.h");
  ASSERT_NE(Parent, nullptr);
  EXPECT_EQ(sym(*Parent, "T"), nullptr)
      << "parent.h supplies T itself, so it is not a dependency of parent.h";

  const HeaderDependencySet *Child = find(Recs, "child.h");
  ASSERT_NE(Child, nullptr);
  EXPECT_NE(sym(*Child, "T"), nullptr)
      << "child.h does not supply T, so it depends on it";
}

} // namespace
