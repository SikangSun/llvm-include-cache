//===- unittests/ASTCache/DependencySerializationTest.cpp ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A copy's metadata is what decides whether the copy may be spliced into a
// live compile, so a read that quietly drops or mangles part of it turns into
// a wrong hit rather than a diagnostic. These tests pin the round trip, that
// the view reads the stored bytes in place, and, just as importantly, that
// damaged bytes are refused whole rather than read as smaller metadata.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/DeclFingerprint.h"
#include "gtest/gtest.h"
#include <string>

using namespace clang;
using namespace clang::ast_cache;

namespace {

std::string encode(const HeaderDependencySet &D,
                   llvm::StringRef ModuleName = "__hcp_0123") {
  llvm::SmallVector<char, 128> Out;
  encodeMetadata(D, ModuleName, Out);
  return std::string(Out.data(), Out.size());
}

std::optional<HeaderDependencySet> decodeDependencySet(llvm::StringRef Data) {
  return decodeMetadata(Data);
}

HeaderDependencySet makeFullSet() {
  HeaderDependencySet D;
  D.Conditional.push_back({"HAVE_ATOMICS", true});
  D.Conditional.push_back({"NDEBUG", false});
  // Embedded NULs are the token separator the tracker writes, so this is the
  // ordinary case, not an edge case.
  D.Syntactic.push_back({"BUFSZ", SyntacticDependency::State::ExpandsTo,
                         std::string(1, '\0') + "1024" + '\0'});
  D.Syntactic.push_back({"malloc", SyntacticDependency::State::NotAMacro, ""});
  D.Symbol.push_back({"T", SymbolClassification::Type, "long"});
  D.Symbol.push_back({"count", SymbolClassification::NonType, ""});
  D.Inclusion.push_back({"/usr/include/stdio.h",
                         InclusionDependency::Reason::PragmaOnce});
  D.MacroEffects.push_back({"GUARD_H", true});
  D.MacroEffects.push_back({"LIMIT", false});
  D.Includes.push_back({IncludeDependency::Kind::Include, false, "sub/x.h", -1,
                        "/src/sub/x.h", "./sub/x.h"});
  D.Includes.push_back(
      {IncludeDependency::Kind::HasIncludeNext, true, "cfg.h", 0, ""});
  D.FileNames.push_back({FileNameDependency::Macro::File, -1, "./ae_epoll.c"});
  D.FileNames.push_back({FileNameDependency::Macro::FileName, 0, "x.h"});
  D.Declarations.push_back({DeclarationDependency::Space::Ordinary, "uint32_t",
                            "typedef unsigned int"});
  D.Declarations.push_back(
      {DeclarationDependency::Space::Tag, "fd_set", "struct defined 1234"});
  D.HeaderName = "./anet.h";
  D.Files.push_back({"/src/redis/src/anet.h", 2491, 1759600000});
  D.Files.push_back({"/usr/include/stdio.h", 0, -1});
  D.Entry = {7, 0x1234};
  D.Exit = {8, 0x5678};
  D.WarningState = 0xabcdef;
  D.MacroStacks.push_back({"X", {std::string("1"), std::nullopt}, 2});
  SavedDiagnostic Diag;
  Diag.Level = 3;
  Diag.ID = 1234;
  Diag.Loc = 99;
  Diag.Message = "unused variable 'x'";
  Diag.Ranges.push_back({10, 20, true});
  Diag.FixIts.push_back({{30, 40, false}, "y", true});
  D.Diagnostics.push_back(Diag);
  D.WarningPragmas.push_back({2, "-Wsign-compare"});
  return D;
}

void expectEqual(const HeaderDependencySet &A, const HeaderDependencySet &B) {
  ASSERT_EQ(A.Conditional.size(), B.Conditional.size());
  for (size_t I = 0; I != A.Conditional.size(); ++I) {
    EXPECT_EQ(A.Conditional[I].MacroName, B.Conditional[I].MacroName);
    EXPECT_EQ(A.Conditional[I].WasDefined, B.Conditional[I].WasDefined);
  }
  ASSERT_EQ(A.Syntactic.size(), B.Syntactic.size());
  for (size_t I = 0; I != A.Syntactic.size(); ++I) {
    EXPECT_EQ(A.Syntactic[I].Name, B.Syntactic[I].Name);
    EXPECT_EQ(A.Syntactic[I].Required, B.Syntactic[I].Required);
    EXPECT_EQ(A.Syntactic[I].TokenSequence, B.Syntactic[I].TokenSequence);
  }
  ASSERT_EQ(A.Symbol.size(), B.Symbol.size());
  for (size_t I = 0; I != A.Symbol.size(); ++I) {
    EXPECT_EQ(A.Symbol[I].Name, B.Symbol[I].Name);
    EXPECT_EQ(A.Symbol[I].Classification, B.Symbol[I].Classification);
    EXPECT_EQ(A.Symbol[I].ResolvedTypeName, B.Symbol[I].ResolvedTypeName);
  }
  ASSERT_EQ(A.Inclusion.size(), B.Inclusion.size());
  for (size_t I = 0; I != A.Inclusion.size(); ++I) {
    EXPECT_EQ(A.Inclusion[I].HeaderPath, B.Inclusion[I].HeaderPath);
    EXPECT_EQ(A.Inclusion[I].Why, B.Inclusion[I].Why);
  }
  ASSERT_EQ(A.MacroEffects.size(), B.MacroEffects.size());
  for (size_t I = 0; I != A.MacroEffects.size(); ++I) {
    EXPECT_EQ(A.MacroEffects[I].Name, B.MacroEffects[I].Name);
    EXPECT_EQ(A.MacroEffects[I].Defined, B.MacroEffects[I].Defined);
  }
  EXPECT_EQ(A.Includes, B.Includes);
  EXPECT_EQ(A.FileNames, B.FileNames);
  EXPECT_EQ(A.Declarations, B.Declarations);
  EXPECT_EQ(A.HeaderName, B.HeaderName);
  EXPECT_EQ(A.Files, B.Files);
  EXPECT_EQ(A.Entry, B.Entry);
  EXPECT_EQ(A.Exit, B.Exit);
  EXPECT_EQ(A.WarningState, B.WarningState);
  ASSERT_EQ(A.MacroStacks.size(), B.MacroStacks.size());
  for (size_t I = 0; I != A.MacroStacks.size(); ++I) {
    EXPECT_EQ(A.MacroStacks[I].Name, B.MacroStacks[I].Name);
    EXPECT_EQ(A.MacroStacks[I].Popped, B.MacroStacks[I].Popped);
    EXPECT_EQ(A.MacroStacks[I].Pushes, B.MacroStacks[I].Pushes);
  }
  ASSERT_EQ(A.Diagnostics.size(), B.Diagnostics.size());
  for (size_t I = 0; I != A.Diagnostics.size(); ++I) {
    const SavedDiagnostic &X = A.Diagnostics[I], &Y = B.Diagnostics[I];
    EXPECT_EQ(X.Level, Y.Level);
    EXPECT_EQ(X.ID, Y.ID);
    EXPECT_EQ(X.Loc, Y.Loc);
    EXPECT_EQ(X.Message, Y.Message);
    ASSERT_EQ(X.Ranges.size(), Y.Ranges.size());
    for (size_t J = 0; J != X.Ranges.size(); ++J) {
      EXPECT_EQ(X.Ranges[J].Begin, Y.Ranges[J].Begin);
      EXPECT_EQ(X.Ranges[J].End, Y.Ranges[J].End);
      EXPECT_EQ(X.Ranges[J].TokenRange, Y.Ranges[J].TokenRange);
    }
    ASSERT_EQ(X.FixIts.size(), Y.FixIts.size());
    for (size_t J = 0; J != X.FixIts.size(); ++J) {
      EXPECT_EQ(X.FixIts[J].Remove.Begin, Y.FixIts[J].Remove.Begin);
      EXPECT_EQ(X.FixIts[J].Insert, Y.FixIts[J].Insert);
      EXPECT_EQ(X.FixIts[J].BeforePreviousInsertions,
                Y.FixIts[J].BeforePreviousInsertions);
    }
  }
  EXPECT_EQ(A.WarningPragmas, B.WarningPragmas);
}

TEST(DependencySerializationTest, EmptySetRoundTrips) {
  // An empty set is a real answer -- a header that depends on nothing -- and
  // must be distinguishable from a failed decode.
  HeaderDependencySet Empty;
  auto Decoded = decodeDependencySet(encode(Empty));
  ASSERT_TRUE(Decoded.has_value());
  expectEqual(Empty, *Decoded);
}

TEST(DependencySerializationTest, FullSetRoundTrips) {
  HeaderDependencySet D = makeFullSet();
  auto Decoded = decodeDependencySet(encode(D));
  ASSERT_TRUE(Decoded.has_value());
  expectEqual(D, *Decoded);
}

TEST(DependencySerializationTest, EmbeddedNulsSurvive) {
  // TokenSequence is length-prefixed precisely because it is not NUL-free; a
  // NUL-terminated encoding would silently truncate every ExpandsTo
  // dependency to its first token, which still decodes and still compares
  // equal to a different macro.
  HeaderDependencySet D;
  D.Syntactic.push_back(
      {"M", SyntacticDependency::State::ExpandsTo, std::string("\0a\0b\0", 5)});
  auto Decoded = decodeDependencySet(encode(D));
  ASSERT_TRUE(Decoded.has_value());
  ASSERT_EQ(Decoded->Syntactic.size(), 1u);
  EXPECT_EQ(Decoded->Syntactic[0].TokenSequence, std::string("\0a\0b\0", 5));
}

TEST(DependencySerializationTest, EachSyntacticDependencyKeepsItsRequirement) {
  // The two requirements are stored in sections of their own. Swapped, a
  // name required to be plain text would be required to be a macro.
  HeaderDependencySet D;
  D.Syntactic.push_back({"M", SyntacticDependency::State::ExpandsTo, "1"});
  D.Syntactic.push_back({"N", SyntacticDependency::State::NotAMacro, ""});
  auto Decoded = decodeDependencySet(encode(D));
  ASSERT_TRUE(Decoded.has_value());
  ASSERT_EQ(Decoded->Syntactic.size(), 2u);
  EXPECT_EQ(Decoded->Syntactic[0].Name, "M");
  EXPECT_EQ(Decoded->Syntactic[0].Required,
            SyntacticDependency::State::ExpandsTo);
  EXPECT_EQ(Decoded->Syntactic[1].Name, "N");
  EXPECT_EQ(Decoded->Syntactic[1].Required,
            SyntacticDependency::State::NotAMacro);
}

TEST(DependencySerializationTest, TheViewReadsTheBytesInPlace) {
  // What every compile reads at a lookup: nothing decoded, each string in
  // the stored bytes themselves.
  HeaderDependencySet D = makeFullSet();
  D.Syntactic.push_back({"long", SyntacticDependency::State::NotAMacro, ""});
  std::string Data = encode(D, "__hcp_feed");
  std::optional<MetadataView> V = MetadataView::open(Data);
  ASSERT_TRUE(V.has_value());
  ASSERT_TRUE(V->validate());
  auto Inside = [&](llvm::StringRef S) {
    return S.data() >= Data.data() && S.end() <= Data.data() + Data.size();
  };
  EXPECT_EQ(V->moduleName(), "__hcp_feed");
  EXPECT_TRUE(Inside(V->moduleName()));
  EXPECT_EQ(V->headerName(), "./anet.h");

  std::vector<llvm::StringRef> Names;
  for (const NameView &N : V->notAMacro()) {
    EXPECT_TRUE(Inside(N.Name));
    Names.push_back(N.Name);
  }
  EXPECT_EQ(Names, (std::vector<llvm::StringRef>{"malloc", "long"}));
  ASSERT_EQ(V->expandsTo().size(), 1u);
  EXPECT_EQ(V->expandsTo().begin()->Name, "BUFSZ");

  std::vector<std::pair<llvm::StringRef, bool>> Effects;
  for (const MacroEffectView &E : V->macroEffects())
    Effects.push_back({E.Name, E.Defined});
  EXPECT_EQ(Effects, (std::vector<std::pair<llvm::StringRef, bool>>{
                         {"GUARD_H", true}, {"LIMIT", false}}));
  EXPECT_EQ(V->entry().AlignPackRaw, 7u);
  EXPECT_EQ(V->warningState(), 0xabcdefu);
}

TEST(DependencySerializationTest, DamagedNameListIsRejected) {
  // The ask-list's form: a list that lost its tail must not read as a
  // shorter list. Lengths are explicit: a literal's own terminating NUL
  // would otherwise complete the list.
  EXPECT_TRUE(splitNameList("").has_value()) << "no names is a valid list";
  EXPECT_TRUE(splitNameList("")->empty());
  EXPECT_FALSE(splitNameList(llvm::StringRef("malloc\0lon", 10)).has_value());
  EXPECT_FALSE(splitNameList(llvm::StringRef("malloc\0\0", 8)).has_value());
}

TEST(DependencySerializationTest, AnEmptyNameIsRejected) {
  // Looked up, an empty name is never a macro, so a requirement on it
  // passes whatever the compile defines; replayed, it changes nothing.
  HeaderDependencySet D;
  D.Syntactic.push_back({"", SyntacticDependency::State::NotAMacro, ""});
  EXPECT_FALSE(decodeDependencySet(encode(D)).has_value());
  D = HeaderDependencySet();
  D.Syntactic.push_back({"", SyntacticDependency::State::ExpandsTo, "1"});
  EXPECT_FALSE(decodeDependencySet(encode(D)).has_value());
  D = HeaderDependencySet();
  D.MacroEffects.push_back({"", true});
  EXPECT_FALSE(decodeDependencySet(encode(D)).has_value());
}

TEST(DependencySerializationTest, TruncationIsRejectedNotPartiallyDecoded) {
  // The failure that matters: a set that decodes to a *prefix* of itself has
  // fewer constraints than the compile it came from, so it would validate
  // against compiles it must not.
  std::string Full = encode(makeFullSet());
  for (size_t Len = 0; Len < Full.size(); ++Len)
    EXPECT_FALSE(decodeDependencySet(llvm::StringRef(Full).substr(0, Len))
                     .has_value())
        << "prefix of length " << Len << " decoded";
}

TEST(DependencySerializationTest, VersionMismatchIsRejected) {
  std::string Data = encode(makeFullSet());
  ASSERT_GE(Data.size(), 4u);
  Data[0] = static_cast<char>(DependencyEncodingVersion + 1);
  EXPECT_FALSE(decodeDependencySet(Data).has_value());
}

TEST(DependencySerializationTest, TrailingBytesAreRejected) {
  // Version matched but the shape did not, so the decode is not trustworthy
  // even though every field read succeeded.
  std::string Data = encode(makeFullSet());
  Data += "junk";
  EXPECT_FALSE(decodeDependencySet(Data).has_value());
}

TEST(DependencySerializationTest, OutOfRangeClassificationIsRejected) {
  std::string Data = encode(makeFullSet());
  // Find the symbol classification byte by rebuilding the same set with a
  // different classification and diffing -- avoids hardcoding an offset.
  HeaderDependencySet Other = makeFullSet();
  Other.Symbol[0].Classification = SymbolClassification::NonType;
  std::string Alt = encode(Other);
  ASSERT_EQ(Data.size(), Alt.size());
  size_t Diff = std::string::npos;
  for (size_t I = 0; I != Data.size(); ++I)
    if (Data[I] != Alt[I]) {
      ASSERT_EQ(Diff, std::string::npos) << "expected exactly one differing byte";
      Diff = I;
    }
  ASSERT_NE(Diff, std::string::npos);
  Data[Diff] = static_cast<char>(200);
  EXPECT_FALSE(decodeDependencySet(Data).has_value());
}

TEST(DependencySerializationTest, OutOfRangeFileNameMacroIsRejected) {
  // The same diffing trick, on which macro a __FILE__ dependency names.
  std::string Data = encode(makeFullSet());
  HeaderDependencySet Other = makeFullSet();
  Other.FileNames[0].Which = FileNameDependency::Macro::FileName;
  std::string Alt = encode(Other);
  ASSERT_EQ(Data.size(), Alt.size());
  size_t Diff = std::string::npos;
  for (size_t I = 0; I != Data.size(); ++I)
    if (Data[I] != Alt[I]) {
      ASSERT_EQ(Diff, std::string::npos)
          << "expected exactly one differing byte";
      Diff = I;
    }
  ASSERT_NE(Diff, std::string::npos);
  Data[Diff] = static_cast<char>(200);
  EXPECT_FALSE(decodeDependencySet(Data).has_value());
}

TEST(DependencySerializationTest, AnIndexThatPointsForwardIsRejected) {
  // An entry's includer and a __FILE__'s file are earlier entries; anything
  // else would send a hit's lookups to the wrong file.
  HeaderDependencySet D = makeFullSet();
  D.Includes[0].Includer = 0;
  EXPECT_FALSE(decodeDependencySet(encode(D)).has_value());
  D = makeFullSet();
  D.FileNames[1].File = 2;
  EXPECT_FALSE(decodeDependencySet(encode(D)).has_value());
}

TEST(DependencySerializationTest, OutOfRangeNameSpaceIsRejected) {
  std::string Data = encode(makeFullSet());
  HeaderDependencySet Other = makeFullSet();
  Other.Declarations[0].In = DeclarationDependency::Space::Tag;
  std::string Alt = encode(Other);
  ASSERT_EQ(Data.size(), Alt.size());
  size_t Diff = std::string::npos;
  for (size_t I = 0; I != Data.size(); ++I)
    if (Data[I] != Alt[I]) {
      ASSERT_EQ(Diff, std::string::npos)
          << "expected exactly one differing byte";
      Diff = I;
    }
  ASSERT_NE(Diff, std::string::npos);
  Data[Diff] = static_cast<char>(200);
  EXPECT_FALSE(decodeDependencySet(Data).has_value());
}

TEST(DeclFingerprintTest, ADeclarationMergesWithADefinitionNotTheOtherWay) {
  // Equal fingerprints always satisfy.
  EXPECT_TRUE(fingerprintSatisfies("struct defined 12", "struct defined 12"));
  // A part carrying only a declaration merges with a live definition of the
  // same kind and type.
  EXPECT_TRUE(fingerprintSatisfies("struct declared", "struct defined 12"));
  EXPECT_TRUE(fingerprintSatisfies("function int (int) asm(f) declared",
                                   "function int (int) asm(f) defined 7"));
  // A part carrying a definition would complete or contradict a live one.
  EXPECT_FALSE(fingerprintSatisfies("struct defined 12", "struct declared"));
  EXPECT_FALSE(fingerprintSatisfies("struct defined 12", "struct defined 13"));
  // Another type, another asm label, another kind.
  EXPECT_FALSE(fingerprintSatisfies("function int (int) declared",
                                    "function long (int) defined 7"));
  EXPECT_FALSE(fingerprintSatisfies("function int (int) asm(a) declared",
                                    "function int (int) asm(b) declared"));
  EXPECT_FALSE(fingerprintSatisfies("struct declared", "union defined 3"));
}

TEST(DependencySerializationTest, HugeCountDoesNotOverAllocate) {
  // A corrupt count must fail immediately rather than sizing a loop from it.
  std::string Out = encode(HeaderDependencySet());
  // Overwrite the first section's count, where the table after the version
  // word says it is, with a value far larger than the bytes could hold.
  ASSERT_GE(Out.size(), 8u);
  uint32_t At = llvm::support::endian::read32le(Out.data() + 4);
  ASSERT_LE(At + 4, Out.size());
  for (int I = 0; I != 4; ++I)
    Out[At + I] = static_cast<char>(0xFF);
  EXPECT_FALSE(decodeDependencySet(Out).has_value());
}

TEST(DependencySerializationTest, ASectionTableOutOfOrderIsRejected) {
  // Each section ends where the next starts; a table that says otherwise
  // would have two sections read the same bytes.
  std::string Out = encode(makeFullSet());
  llvm::support::endian::write32le(
      Out.data() + 4, llvm::support::endian::read32le(Out.data() + 8));
  EXPECT_FALSE(MetadataView::open(Out).has_value());
}

} // namespace
