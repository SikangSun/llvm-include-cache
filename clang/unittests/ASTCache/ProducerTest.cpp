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
    R.Header.Path = Header;
    R.Args = {"-std=c17"};
    for (std::string &C : Context)
      R.Context.push_back(IncludeRef{"", std::move(C)});
    return R;
  }

  /// Makes \p Name a directory inside the test directory; returns its path.
  std::string mkdir(llvm::StringRef Name) {
    std::string P = Dir + "/" + Name.str();
    EXPECT_FALSE(llvm::sys::fs::create_directory(P));
    return P;
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
  OtherHeader.Header.Path = "/x/g.h";
  ProduceRequest OtherContext = Base;
  OtherContext.Context = {IncludeRef{"", "/x/other.h"}};
  ProduceRequest OtherArgs = Base;
  OtherArgs.Args.push_back("-DX=1");
  // Same characters, split differently across fields.
  ProduceRequest Resplit = Base;
  Resplit.Context = {IncludeRef{"", "/x/ctx"}, IncludeRef{"", ".h"}};
  // Same file, replayed through the include path instead of by its path.
  ProduceRequest OtherSpelling = Base;
  OtherSpelling.Context = {IncludeRef{"<ctx.h>", "/x/ctx.h"}};

  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherHeader));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherContext));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherArgs));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(Resplit));
  EXPECT_NE(moduleNameFor(Base), moduleNameFor(OtherSpelling));
}

TEST_F(ProducerTest, IncludeNextWorksOnlyWhenTheContextIsReplayedAsWritten) {
  // The shape of Clang's <stdint.h>. first/n.h is found first on the include
  // path and hands over to second/n.h with #include_next, which searches the
  // include path again starting after first/. second/n.h declares the type.
  std::string First = mkdir("first"), Second = mkdir("second");
  std::string N = write("first/n.h", "#ifndef FIRST_N_H\n#define FIRST_N_H\n"
                                     "#include_next <n.h>\n#endif\n");
  write("second/n.h", "typedef unsigned int n_t;\n");
  std::string Use = write("use.h", "n_t get(void);\n");

  ProduceRequest AsWritten = request(Use);
  AsWritten.Args.push_back("-I" + First);
  AsWritten.Args.push_back("-I" + Second);
  AsWritten.Context = {IncludeRef{"<n.h>", N}};
  ProduceResult R = produceHeader(AsWritten);
  EXPECT_TRUE(R.produced()) << R.Reason;

  // Opened by its path, first/n.h was found in no include directory, so its
  // #include_next searches from the start, finds first/n.h again, and the
  // guard makes that a no-op. second/n.h is never read; n_t is undeclared.
  ProduceRequest ByPath = AsWritten;
  ByPath.Context = {IncludeRef{"", N}};
  ProduceResult P = produceHeader(ByPath);
  EXPECT_EQ(P.Stat, ProduceResult::Status::Declined) << P.Reason;
}

TEST_F(ProducerTest, AReplayThatOpensADifferentFileIsDeclined) {
  // Both directories have n.h and first/ is searched first, so <n.h> opens
  // first/n.h here. A request saying <n.h> opened second/n.h came from a
  // compile with some other search order; a copy built here would describe
  // a compile that never happened.
  std::string First = mkdir("first"), Second = mkdir("second");
  std::string InFirst = write("first/n.h", "int in_first;\n");
  std::string InSecond = write("second/n.h", "int in_second;\n");
  std::string H = write("h.h", "int h;\n");
  auto searchBoth = [&](ProduceRequest R) {
    R.Args.push_back("-I" + First);
    R.Args.push_back("-I" + Second);
    return R;
  };

  ProduceRequest Ctx = searchBoth(request(H));
  Ctx.Context = {IncludeRef{"<n.h>", InSecond}};
  ProduceResult RC = produceHeader(Ctx);
  EXPECT_EQ(RC.Stat, ProduceResult::Status::Declined) << RC.Reason;
  EXPECT_NE(RC.Reason.find("opened"), std::string::npos) << RC.Reason;

  ProduceRequest Target = searchBoth(request(InSecond));
  Target.Header.Spelled = "<n.h>";
  ProduceResult RT = produceHeader(Target);
  EXPECT_EQ(RT.Stat, ProduceResult::Status::Declined) << RT.Reason;
  EXPECT_NE(RT.Reason.find("opened"), std::string::npos) << RT.Reason;

  // The file the spelling really finds is fine, and so is no path at all.
  Ctx.Context = {IncludeRef{"<n.h>", InFirst}};
  ProduceResult Right = produceHeader(Ctx);
  EXPECT_TRUE(Right.produced()) << Right.Reason;
  Ctx.Context = {IncludeRef{"<n.h>", ""}};
  ProduceResult Unchecked = produceHeader(Ctx);
  EXPECT_TRUE(Unchecked.produced()) << Unchecked.Reason;
}

TEST_F(ProducerTest, AHeaderThatReadsTheCounterIsDeclined) {
  // A saved copy holding __COUNTER__'s values 0 and 1 would be wrong in a
  // compile whose counter is already at 7. Until the backfill rewrites such
  // values on load (DECISIONS.md 2.7), the header is not saved.
  std::string Direct = write("direct.h", "int a = __COUNTER__;\n");
  ProduceResult D = produceHeader(request(Direct));
  EXPECT_EQ(D.Stat, ProduceResult::Status::Declined) << D.Reason;
  EXPECT_NE(D.Reason.find("__COUNTER__"), std::string::npos) << D.Reason;

  // Through a macro the context defines: the counter is still read inside
  // the header.
  std::string Ctx = write("ctx.h", "#define NEXT_ID __COUNTER__\n");
  std::string Indirect = write("indirect.h", "int b = NEXT_ID;\n");
  ProduceResult I = produceHeader(request(Indirect, {Ctx}));
  EXPECT_EQ(I.Stat, ProduceResult::Status::Declined) << I.Reason;

  // The context reading the counter does not matter: the context's
  // declarations stay hidden, and the header never reads the counter.
  std::string CtxReads = write("ctx_reads.h", "int c = __COUNTER__;\n");
  std::string Plain = write("plain.h", "int d;\n");
  ProduceResult P = produceHeader(request(Plain, {CtxReads}));
  EXPECT_TRUE(P.produced()) << P.Reason;

  // __LINE__ is the header's own line number in every compile.
  std::string Line = write("line.h", "int e = __LINE__;\n");
  ProduceResult L = produceHeader(request(Line));
  EXPECT_TRUE(L.produced()) << L.Reason;
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
  ProduceRequest J1 = request("/x/h.h", {"/x/fmacros.h"});
  J1.Args.push_back("-DMSG=hello world"); // spaces inside a value survive
  J1.Context.push_back({"<stdint.h>", "/usr/lib/clang/22/include/stdint.h"});
  J1.Context.push_back({"\"my dir/x.h\"", "/x/my dir/x.h"});
  J1.Context.push_back({"<stdio.h>", ""}); // spelling, unchecked
  ProduceRequest J2 = request("/x/g.h");
  J2.Header.Spelled = "<g.h>";
  J2.Args.clear();

  std::string Path = Dir + "/jobs.txt";
  ASSERT_THAT_ERROR(writeJobFile(Path, {J1, J2}), llvm::Succeeded());

  llvm::Expected<std::vector<ProduceRequest>> Back = readJobFile(Path);
  ASSERT_THAT_EXPECTED(Back, llvm::Succeeded());
  ASSERT_EQ(Back->size(), 2u);
  EXPECT_EQ((*Back)[0].Header, J1.Header);
  EXPECT_EQ((*Back)[0].Args, J1.Args);
  EXPECT_EQ((*Back)[0].Context, J1.Context);
  EXPECT_EQ((*Back)[1].Header, J2.Header);
  EXPECT_TRUE((*Back)[1].Args.empty());
  EXPECT_TRUE((*Back)[1].Context.empty());
}

TEST_F(ProducerTest, JobFilesRejectWhatTheyCannotRepresent) {
  ProduceRequest Bad = request("/x/h.h");
  Bad.Args.push_back("-DA=1\n-DB=2");
  EXPECT_THAT_EXPECTED(encodeJobs({Bad}), llvm::Failed())
      << "a line break would silently split one argument into two";

  ProduceRequest BadSpelling = request("/x/h.h");
  BadSpelling.Context.push_back({"<stdint.h", "/x/stdint.h"});
  EXPECT_THAT_EXPECTED(encodeJobs({BadSpelling}), llvm::Failed());
  ProduceRequest PathLikeSpelling = request("/x/h.h");
  PathLikeSpelling.Context.push_back({"", "<odd>.h"});
  EXPECT_THAT_EXPECTED(encodeJobs({PathLikeSpelling}), llvm::Failed())
      << "would read back as a spelling";

  const std::string Magic = "clang-header-cache-jobs 2\n";
  EXPECT_THAT_EXPECTED(decodeJobs("not a job file\n"), llvm::Failed());
  EXPECT_THAT_EXPECTED(
      decodeJobs("clang-header-cache-jobs 1\njob\nheader /x/h.h\nend\n"),
      llvm::Failed())
      << "version 1 had no spellings";
  EXPECT_THAT_EXPECTED(decodeJobs(Magic + "job\nheader /x/h.h\n"),
                       llvm::Failed())
      << "a job with no end line";
  EXPECT_THAT_EXPECTED(decodeJobs(Magic + "job\narg -DX\nend\n"),
                       llvm::Failed())
      << "a job with no header";
  EXPECT_THAT_EXPECTED(
      decodeJobs(Magic + "job\nheader /x/h.h\nbogus line\nend\n"),
      llvm::Failed());
  EXPECT_THAT_EXPECTED(decodeJobs(Magic + "job\nheader <g.h>\nend\n"),
                       llvm::Failed())
      << "a header needs a path";
  EXPECT_THAT_EXPECTED(
      decodeJobs(Magic + "job\nheader /x/h.h\ncontext <stdint.h\nend\n"),
      llvm::Failed())
      << "an unterminated spelling";
}

} // namespace
