//===- unittests/ASTCache/EndToEndTest.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The whole chain over a real socket: a daemon on its own thread, a client
// that pushes an entry, and a second client -- running inside an actual
// compile, so it has a live Preprocessor and Sema -- that looks it up and
// decides whether it applies.
//
// The matching is what these tests are really about. Transport is covered by
// CacheProtocolTest; what cannot be checked without a real compile is whether
// a stored record is accepted exactly when it should be.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/FileSystem.h"
#include "gtest/gtest.h"
#include <thread>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Runs a callable with a live Preprocessor and Sema from a real compile.
class ProbeConsumer : public SemaConsumer {
public:
  using Fn = std::function<void(Preprocessor &, Sema &)>;
  ProbeConsumer(Preprocessor &PP, Fn F) : PP(PP), F(std::move(F)) {}

  void InitializeSema(Sema &S) override { TheSema = &S; }

  void HandleTranslationUnit(ASTContext &) override {
    if (TheSema)
      F(PP, *TheSema);
  }

private:
  Preprocessor &PP;
  Fn F;
  Sema *TheSema = nullptr;
};

class ProbeAction : public ASTFrontendAction {
public:
  explicit ProbeAction(ProbeConsumer::Fn F) : F(std::move(F)) {}
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef) override {
    return std::make_unique<ProbeConsumer>(CI.getPreprocessor(), F);
  }

private:
  ProbeConsumer::Fn F;
};

/// Compiles \p Code as C and runs \p F with its Preprocessor and Sema.
void withCompile(StringRef Code, ProbeConsumer::Fn F) {
  EXPECT_TRUE(tooling::runToolOnCodeWithArgs(
      std::make_unique<ProbeAction>(std::move(F)), Code, {"-std=c17"},
      "main.c"));
}

class EndToEndTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-e2e", Dir));
    TestDir = std::string(Dir);
    SocketPath = TestDir + "/sock";
    RegionPath = TestDir + "/region";

    auto D = CacheDaemon::create(RegionPath, 4u << 20);
    ASSERT_TRUE(bool(D)) << llvm::toString(D.takeError());
    Daemon = std::move(*D);

    Server = std::thread([this] {
      if (llvm::Error E = Daemon->run(SocketPath))
        llvm::consumeError(std::move(E));
    });

    // Wait for the socket file to appear rather than sleeping a fixed time.
    for (int I = 0; I < 500 && !llvm::sys::fs::exists(SocketPath); ++I)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(llvm::sys::fs::exists(SocketPath)) << "daemon never bound";
  }

  void TearDown() override {
    Daemon->stop();
    if (Server.joinable())
      Server.join();
    llvm::sys::fs::remove_directories(TestDir);
  }

  std::unique_ptr<CacheClient> client() {
    auto C = CacheClient::connect(SocketPath, RegionPath);
    EXPECT_TRUE(bool(C)) << llvm::toString(C.takeError());
    return C ? std::move(*C) : nullptr;
  }

  std::string TestDir, SocketPath, RegionPath;
  std::unique_ptr<CacheDaemon> Daemon;
  std::thread Server;
};

TEST_F(EndToEndTest, PushedEntryComesBackThroughTheSocket) {
  // The plainest possible chain: one process stores an entry, another asks
  // for it by name and gets the exact bytes back out of shared memory.
  auto Writer = client();
  ASSERT_TRUE(Writer);

  PushRequest P;
  P.Filename = "/x/h.h";
  P.Data = "STORED-PARSED-RESULT";
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &, Sema &S) {
    P.Deps.Entry = capturePragmaState(S);
    P.Deps.Exit = P.Deps.Entry;
  });
  ASSERT_TRUE(Writer->push(P));

  auto Reader = client();
  ASSERT_TRUE(Reader);
  std::string Got;
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &S) {
    Got = Reader->lookup("/x/h.h", PP, &S).str();
  });
  EXPECT_EQ(Got, "STORED-PARSED-RESULT");
}

TEST_F(EndToEndTest, ConditionalMismatchRejectsTheEntry) {
  // The entry was built with FEATURE undefined; this compile defines it, so
  // the stored parse describes a different header body and must not be used.
  auto Writer = client();
  ASSERT_TRUE(Writer);

  PushRequest P;
  P.Filename = "/x/h.h";
  P.Data = "BUILT-WITHOUT-FEATURE";
  P.Deps.Conditional.push_back({"FEATURE", false});
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &, Sema &S) {
    P.Deps.Entry = P.Deps.Exit = capturePragmaState(S);
  });
  ASSERT_TRUE(Writer->push(P));

  auto Reader = client();
  ASSERT_TRUE(Reader);

  bool HitWithout = false, HitWith = false;
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &S) {
    HitWithout = !Reader->lookup("/x/h.h", PP, &S).empty();
  });
  withCompile("#define FEATURE 1\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                HitWith = !Reader->lookup("/x/h.h", PP, &S).empty();
              });

  EXPECT_TRUE(HitWithout) << "the compile it was built for must hit";
  EXPECT_FALSE(HitWith) << "a compile that defines FEATURE must not hit";
}

TEST_F(EndToEndTest, SyntacticMismatchRejectsTheEntry) {
  // "char buf[BUFSZ];" compiled with BUFSZ as 1024 cannot be reused where
  // BUFSZ is 512 -- the array bound is baked into the stored declarations.
  auto Writer = client();
  ASSERT_TRUE(Writer);

  // Capture the exact token spelling the tracker would have recorded, by
  // asking the same function the tracker uses.
  std::string Seq1024;
  withCompile("#define BUFSZ 1024\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &) {
                IdentifierInfo &II = PP.getIdentifierTable().get("BUFSZ");
                const MacroInfo *MI = PP.getMacroDefinition(&II).getMacroInfo();
                ASSERT_NE(MI, nullptr);
                Seq1024 = serializeMacroTokens(*MI);
              });

  PushRequest P;
  P.Filename = "/x/h.h";
  P.Data = "BUILT-WITH-1024";
  P.Deps.Syntactic.push_back(
      {"BUFSZ", SyntacticDependency::State::ExpandsTo, Seq1024});
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &, Sema &S) {
    P.Deps.Entry = P.Deps.Exit = capturePragmaState(S);
  });
  ASSERT_TRUE(Writer->push(P));

  auto Reader = client();
  ASSERT_TRUE(Reader);
  bool Same = false, Different = false, Absent = false;
  withCompile("#define BUFSZ 1024\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                Same = !Reader->lookup("/x/h.h", PP, &S).empty();
              });
  withCompile("#define BUFSZ 512\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                Different = !Reader->lookup("/x/h.h", PP, &S).empty();
              });
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &S) {
    Absent = !Reader->lookup("/x/h.h", PP, &S).empty();
  });

  EXPECT_TRUE(Same);
  EXPECT_FALSE(Different) << "a different expansion must not hit";
  EXPECT_FALSE(Absent) << "an undefined macro must not hit either";
}

TEST_F(EndToEndTest, SymbolMismatchRejectsTheEntry) {
  // The case a macro table cannot express: "T *p;" parsed as a declaration
  // only because T named a type, and it must still name the *same* type.
  auto Writer = client();
  ASSERT_TRUE(Writer);

  PushRequest P;
  P.Filename = "/x/h.h";
  P.Data = "BUILT-WITH-T-AS-INT";
  P.Deps.Symbol.push_back({"T", SymbolClassification::Type, "int"});
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &, Sema &S) {
    P.Deps.Entry = P.Deps.Exit = capturePragmaState(S);
  });
  ASSERT_TRUE(Writer->push(P));

  auto Reader = client();
  ASSERT_TRUE(Reader);
  bool AsInt = false, AsLong = false, NotAType = false, Missing = false;
  withCompile("typedef int T;\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                AsInt = !Reader->lookup("/x/h.h", PP, &S).empty();
              });
  withCompile("typedef long T;\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                AsLong = !Reader->lookup("/x/h.h", PP, &S).empty();
              });
  withCompile("int T;\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                NotAType = !Reader->lookup("/x/h.h", PP, &S).empty();
              });
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &S) {
    Missing = !Reader->lookup("/x/h.h", PP, &S).empty();
  });

  EXPECT_TRUE(AsInt);
  EXPECT_FALSE(AsLong) << "still a type, but a different one -- must not hit";
  EXPECT_FALSE(NotAType) << "T as a variable parses the header differently";
  EXPECT_FALSE(Missing) << "no T at all parses the header differently";
}

TEST_F(EndToEndTest, PragmaStateMismatchRejectsTheEntry) {
  // Pragma state at the #include is an input no macro records. An entry
  // built under one pack setting must not serve a compile under another.
  auto Writer = client();
  ASSERT_TRUE(Writer);

  PushRequest P;
  P.Filename = "/x/h.h";
  P.Data = "BUILT-UNDER-PACK-1";
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &, Sema &S) {
    // Fabricate an entry snapshot that differs from any default state.
    P.Deps.Entry = capturePragmaState(S);
    P.Deps.Entry.AlignPackRaw ^= 0x10;
    P.Deps.Exit = P.Deps.Entry;
  });
  ASSERT_TRUE(Writer->push(P));

  auto Reader = client();
  ASSERT_TRUE(Reader);
  bool Hit = false;
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &S) {
    Hit = !Reader->lookup("/x/h.h", PP, &S).empty();
  });
  EXPECT_FALSE(Hit) << "different pragma state must not hit";
}

TEST_F(EndToEndTest, NoSemaRejectsEverything) {
  // A check that cannot run must reject. Passing a candidate because there
  // was nothing to verify it against is the exact fail-open shape the design
  // is built to avoid.
  auto Writer = client();
  ASSERT_TRUE(Writer);
  PushRequest P;
  P.Filename = "/x/h.h";
  P.Data = "ANYTHING";
  ASSERT_TRUE(Writer->push(P));

  auto Reader = client();
  ASSERT_TRUE(Reader);
  bool Hit = false;
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &) {
    Hit = !Reader->lookup("/x/h.h", PP, /*Sema=*/nullptr).empty();
  });
  EXPECT_FALSE(Hit);
}

TEST_F(EndToEndTest, VariantsAreChosenBetweenNotJustReturned) {
  // Two entries for one header, differing only in their record. The daemon
  // hands back both; picking the right one is entirely the client's job.
  auto Writer = client();
  ASSERT_TRUE(Writer);

  PragmaSnapshot Default;
  withCompile("int main(void){return 0;}\n",
              [&](Preprocessor &, Sema &S) { Default = capturePragmaState(S); });

  PushRequest On;
  On.Filename = "/x/h.h";
  On.Data = "WITH-FEATURE";
  On.Deps.Conditional.push_back({"FEATURE", true});
  On.Deps.Entry = On.Deps.Exit = Default;
  ASSERT_TRUE(Writer->push(On));

  PushRequest Off;
  Off.Filename = "/x/h.h";
  Off.Data = "WITHOUT-FEATURE";
  Off.Deps.Conditional.push_back({"FEATURE", false});
  Off.Deps.Entry = Off.Deps.Exit = Default;
  ASSERT_TRUE(Writer->push(Off));

  auto Reader = client();
  ASSERT_TRUE(Reader);
  std::string GotOn, GotOff;
  withCompile("#define FEATURE 1\nint main(void){return 0;}\n",
              [&](Preprocessor &PP, Sema &S) {
                GotOn = Reader->lookup("/x/h.h", PP, &S).str();
              });
  withCompile("int main(void){return 0;}\n", [&](Preprocessor &PP, Sema &S) {
    GotOff = Reader->lookup("/x/h.h", PP, &S).str();
  });

  EXPECT_EQ(GotOn, "WITH-FEATURE");
  EXPECT_EQ(GotOff, "WITHOUT-FEATURE");
}

} // namespace
