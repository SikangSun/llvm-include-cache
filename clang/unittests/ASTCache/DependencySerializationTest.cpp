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
  D.Value.push_back({"BUFSZ", std::string("\0" "1024" "\0", 6)});
  D.Symbol.push_back({"T", SymbolClassification::Type, "long"});
  D.Symbol.push_back({"count", SymbolClassification::NonType, ""});
  D.Inclusion.push_back({"/usr/include/stdio.h",
                         InclusionDependency::Reason::PragmaOnce});
  return D;
}

void expectEqual(const HeaderDependencySet &A, const HeaderDependencySet &B) {
  ASSERT_EQ(A.Conditional.size(), B.Conditional.size());
  for (size_t I = 0; I != A.Conditional.size(); ++I) {
    EXPECT_EQ(A.Conditional[I].MacroName, B.Conditional[I].MacroName);
    EXPECT_EQ(A.Conditional[I].WasDefined, B.Conditional[I].WasDefined);
  }
  ASSERT_EQ(A.Value.size(), B.Value.size());
  for (size_t I = 0; I != A.Value.size(); ++I) {
    EXPECT_EQ(A.Value[I].MacroName, B.Value[I].MacroName);
    EXPECT_EQ(A.Value[I].TokenSequence, B.Value[I].TokenSequence);
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
  // NUL-terminated encoding would silently truncate every value dependency
  // to its first token, which still decodes and still compares equal to a
  // different macro.
  HeaderDependencySet D;
  D.Value.push_back({"M", std::string("\0a\0b\0", 5)});
  auto Decoded = decodeDependencySet(encode(D));
  ASSERT_TRUE(Decoded.has_value());
  ASSERT_EQ(Decoded->Value.size(), 1u);
  EXPECT_EQ(Decoded->Value[0].TokenSequence, std::string("\0a\0b\0", 5));
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
