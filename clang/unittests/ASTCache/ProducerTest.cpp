//===- unittests/ASTCache/ProducerTest.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Building a saved copy in a given context. Every test writes real files,
// because the producer runs a real compile against the file system.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheProducer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

class ProducerTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-producer", D));
    Dir = std::string(D);
  }
  void TearDown() override { llvm::sys::fs::remove_directories(Dir); }

  /// Writes \p Text to \p Name inside the test directory; returns the path.
  std::string write(llvm::StringRef Name, llvm::StringRef Text) {
    llvm::SmallString<256> P(Dir);
    llvm::sys::path::append(P, Name);
    std::error_code EC;
    llvm::raw_fd_ostream OS(P, EC);
    EXPECT_FALSE(EC);
    OS << Text;
    return std::string(P);
  }

  ProduceRequest request(const std::string &Header,
                         std::vector<std::string> Context = {}) {
    ProduceRequest R;
    R.HeaderPath = Header;
    R.Args = {"-std=c17"};
    R.Context = std::move(Context);
    return R;
  }

  std::string Dir;
};

const SymbolDependency *symbol(const HeaderDependencySet &D,
                               llvm::StringRef Name) {
  for (const SymbolDependency &S : D.Symbol)
    if (S.Name == Name)
      return &S;
  return nullptr;
}

const ConditionalDependency *conditional(const HeaderDependencySet &D,
                                         llvm::StringRef Name) {
  for (const ConditionalDependency &C : D.Conditional)
    if (C.MacroName == Name)
      return &C;
  return nullptr;
}

TEST_F(ProducerTest, ContextMakesAHeaderThatNeedsItsIncluderSaveable) {
  // use.h names u32 but never includes the header that declares it -- the
  // shape of redis's anet.h and uint32_t. In a real build use.h always
  // compiles, because whatever includes use.h included types.h first.
  std::string Types = write("types.h", "#ifndef TYPES_H\n#define TYPES_H\n"
                                       "typedef unsigned int u32;\n#endif\n");
  std::string Use = write("use.h", "#ifndef USE_H\n#define USE_H\n"
                                   "u32 counter(void);\n#endif\n");

  ProduceResult Alone = produceHeader(request(Use));
  EXPECT_FALSE(Alone.produced()) << "with nothing in front, u32 is undeclared";

  ProduceResult WithContext = produceHeader(request(Use, {Types}));
  ASSERT_TRUE(WithContext.produced()) << WithContext.Reason;

  // Needing u32 from the context is a dependency, recorded like any other: a
  // loading compile must resolve u32 to the same type.
  const SymbolDependency *S = symbol(WithContext.Push.Deps, "u32");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Classification, SymbolClassification::Type);
  EXPECT_EQ(S->ResolvedTypeName, "unsigned int");
}

TEST_F(ProducerTest, AMacroFromTheContextIsAConditionalDependency) {
  // The _GNU_SOURCE pattern: a macro set by an earlier header decides what a
  // later header declares. It is ordinary macro state; nothing special.
  std::string Feat = write("feat.h", "#define FEATURE 1\n");
  std::string H =
      write("h.h", "#ifdef FEATURE\nint with_feature;\n#endif\nint always;\n");

  ProduceResult With = produceHeader(request(H, {Feat}));
  ASSERT_TRUE(With.produced()) << With.Reason;
  const ConditionalDependency *C = conditional(With.Push.Deps, "FEATURE");
  ASSERT_NE(C, nullptr);
  EXPECT_TRUE(C->WasDefined);

  ProduceResult Without = produceHeader(request(H));
  ASSERT_TRUE(Without.produced()) << Without.Reason;
  const ConditionalDependency *C2 = conditional(Without.Push.Deps, "FEATURE");
  ASSERT_NE(C2, nullptr);
  EXPECT_FALSE(C2->WasDefined);
}

TEST_F(ProducerTest, PragmaEntryIsTheStateTheHeaderSaw) {
  // The pragma snapshot must be taken where the header begins, not where the
  // compile begins, because the context in front of the header can change it.
  std::string Pack = write("pack.h", "#pragma pack(1)\n");
  std::string S = write("s.h", "struct S { char c; int x; };\n");

  ProduceResult Plain = produceHeader(request(S));
  ProduceResult Packed = produceHeader(request(S, {Pack}));
  ASSERT_TRUE(Plain.produced()) << Plain.Reason;
  ASSERT_TRUE(Packed.produced()) << Packed.Reason;

  EXPECT_NE(Plain.Push.Deps.Entry, Packed.Push.Deps.Entry)
      << "#pragma pack(1) in the context must show up in s.h's entry state";
  EXPECT_EQ(Packed.Push.Deps.Exit, Packed.Push.Deps.Entry)
      << "s.h changes no pragma, so s.h leaves the state it found";
}

TEST_F(ProducerTest, AHeaderReachedThroughItsOwnContextIsDeclined) {
  // a.h includes b.h. Saving b.h with a.h in front would compile b.h inside
  // the hidden context submodule and leave the visible target submodule
  // empty. A header reached from inside another is saved as part of that
  // other header's copy instead.
  std::string B = write("b.h", "#ifndef B_H\n#define B_H\n"
                               "int from_b(void);\n#endif\n");
  std::string A = write("a.h", "#ifndef A_H\n#define A_H\n#include \"b.h\"\n"
                               "int from_a(void);\n#endif\n");

  ProduceResult R = produceHeader(request(B, {A}));
  EXPECT_EQ(R.Stat, ProduceResult::Status::Declined) << R.Reason;
}

TEST_F(ProducerTest, ModuleNamesAreUniquePerRequestAndStable) {
  ProduceRequest Base = request("/x/h.h", {"/x/ctx.h"});
  EXPECT_EQ(moduleNameFor(Base), moduleNameFor(Base))
      << "the same request must always get the same name";
  EXPECT_EQ(moduleNameFor(Base).rfind("__hc_", 0), 0u);

  ProduceRequest OtherHeader = Base;
  OtherHeader.HeaderPath = "/x/g.h";
  ProduceRequest OtherContext = Base;
  OtherContext.Context = {"/x/other.h"};
  ProduceRequest OtherArgs = Base;
  OtherArgs.Args.push_back("-DX=1");
  // Same characters, split differently across fields.
  ProduceRequest Resplit = Base;
  Resplit.Context = {"/x/ctx", ".h"};

  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherHeader));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherContext));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherArgs));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(Resplit));
}

TEST_F(ProducerTest, TwoHeadersGetTwoModuleNames) {
  // Two modules with the same name cannot both be loaded into one compile,
  // so two saved copies loaded together must never share a name.
  std::string A = write("one.h", "int one;\n");
  std::string B = write("two.h", "int two;\n");
  ProduceResult RA = produceHeader(request(A));
  ProduceResult RB = produceHeader(request(B));
  ASSERT_TRUE(RA.produced()) << RA.Reason;
  ASSERT_TRUE(RB.produced()) << RB.Reason;
  EXPECT_NE(RA.ModuleName, RB.ModuleName);
}

TEST_F(ProducerTest, MissingFilesFail) {
  EXPECT_EQ(produceHeader(request(Dir + "/nope.h")).Stat,
            ProduceResult::Status::Failed);
  std::string H = write("h2.h", "int h2;\n");
  EXPECT_EQ(produceHeader(request(H, {Dir + "/nope.h"})).Stat,
            ProduceResult::Status::Failed);
}

TEST_F(ProducerTest, JobFilesRoundTrip) {
  ProduceRequest J1 = request("/x/h.h", {"/x/fmacros.h", "/x/stdio.h"});
  J1.Args.push_back("-DMSG=hello world"); // spaces inside a value survive
  ProduceRequest J2 = request("/x/g.h");
  J2.Args.clear();

  std::string Path = Dir + "/jobs.txt";
  ASSERT_THAT_ERROR(writeJobFile(Path, {J1, J2}), llvm::Succeeded());

  llvm::Expected<std::vector<ProduceRequest>> Back = readJobFile(Path);
  ASSERT_THAT_EXPECTED(Back, llvm::Succeeded());
  ASSERT_EQ(Back->size(), 2u);
  EXPECT_EQ((*Back)[0].HeaderPath, J1.HeaderPath);
  EXPECT_EQ((*Back)[0].Args, J1.Args);
  EXPECT_EQ((*Back)[0].Context, J1.Context);
  EXPECT_EQ((*Back)[1].HeaderPath, J2.HeaderPath);
  EXPECT_TRUE((*Back)[1].Args.empty());
  EXPECT_TRUE((*Back)[1].Context.empty());
}

TEST_F(ProducerTest, JobFilesRejectWhatTheyCannotRepresent) {
  ProduceRequest Bad = request("/x/h.h");
  Bad.Args.push_back("-DA=1\n-DB=2");
  EXPECT_THAT_EXPECTED(encodeJobs({Bad}), llvm::Failed())
      << "a line break would silently split one argument into two";

  EXPECT_THAT_EXPECTED(decodeJobs("not a job file\n"), llvm::Failed());
  EXPECT_THAT_EXPECTED(
      decodeJobs("clang-header-cache-jobs 1\njob\nheader /x/h.h\n"),
      llvm::Failed())
      << "a job with no end line";
  EXPECT_THAT_EXPECTED(
      decodeJobs("clang-header-cache-jobs 1\njob\narg -DX\nend\n"),
      llvm::Failed())
      << "a job with no header";
  EXPECT_THAT_EXPECTED(decodeJobs("clang-header-cache-jobs 1\njob\n"
                                  "header /x/h.h\nbogus line\nend\n"),
                       llvm::Failed());
}

} // namespace
