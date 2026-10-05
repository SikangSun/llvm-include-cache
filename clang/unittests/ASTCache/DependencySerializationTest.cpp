//===- unittests/ASTCache/DependencySerializationTest.cpp ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The dependency set is what decides whether a cached AST may be spliced into
// a live compile, so a decode that quietly drops or mangles part of it turns
// into a wrong hit rather than a diagnostic. These tests pin the round trip
// and, just as importantly, that a damaged buffer decodes to nothing at all
// rather than to a smaller set.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/DependencySerialization.h"
#include "clang/ASTCache/DeclFingerprint.h"
#include "gtest/gtest.h"
#include <string>

using namespace clang;
using namespace clang::ast_cache;

namespace {

std::string encode(const HeaderDependencySet &D) {
  llvm::SmallVector<char, 128> Out;
  encodeDependencySet(D, Out);
  return std::string(Out.data(), Out.size());
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

TEST(DependencySerializationTest, UnknownSyntacticStateIsRejected) {
  // A state byte this build does not know must not decode as one it does:
  // read as NotAMacro, a record meant to require a macro would require the
  // opposite.
  HeaderDependencySet D;
  D.Syntactic.push_back({"M", SyntacticDependency::State::NotAMacro, ""});
  std::string Data = encode(D);
  // version, conditional count, syntactic count, name length, "M", state.
  const size_t StateAt = 4 + 4 + 4 + 4 + 1;
  ASSERT_GT(Data.size(), StateAt);
  ASSERT_EQ(Data[StateAt], 0);
  Data[StateAt] = 7;
  EXPECT_FALSE(decodeDependencySet(Data).has_value());
}

TEST(DependencySerializationTest, NotAMacroNamesMoveIntoTheList) {
  HeaderDependencySet D = makeFullSet();
  D.Syntactic.push_back({"long", SyntacticDependency::State::NotAMacro, ""});
  std::string List = extractNotAMacroNames(D);

  // Only the names left; everything else stays in the record.
  ASSERT_EQ(D.Syntactic.size(), 1u);
  EXPECT_EQ(D.Syntactic[0].Name, "BUFSZ");
  EXPECT_EQ(D.Conditional.size(), 2u);

  auto Names = splitNameList(List);
  ASSERT_TRUE(Names.has_value());
  ASSERT_EQ(Names->size(), 2u);
  EXPECT_EQ((*Names)[0], "malloc");
  EXPECT_EQ((*Names)[1], "long");
  EXPECT_TRUE(splitNameList("").has_value()) << "no names is a valid list";
  EXPECT_TRUE(splitNameList("")->empty());
}

TEST(DependencySerializationTest, DamagedNameListIsRejected) {
  // Same rule as the record: a list that lost its tail must not read as a
  // shorter list, which would check fewer names than the record requires.
  // Lengths are explicit: a literal's own terminating NUL would otherwise
  // complete the list.
  EXPECT_FALSE(splitNameList(llvm::StringRef("malloc\0lon", 10)).has_value());
  EXPECT_FALSE(splitNameList(llvm::StringRef("malloc\0\0", 8)).has_value());
}

TEST(DependencySerializationTest, MacroEffectsMoveIntoTheirList) {
  HeaderDependencySet D = makeFullSet();
  std::string List = extractMacroEffects(D);
  EXPECT_TRUE(D.MacroEffects.empty());
  EXPECT_EQ(List, std::string("+GUARD_H\0-LIMIT\0", 16));

  auto Effects = decodeMacroEffects(List);
  ASSERT_TRUE(Effects.has_value());
  ASSERT_EQ(Effects->size(), 2u);
  EXPECT_EQ((*Effects)[0].Name, "GUARD_H");
  EXPECT_TRUE((*Effects)[0].Defined);
  EXPECT_EQ((*Effects)[1].Name, "LIMIT");
  EXPECT_FALSE((*Effects)[1].Defined);
  EXPECT_TRUE(decodeMacroEffects("").has_value()) << "no effects is valid";
}

TEST(DependencySerializationTest, DamagedEffectListIsRejected) {
  // A dropped effect leaves a macro as the header would not have: an #undef
  // that never happens is a different program.
  EXPECT_FALSE(decodeMacroEffects(llvm::StringRef("+A\0-B", 5)).has_value());
  EXPECT_FALSE(decodeMacroEffects(llvm::StringRef("+\0", 2)).has_value());
  EXPECT_FALSE(decodeMacroEffects(llvm::StringRef("A\0", 2)).has_value());
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
  llvm::SmallVector<char, 16> Out;
  encodeDependencySet(HeaderDependencySet(), Out);
  // Overwrite the first count (immediately after the version word) with a
  // value far larger than the buffer could hold.
  ASSERT_GE(Out.size(), 8u);
  for (int I = 0; I != 4; ++I)
    Out[4 + I] = static_cast<char>(0xFF);
  EXPECT_FALSE(
      decodeDependencySet(llvm::StringRef(Out.data(), Out.size())).has_value());
}

} // namespace
