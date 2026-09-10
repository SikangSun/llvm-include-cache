//===- unittests/ASTCache/ASTCacheDependencyTrackerTest.cpp -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheDependencyTracker.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/ModuleLoader.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/Support/MemoryBuffer.h"
#include "gtest/gtest.h"
#include <optional>

using namespace clang;
using namespace clang::ast_cache;

namespace {

class ASTCacheDependencyTrackerTest : public ::testing::Test {
protected:
  ASTCacheDependencyTrackerTest()
      : InMemoryFileSystem(
            llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>()),
        FileMgr(FileSystemOptions(), InMemoryFileSystem),
        DiagID(DiagnosticIDs::create()),
        Diags(DiagID, DiagOpts, new IgnoringDiagConsumer()),
        SourceMgr(Diags, FileMgr), TargetOpts(new TargetOptions()) {
    TargetOpts->Triple = "x86_64-apple-darwin11.1.0";
    Target = TargetInfo::CreateTargetInfo(Diags, *TargetOpts);
  }

  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem;
  FileManager FileMgr;
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID;
  DiagnosticOptions DiagOpts;
  DiagnosticsEngine Diags;
  SourceManager SourceMgr;
  LangOptions LangOpts;
  std::shared_ptr<TargetOptions> TargetOpts;
  IntrusiveRefCntPtr<TargetInfo> Target;

  void addFile(StringRef Path, StringRef Content) {
    InMemoryFileSystem->addFile(Path, 0,
                                llvm::MemoryBuffer::getMemBuffer(Content));
  }

  /// Preprocesses /main.c and returns the per-header records collected.
  ///
  /// The header under test is #included from a real main file rather than
  /// being the main file itself: making the header the main file collapses
  /// the nesting this tracker exists to model, and hides whether state
  /// arriving from the includer is correctly treated as external.
  std::vector<HeaderRecord> run(StringRef MainText,
                                 StringRef Predefines = "") {
    addFile("/main.c", MainText);
    auto FE = FileMgr.getOptionalFileRef("/main.c");
    SourceMgr.setMainFileID(
        SourceMgr.createFileID(*FE, SourceLocation(), SrcMgr::C_User));

    HeaderSearchOptions HSOpts;
    TrivialModuleLoader ModLoader;
    HeaderSearch HeaderInfo(HSOpts, SourceMgr, Diags, LangOpts, Target.get());
    auto Dir = FileMgr.getOptionalDirectoryRef("/");
    HeaderInfo.AddSearchPath(DirectoryLookup(*Dir, SrcMgr::C_User, false),
                             /*isAngled=*/false);

    PreprocessorOptions PPOpts;
    Preprocessor PP(PPOpts, Diags, LangOpts, SourceMgr, HeaderInfo, ModLoader,
                    /*IILookup=*/nullptr, /*OwnsHeaderSearch=*/false);
    PP.Initialize(*Target);
    if (!Predefines.empty())
      PP.setPredefines(Predefines.str());

    auto Tracker = std::make_unique<ASTCacheDependencyTracker>(PP);
    ASTCacheDependencyTracker *TrackerPtr = Tracker.get();
    PP.addPPCallbacks(std::move(Tracker));

    PP.EnterMainSourceFile();
    PP.LexTokensUntilEOF();

    // Copy out before the Preprocessor (which owns the tracker) dies.
    return std::vector<HeaderRecord>(TrackerPtr->completed().begin(),
                                      TrackerPtr->completed().end());
  }

  static const HeaderDependencySet *find(const std::vector<HeaderRecord> &Recs,
                                          StringRef PathSuffix) {
    for (const HeaderRecord &R : Recs)
      if (StringRef(R.HeaderPath).ends_with(PathSuffix))
        return &R.Deps;
    return nullptr;
  }

  static bool hasValue(const HeaderDependencySet &D, StringRef Name) {
    for (const ValueDependency &V : D.Value)
      if (V.MacroName == Name)
        return true;
    return false;
  }

  static const ConditionalDependency *cond(const HeaderDependencySet &D,
                                            StringRef Name) {
    for (const ConditionalDependency &C : D.Conditional)
      if (C.MacroName == Name)
        return &C;
    return nullptr;
  }
};

TEST_F(ASTCacheDependencyTrackerTest, InternalMacroIsNotADependency) {
  // Defined and used entirely within the header: its expansion cannot vary
  // across compiles of this header text, so it is not a dependency at all,
  // regardless of the grammar position it appears in.
  addFile("/h.h", "#define ALIGNMENT 16\nint buf[ALIGNMENT];\n");
  auto Recs = run("#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_FALSE(hasValue(*D, "ALIGNMENT"));
}

TEST_F(ASTCacheDependencyTrackerTest, PredefinedMacroIsAValueDependency) {
  addFile("/h.h", "int buf[EXTERNAL];\n");
  auto Recs = run("#include \"h.h\"\n", "#define EXTERNAL 99\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_TRUE(hasValue(*D, "EXTERNAL"));
}

TEST_F(ASTCacheDependencyTrackerTest, MacroFromIncludingFileIsADependency) {
  // A macro defined in the .c file before the #include is the most external
  // state a header can be exposed to. A TU-wide closure classifies it as
  // internal -- the main file is a real file too -- and drops it, so the
  // header caches as if it had no dependency at all.
  addFile("/h.h", "int buf[FROM_MAIN];\n");
  auto Recs = run("#define FROM_MAIN 7\n#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_TRUE(hasValue(*D, "FROM_MAIN"))
      << "a macro defined in the including file is external to the header";
}

TEST_F(ASTCacheDependencyTrackerTest, SubHeaderDefinedMacroIsInternalToParent) {
  // The parent includes the sub-header that defines the macro, so the macro
  // is inside the parent's own closure and cannot vary for the parent.
  addFile("/config.h", "#define ALIGNMENT 16\n");
  addFile("/h.h", "#include \"config.h\"\nint buf[ALIGNMENT];\n");
  auto Recs = run("#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_FALSE(hasValue(*D, "ALIGNMENT"));
}

TEST_F(ASTCacheDependencyTrackerTest, ParentResolvesChildDependency) {
  // The same macro expansion answers the provenance question differently per
  // level: SIZE is defined by the parent, so it is external to the child
  // (a genuine dependency of child.h, which could be included on its own)
  // and internal to the parent (parent.h's own text pins it).
  addFile("/child.h", "int buf[SIZE];\n");
  addFile("/parent.h", "#define SIZE 64\n#include \"child.h\"\n");
  auto Recs = run("#include \"parent.h\"\n");

  const HeaderDependencySet *Child = find(Recs, "child.h");
  ASSERT_NE(Child, nullptr);
  EXPECT_TRUE(hasValue(*Child, "SIZE"))
      << "SIZE comes from outside child.h, so child.h depends on it";

  const HeaderDependencySet *Parent = find(Recs, "parent.h");
  ASSERT_NE(Parent, nullptr);
  EXPECT_FALSE(hasValue(*Parent, "SIZE"))
      << "parent.h defines SIZE itself, so it is not a dependency of parent.h";
}

TEST_F(ASTCacheDependencyTrackerTest, ConditionalRecordedAtEveryOpenLevel) {
  // A branch taken inside the child governs the content of every enclosing
  // header too, since each one's cached artifact embeds that content.
  addFile("/child.h", "#ifdef DEBUG\nint x;\n#endif\n");
  addFile("/parent.h", "#include \"child.h\"\n");
  auto Recs = run("#include \"parent.h\"\n");

  const HeaderDependencySet *Child = find(Recs, "child.h");
  ASSERT_NE(Child, nullptr);
  const ConditionalDependency *CC = cond(*Child, "DEBUG");
  ASSERT_NE(CC, nullptr);
  EXPECT_FALSE(CC->WasDefined);

  const HeaderDependencySet *Parent = find(Recs, "parent.h");
  ASSERT_NE(Parent, nullptr);
  EXPECT_NE(cond(*Parent, "DEBUG"), nullptr);
}

TEST_F(ASTCacheDependencyTrackerTest, DefinedConditionalRecordsTrue) {
  addFile("/h.h", "#ifdef DEBUG\nint x;\n#endif\n");
  auto Recs = run("#include \"h.h\"\n", "#define DEBUG 1\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  const ConditionalDependency *C = cond(*D, "DEBUG");
  ASSERT_NE(C, nullptr);
  EXPECT_TRUE(C->WasDefined);
}

TEST_F(ASTCacheDependencyTrackerTest, DefinedOperatorRecordsConditional) {
  addFile("/h.h", "#if defined(FEATURE_X)\nint x;\n#endif\n");
  auto Recs = run("#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_NE(cond(*D, "FEATURE_X"), nullptr);
}

TEST_F(ASTCacheDependencyTrackerTest, BareUndefinedIdentifierInIfIsRecorded) {
  // "#if HAVE_FEATURE" against an undefined HAVE_FEATURE is the autoconf
  // idiom. It is not #ifdef, has no defined() operator, and expands nothing,
  // so none of Ifdef/Ifndef/Defined/MacroExpands names it. Unrecorded, the
  // header would carry no dependency at all and match every compile --
  // including one that defines the macro and needs the other branch.
  addFile("/h.h", "#if HAVE_ATOMICS\nint big[64];\n#else\nint small[1];\n#endif\n");
  auto Recs = run("#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  const ConditionalDependency *C = cond(*D, "HAVE_ATOMICS");
  ASSERT_NE(C, nullptr)
      << "an undefined identifier that decided an #if must be recorded";
  EXPECT_FALSE(C->WasDefined);
}

TEST_F(ASTCacheDependencyTrackerTest, UndefinedIdentifierInArithmeticIfRecorded) {
  addFile("/h.h", "#if VERSION > 5\nint x;\n#endif\n");
  auto Recs = run("#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_NE(cond(*D, "VERSION"), nullptr);
}

TEST_F(ASTCacheDependencyTrackerTest, ElifdefIsRecorded) {
  // #elifdef is a definedness test like any other. Overriding only one of
  // its two PPCallbacks overloads would hide the other and lose it silently.
  addFile("/h.h", "#ifdef A\nint a;\n#elifdef B\nint b;\n#endif\n");
  auto Recs = run("#include \"h.h\"\n");
  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  EXPECT_NE(cond(*D, "A"), nullptr);
  EXPECT_NE(cond(*D, "B"), nullptr) << "#elifdef's macro must be recorded";
}

TEST_F(ASTCacheDependencyTrackerTest, GuardSuppressedIncludeIsRecorded) {
  // main.c includes config.h first, so h.h's own #include "config.h" is
  // suppressed by the multiple-include optimization: the file is never
  // opened and its #ifndef never lexed, so nothing about it reaches Ifndef.
  // The suppression is still a dependency -- in a TU that had not already
  // included config.h, h.h would have entered it and gained its content.
  addFile("/config.h",
          "#ifndef CONFIG_H\n#define CONFIG_H\n#define MEM_ALIGN 8\n#endif\n");
  addFile("/h.h", "#include \"config.h\"\nstruct Buffer { char d[64]; };\n");
  auto Recs = run("#include \"config.h\"\n#include \"h.h\"\n");

  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  const ConditionalDependency *C = cond(*D, "CONFIG_H");
  ASSERT_NE(C, nullptr)
      << "a guard-suppressed #include must record its controlling macro";
  EXPECT_TRUE(C->WasDefined);
}

TEST_F(ASTCacheDependencyTrackerTest, PragmaOnceSkipRecordsFileIdentity) {
  // #pragma once has no controlling macro, so there is no macro state to
  // compare; the dependency is on the file already having been included.
  addFile("/once.h", "#pragma once\n#define ONCE_VAL 3\n");
  addFile("/h.h", "#include \"once.h\"\nint z;\n");
  auto Recs = run("#include \"once.h\"\n#include \"h.h\"\n");

  const HeaderDependencySet *D = find(Recs, "h.h");
  ASSERT_NE(D, nullptr);
  ASSERT_EQ(D->Inclusion.size(), 1u);
  EXPECT_TRUE(StringRef(D->Inclusion[0].HeaderPath).ends_with("once.h"));
  EXPECT_EQ(D->Inclusion[0].Why, InclusionDependency::Reason::PragmaOnce);
}

} // namespace
