//===- unittests/ASTCache/CacheProtocolTest.cpp --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The protocol and the daemon's message handling, tested without sockets.
// handleMessage is a pure function of (message, daemon state), so the whole
// lookup/push behaviour can be exercised directly -- what goes over a socket
// is only framing, covered separately.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/CacheClient.h"
#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "llvm/Support/FileSystem.h"
#include "gtest/gtest.h"
#include <chrono>
#include <string>
#include <thread>

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Strips the 4-byte length prefix an encoder produces, leaving the body a
/// decoder expects. receiveMessage does this on the wire.
llvm::StringRef body(const llvm::SmallVectorImpl<char> &Framed) {
  return llvm::StringRef(Framed.data() + 4, Framed.size() - 4);
}

HeaderDependencySet makeRecord(llvm::StringRef Macro, bool Defined) {
  HeaderDependencySet D;
  D.Conditional.push_back({Macro.str(), Defined});
  D.Syntactic.push_back({"BUFSZ", SyntacticDependency::State::ExpandsTo,
                         std::string(1, '\0') + "1024" + '\0'});
  D.Symbol.push_back({"T", SymbolClassification::Type, "long"});
  D.Entry.AlignPackRaw = 7;
  D.Exit.AlignPackRaw = 7;
  return D;
}

TEST(CacheProtocolTest, LengthPrefixMatchesBody) {
  // Framing is what lets one connection carry a run of lookups and then, much
  // later, a push. A prefix that disagreed with the body would desynchronise
  // the stream permanently.
  llvm::SmallVector<char, 0> Out;
  encodeLookupRequest({"/usr/include/stdio.h"}, Out);
  ASSERT_GE(Out.size(), 4u);
  uint32_t Len = llvm::support::endian::read32le(Out.data());
  EXPECT_EQ(Len, Out.size() - 4);
}

TEST(CacheProtocolTest, RequestsRoundTrip) {
  llvm::SmallVector<char, 0> Out;
  encodeLookupRequest({"h.h"}, Out);
  auto Req = decodeLookupRequest(body(Out));
  ASSERT_TRUE(Req.has_value());
  EXPECT_EQ(Req->Filename, "h.h");

  llvm::SmallVector<char, 0> Out2;
  PushRequest P;
  P.Filename = "h.h";
  P.ModuleName = "__hc_0123456789abcdef";
  P.Deps = makeRecord("FEATURE", true);
  P.Data = std::string("\0\1\2binary\0", 10);
  encodePushRequest(P, Out2);

  auto Got = decodePushRequest(body(Out2));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->Filename, "h.h");
  EXPECT_EQ(Got->Data, P.Data) << "payload must survive embedded NULs";
  std::string Name;
  auto Deps = decodeMetadata(Got->Metadata, &Name);
  ASSERT_TRUE(Deps.has_value());
  EXPECT_EQ(Name, "__hc_0123456789abcdef");
  ASSERT_EQ(Deps->Conditional.size(), 1u);
  EXPECT_EQ(Deps->Conditional[0].MacroName, "FEATURE");
  EXPECT_EQ(Deps->Syntactic[0].TokenSequence,
            P.Deps.Syntactic[0].TokenSequence);
  EXPECT_EQ(Deps->Entry.AlignPackRaw, 7u);
}

TEST(CacheProtocolTest, LookupResponseSaysWhereTheMetadataIs) {
  LookupResponse Resp;
  CacheEntryRef E;
  E.MetadataOffset = 64;
  E.MetadataLength = 36;
  E.Offset = 100;
  E.Length = 20;
  E.Searches.push_back({0x1234, 3});
  Resp.Entries.push_back(E);
  llvm::SmallVector<char, 0> Out;
  encodeLookupResponse(Resp, Out);
  auto Got = decodeLookupResponse(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Entries.size(), 1u);
  EXPECT_EQ(Got->Entries[0].MetadataOffset, 64u);
  EXPECT_EQ(Got->Entries[0].MetadataLength, 36u);
  EXPECT_EQ(Got->Entries[0].Offset, 100u);
  EXPECT_EQ(Got->Entries[0].Length, 20u);
  ASSERT_EQ(Got->Entries[0].Searches.size(), 1u);
  EXPECT_EQ(Got->Entries[0].Searches[0].Key, 0x1234u);
  EXPECT_EQ(Got->Entries[0].Searches[0].Outcome, 3u);
}

TEST(CacheProtocolTest, ASocketPathTooLongForTheAddressIsRefused) {
  // Cut short silently, it would name a different socket.
  EXPECT_TRUE(llvm::errorToBool(checkSocketPath(std::string(200, 'a'))));
  EXPECT_FALSE(llvm::errorToBool(checkSocketPath(DefaultSocketPath)));
  std::string Long = "/tmp/" + std::string(150, 'x');
  llvm::Error E = checkSocketPath(Long);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("at most"), std::string::npos);
  auto C = CacheClient::connect(Long, "/dev/null");
  ASSERT_FALSE(bool(C));
  EXPECT_NE(llvm::toString(C.takeError()).find("at most"), std::string::npos)
      << "the client must refuse before trying the shortened path";
}

TEST(CacheProtocolTest, TimingReportRoundTrips) {
  TimingReport Report;
  Report.Headers = {{"/src/server.h", 41250}, {"/usr/include/stdio.h", 612}};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Headers.size(), 2u);
  EXPECT_EQ(Got->Headers[0].Path, "/src/server.h");
  EXPECT_EQ(Got->Headers[0].Micros, 41250u);
  EXPECT_EQ(Got->Headers[1].Micros, 612u);
}

TEST(CacheProtocolTest, TimingReportCarriesHitCosts) {
  TimingReport Report;
  Report.Headers = {{"/src/server.h", 41250}};
  Report.Hits = {{"/usr/include/stdio.h", 530}, {"/src/rio.h", 2251}};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Headers.size(), 1u);
  ASSERT_EQ(Got->Hits.size(), 2u);
  EXPECT_EQ(Got->Hits[0].Path, "/usr/include/stdio.h");
  EXPECT_EQ(Got->Hits[1].Micros, 2251u);
}

TEST(CacheProtocolTest, HelloReplyRoundTrips) {
  llvm::SmallVector<char, 0> Out;
  encodeHelloReply({"/dev/shm/x.region", 0x0123456789abcdefull}, Out);
  auto Got = decodeHelloReply(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->RegionPath, "/dev/shm/x.region");
  EXPECT_EQ(Got->InstanceId, 0x0123456789abcdefull);

  // A reply naming no file tells the client nothing it can attach to.
  Out.clear();
  encodeHelloReply({"", 1}, Out);
  EXPECT_FALSE(decodeHelloReply(body(Out)).has_value());
}

TEST(CacheProtocolTest, WrongKindIsRejected) {
  llvm::SmallVector<char, 0> Out;
  encodeLookupRequest({"h.h"}, Out);
  EXPECT_FALSE(decodePushRequest(body(Out)).has_value());
}

TEST(CacheProtocolTest, TruncatedMessageIsRejected) {
  // Same rule as the record encoding: a message must decode fully or not at
  // all. A half-decoded lookup response would offer entries with incomplete
  // records, and an incomplete record matches more compiles than it should.
  llvm::SmallVector<char, 0> Out;
  PushRequest P;
  P.Filename = "h.h";
  P.Deps = makeRecord("FEATURE", true);
  P.Data = "payload";
  encodePushRequest(P, Out);
  llvm::StringRef Full = body(Out);
  for (size_t Len = 0; Len < Full.size(); ++Len)
    EXPECT_FALSE(decodePushRequest(Full.substr(0, Len)).has_value())
        << "prefix of length " << Len << " decoded";
}

//===----------------------------------------------------------------------===//
// Daemon behaviour
//===----------------------------------------------------------------------===//

class DaemonTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("astcache-daemon", Dir));
    TestDir = std::string(Dir);
    RegionPath = TestDir + "/region";
    auto D = CacheDaemon::create(RegionPath, 1u << 20);
    ASSERT_TRUE(bool(D)) << llvm::toString(D.takeError());
    Daemon = std::move(*D);
  }
  void TearDown() override { llvm::sys::fs::remove_directories(TestDir); }

  void report(std::vector<HeaderTiming> Headers,
              std::vector<HeaderTiming> Hits = {}) {
    llvm::SmallVector<char, 0> Msg;
    encodeTimingReport({std::move(Headers), std::move(Hits)}, Msg);
    auto Reply = Daemon->handleMessage(body(Msg));
    ASSERT_TRUE(Reply.has_value());
    EXPECT_EQ(peekMessageKind(body(*Reply)), MessageKind::TimingAck);
  }

  /// The ask-list as a client reads it from the region.
  std::vector<std::string> askList() {
    auto Attached = SharedRegion::attach(RegionPath);
    EXPECT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
    std::vector<std::string> Out;
    if (!Attached)
      return Out;
    auto Names = splitNameList(Attached->askList());
    EXPECT_TRUE(Names.has_value());
    if (Names)
      for (llvm::StringRef N : *Names)
        Out.push_back(N.str());
    return Out;
  }

  LookupResponse lookup(llvm::StringRef Name, size_t *ReplySize = nullptr) {
    llvm::SmallVector<char, 0> Msg;
    encodeLookupRequest({Name.str()}, Msg);
    auto Reply = Daemon->handleMessage(body(Msg));
    EXPECT_TRUE(Reply.has_value());
    if (ReplySize)
      *ReplySize = Reply->size();
    auto Resp = decodeLookupResponse(body(*Reply));
    EXPECT_TRUE(Resp.has_value());
    return Resp.value_or(LookupResponse{});
  }

  /// An entry's metadata, read from the region as a client reads it.
  HeaderDependencySet metadata(const CacheEntryRef &E) {
    auto Attached = SharedRegion::attach(RegionPath);
    EXPECT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
    if (!Attached)
      return {};
    auto Deps =
        decodeMetadata(Attached->read(E.MetadataOffset, E.MetadataLength));
    EXPECT_TRUE(Deps.has_value());
    return Deps.value_or(HeaderDependencySet());
  }

  void push(llvm::StringRef Name, const HeaderDependencySet &Deps,
            llvm::StringRef Data, llvm::StringRef ModuleName = "__hc_m") {
    llvm::SmallVector<char, 0> Msg;
    PushRequest P;
    P.Filename = Name.str();
    P.ModuleName = ModuleName.str();
    P.Deps = Deps;
    P.Data = Data.str();
    encodePushRequest(P, Msg);
    auto Reply = Daemon->handleMessage(body(Msg));
    ASSERT_TRUE(Reply.has_value());
    EXPECT_EQ(peekMessageKind(body(*Reply)), MessageKind::PushAck);
  }

  /// Replaces the daemon with one created with \p O.
  void useDaemon(const CacheDaemon::Options &O) {
    llvm::sys::fs::remove(RegionPath);
    auto D = CacheDaemon::create(RegionPath, 1u << 20, O);
    ASSERT_TRUE(bool(D)) << llvm::toString(D.takeError());
    Daemon = std::move(*D);
  }
  static CacheDaemon::Options dropCostly() {
    CacheDaemon::Options O;
    O.DropCostly = true;
    return O;
  }

  std::string TestDir, RegionPath;
  std::unique_ptr<CacheDaemon> Daemon;
};

/// CLANG_HEADER_CACHE_WAIT and _CLAIM: messages as they arrive on numbered
/// connections, the way run() hands them over.
class ConcurrencyTest : public DaemonTest {
protected:
  LookupResponse lookupFrom(uint64_t Conn, llvm::StringRef Name,
                            uint64_t Client, bool Wait,
                            std::vector<uint64_t> Used = {}) {
    LookupRequest Req{Name.str(), 0, {}, std::move(Used)};
    Req.ClientId = Client;
    Req.Wait = Wait;
    llvm::SmallVector<char, 0> Msg;
    encodeLookupRequest(Req, Msg);
    auto Reply = Daemon->handleMessage(body(Msg), Conn);
    EXPECT_TRUE(Reply.has_value());
    auto Resp = Reply ? decodeLookupResponse(body(*Reply)) : std::nullopt;
    EXPECT_TRUE(Resp.has_value());
    return Resp.value_or(LookupResponse{});
  }
  void notice(uint64_t Conn, llvm::StringRef Name, uint64_t Client,
              bool Ended = false) {
    llvm::SmallVector<char, 0> Msg;
    encodeWritingNotice({Name.str(), Client, Ended}, Msg);
    auto Reply = Daemon->handleMessage(body(Msg), Conn);
    ASSERT_TRUE(Reply.has_value());
    EXPECT_EQ(peekMessageKind(body(*Reply)), MessageKind::NoticeAck);
  }
  bool claim(uint64_t Conn, llvm::StringRef Name, llvm::StringRef Module,
             bool Release = false) {
    llvm::SmallVector<char, 0> Msg;
    encodeClaimRequest({Name.str(), Module.str(), Release}, Msg);
    auto Reply = Daemon->handleMessage(body(Msg), Conn);
    EXPECT_TRUE(Reply.has_value());
    return Reply && decodeClaimReply(body(*Reply)).value_or(false);
  }
  void pushFrom(uint64_t Conn, llvm::StringRef Name, llvm::StringRef Module,
                llvm::StringRef Data = "BYTES") {
    llvm::SmallVector<char, 0> Msg;
    PushRequest P;
    P.Filename = Name.str();
    P.ModuleName = Module.str();
    P.Deps = makeRecord("M", true);
    P.Data = Data.str();
    encodePushRequest(P, Msg);
    auto Reply = Daemon->handleMessage(body(Msg), Conn);
    ASSERT_TRUE(Reply.has_value());
  }
  /// A wait's time limit runs from the header's parse time: long here, so
  /// only the events a test sends end a wait.
  void timeHeader(llvm::StringRef Name) { report({{Name.str(), 2000000}}); }
};

TEST_F(ConcurrencyTest, AClaimIsGrantedOnceUntilGivenBack) {
  // A claim still held when its compile ends is for a copy the compile
  // wrote (one declined is given back), and stays: claims by the whole
  // dependency list are not the module names pushes mark.
  EXPECT_TRUE(claim(1, "h.h", "__hcp_a"));
  EXPECT_FALSE(claim(2, "h.h", "__hcp_a")) << "claimed by connection 1";
  EXPECT_TRUE(claim(2, "h.h", "__hcp_b")) << "another copy of the header";
  claim(1, "h.h", "__hcp_a", /*Release=*/true);
  EXPECT_TRUE(claim(3, "h.h", "__hcp_a")) << "given back";
  Daemon->connectionClosed(3);
  EXPECT_FALSE(claim(4, "h.h", "__hcp_a")) << "written by connection 3";
  pushFrom(5, "h.h", "__hcp_c");
  EXPECT_FALSE(claim(6, "h.h", "__hcp_c")) << "stored";
  EXPECT_EQ(Daemon->stats().ClaimsRefused, 3u);
}

TEST_F(ConcurrencyTest, AWaitingLookupIsServedByTheCopyBeingWritten) {
  timeHeader("h.h");
  notice(1, "h.h", /*Client=*/1);
  LookupResponse Got;
  std::thread Waiter([&] { Got = lookupFrom(2, "h.h", /*Client=*/2, true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pushFrom(1, "h.h", "__hcp_a");
  Waiter.join();
  EXPECT_TRUE(Got.Waited);
  EXPECT_EQ(Got.Entries.size(), 1u) << "the copy that arrived";
  EXPECT_EQ(Daemon->stats().WaitsServed, 1u);
}

TEST_F(ConcurrencyTest, AWaitEndsWhenTheWriterGivesUpOrCloses) {
  timeHeader("h.h");
  for (bool Close : {false, true}) {
    notice(1, "h.h", /*Client=*/1);
    LookupResponse Got;
    std::thread Waiter([&] { Got = lookupFrom(2, "h.h", 2, true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (Close)
      Daemon->connectionClosed(1);
    else
      notice(1, "h.h", 1, /*Ended=*/true);
    Waiter.join();
    EXPECT_TRUE(Got.Waited);
    EXPECT_TRUE(Got.Entries.empty());
  }
  EXPECT_EQ(Daemon->stats().WaitsTimedOut, 0u);
}

TEST_F(ConcurrencyTest, ACompileDoesNotWaitForItself) {
  timeHeader("h.h");
  notice(1, "h.h", /*Client=*/7);
  EXPECT_FALSE(lookupFrom(2, "h.h", /*Client=*/7, true).Waited);
  EXPECT_FALSE(lookupFrom(3, "h.h", /*Client=*/8, false).Waited)
      << "a lookup that does not ask to wait";
}

TEST_F(ConcurrencyTest, TheGateStopsWaitsWhoseCopiesDoNotServe) {
  // Three waits each end in a copy the waiting compile's next message does
  // not list as used: the fourth lookup is not held.
  timeHeader("h.h");
  for (uint64_t I = 0; I != 3; ++I) {
    notice(10 + I, "h.h", 100 + I);
    LookupResponse Got;
    std::thread Waiter([&] { Got = lookupFrom(20 + I, "h.h", 200 + I, true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pushFrom(10 + I, "h.h", "__hcp_" + std::to_string(I), "B" + std::to_string(I));
    Waiter.join();
    ASSERT_TRUE(Got.Waited);
    lookupFrom(20 + I, "other.h", 200 + I, false, /*Used=*/{});
  }
  notice(30, "h.h", 300);
  EXPECT_FALSE(lookupFrom(31, "h.h", 301, true).Waited);
  EXPECT_EQ(Daemon->stats().WaitsGated, 1u);
}

TEST_F(DaemonTest, UnknownHeaderIsACleanMiss) {
  // An empty result, not an error: nothing has been stored under that name.
  EXPECT_TRUE(lookup("never-seen.h").Entries.empty());
}

TEST_F(DaemonTest, PushThenLookupReturnsTheEntry) {
  push("h.h", makeRecord("FEATURE", true), "PARSED-RESULT-BYTES");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 1u);
  HeaderDependencySet Deps = metadata(R.Entries[0]);
  ASSERT_EQ(Deps.Conditional.size(), 1u);
  EXPECT_EQ(Deps.Conditional[0].MacroName, "FEATURE");
  EXPECT_EQ(R.Entries[0].Length, 19u);
}

TEST_F(DaemonTest, TheMetadataIsInTheRegionNotInTheReply) {
  // Several hundred names for a system header, thousands of macro effects
  // for one that pulls a system header in: all of it sits in the region,
  // where every compile reads it in place, and a reply says only where.
  HeaderDependencySet Small = makeRecord("FEATURE", true);
  HeaderDependencySet Large = Small;
  for (int I = 0; I != 1000; ++I) {
    Large.Syntactic.push_back({"name" + std::to_string(I),
                               SyntacticDependency::State::NotAMacro, ""});
    Large.MacroEffects.push_back({"M" + std::to_string(I), I % 2 == 0});
  }
  push("small.h", Small, "PARSED");
  push("large.h", Large, "PARSED");
  size_t SmallReply = 0, LargeReply = 0;
  LookupResponse S = lookup("small.h", &SmallReply);
  LookupResponse L = lookup("large.h", &LargeReply);
  ASSERT_EQ(S.Entries.size(), 1u);
  ASSERT_EQ(L.Entries.size(), 1u);
  EXPECT_EQ(SmallReply, LargeReply);
  EXPECT_GT(L.Entries[0].MetadataLength, 10000u);

  HeaderDependencySet Got = metadata(L.Entries[0]);
  ASSERT_EQ(Got.Syntactic.size(), 1001u);
  EXPECT_EQ(Got.Syntactic[1].Name, "name0");
  ASSERT_EQ(Got.MacroEffects.size(), 1000u);
  EXPECT_FALSE(Got.MacroEffects[1].Defined);

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  EXPECT_EQ(Attached->read(L.Entries[0].Offset, L.Entries[0].Length),
            "PARSED");
}

TEST_F(DaemonTest, MalformedMetadataIsRefused) {
  // The daemon checks metadata once, when it is pushed; every compile after
  // reads it as stored. An #include whose includer is not an earlier one
  // would send a hit's searches to the wrong file.
  HeaderDependencySet D = makeRecord("FEATURE", true);
  D.Includes.push_back(
      {IncludeDependency::Kind::Include, false, "x.h", 0, "/x.h", "./x.h"});
  llvm::SmallVector<char, 0> Msg;
  PushRequest P;
  P.Filename = "h.h";
  P.Deps = D;
  P.Data = "PARSED";
  encodePushRequest(P, Msg);
  EXPECT_FALSE(Daemon->handleMessage(body(Msg)).has_value());
  EXPECT_TRUE(lookup("h.h").Entries.empty());
}

TEST_F(DaemonTest, EveryVariantIsReturnedNotJustOne) {
  // The daemon matches on filename only. Deciding between variants needs the
  // client's live macro table and its Sema, neither of which the daemon has,
  // so it hands back all of them.
  push("h.h", makeRecord("FEATURE", true), "AAA");
  push("h.h", makeRecord("FEATURE", false), "BBBB");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 2u);
  EXPECT_NE(R.Entries[0].Offset, R.Entries[1].Offset);
  EXPECT_EQ(R.Entries[0].Length, 3u);
  EXPECT_EQ(R.Entries[1].Length, 4u);
}

TEST_F(DaemonTest, IdenticalPushIsDiscarded) {
  // Two clients raced and produced the same result. The loser already holds
  // its own copy, so the data is dropped and only acknowledged.
  push("h.h", makeRecord("FEATURE", true), "SAME");
  push("h.h", makeRecord("FEATURE", true), "SAME");
  EXPECT_EQ(lookup("h.h").Entries.size(), 1u);
  EXPECT_EQ(Daemon->stats().Duplicates, 1u);
}

TEST_F(DaemonTest, TheSameRecordBuiltWithOtherOptionsIsKept) {
  // Compiler options are not part of the record; they are in the module
  // name. Discarding this push would leave only a copy the module reader
  // refuses for every compile using the second set of options.
  push("h.h", makeRecord("FEATURE", true), "SHORT-ENUMS", "__hc_1111");
  push("h.h", makeRecord("FEATURE", true), "PLAIN", "__hc_2222");
  EXPECT_EQ(lookup("h.h").Entries.size(), 2u);
  EXPECT_EQ(Daemon->stats().Duplicates, 0u);
}

TEST_F(DaemonTest, ASlowHeaderGoesOnTheAskList) {
  // At or above the cost of one load, a saved copy is worth asking for.
  EXPECT_TRUE(askList().empty()) << "nothing is listed before any report";
  report({{"/src/slow.h", 5000},
          {"/src/fast.h", 40},
          {"/src/edge.h", DefaultAskCutoffMicros}});
  EXPECT_EQ(askList(),
            (std::vector<std::string>{"/src/slow.h", "/src/edge.h"}));
  EXPECT_EQ(Daemon->stats().HeadersTimed, 3u);
}

TEST_F(DaemonTest, AHeadersFirstTimeIsTheOneKept) {
  // One observation predicts the rest well, and keeping the first makes
  // membership final: a header does not flicker on and off the list.
  report({{"/src/fast.h", 40}});
  report({{"/src/fast.h", 90000}});
  EXPECT_TRUE(askList().empty());
}

TEST_F(DaemonTest, AHeaderWhoseHitsCostMoreThanItsParseIsDropped) {
  // A hit costs its query and replay, plus what the loaded PCM adds to the
  // rest of the compile. mid.h parses in 400 us; a 300 us hit plus 200 us
  // later costs more, so asking about mid.h only loses time.
  useDaemon(dropCostly());
  report({{"/src/slow.h", 5000}, {"/src/mid.h", 400}});
  EXPECT_EQ(askList(), (std::vector<std::string>{"/src/slow.h", "/src/mid.h"}));
  report({}, {{"/src/slow.h", 1000}, {"/src/mid.h", 300}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/slow.h"});
  EXPECT_EQ(Daemon->stats().HeadersDropped, 1u);
  // Dropping is final, as listing is.
  report({{"/src/mid.h", 90000}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/slow.h"});
}

TEST_F(DaemonTest, TheAverageHitCostDecides) {
  // The first hit loads the PCM; a later one in the same compile reuses it.
  useDaemon(dropCostly());
  report({{"/src/h.h", 1000}});
  report({}, {{"/src/h.h", 900}});
  EXPECT_TRUE(askList().empty()) << "900 + 200 us is more than 1000 us";
  report({{"/src/k.h", 1000}});
  report({}, {{"/src/k.h", 900}, {"/src/k.h", 100}, {"/src/k.h", 100}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/k.h"})
      << "an average of 367 + 200 us is less than 1000 us";
}

TEST_F(DaemonTest, DroppingIsOffByDefault) {
  report({{"/src/mid.h", 400}}, {{"/src/mid.h", 300}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/mid.h"});
}

TEST_F(DaemonTest, StoredHeadersAreListedWhenAskedTo) {
  llvm::sys::fs::remove(RegionPath);
  CacheDaemon::Options O;
  O.ListStored = true;
  auto D = CacheDaemon::create(RegionPath, 1u << 20, O);
  ASSERT_TRUE(bool(D)) << llvm::toString(D.takeError());
  Daemon = std::move(*D);
  push("/src/h.h", makeRecord("FEATURE", true), "PARSED");
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/h.h"});
}

TEST_F(DaemonTest, DistinctHeadersDoNotCollide) {
  push("a.h", makeRecord("A", true), "AAA");
  push("b.h", makeRecord("B", true), "BBB");
  ASSERT_EQ(lookup("a.h").Entries.size(), 1u);
  ASSERT_EQ(lookup("b.h").Entries.size(), 1u);
  EXPECT_EQ(metadata(lookup("a.h").Entries[0]).Conditional[0].MacroName, "A");
}

TEST_F(DaemonTest, StoredBytesComeBackExactly) {
  // The offset and length are only useful if what sits there is what was
  // pushed, byte for byte -- a parsed header is not text and does not
  // tolerate a truncation or a shifted start.
  std::string Data("\0\1\2\3 binary \xff\xfe", 14);
  push("h.h", makeRecord("X", true), Data);
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 1u);

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  llvm::StringRef Got =
      Attached->read(R.Entries[0].Offset, R.Entries[0].Length);
  EXPECT_EQ(Got.str(), Data);
}

TEST_F(DaemonTest, OutOfRangeOffsetReadsAsNothing) {
  // Offsets arrive over a socket, so a bogus one must read as empty rather
  // than as whatever happens to be mapped there.
  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached));
  EXPECT_TRUE(Attached->read(1u << 30, 16).empty());
  EXPECT_TRUE(Attached->read(0, 16).empty()) << "the header is not data";
}

TEST_F(DaemonTest, GarbageMessageClosesTheConnection) {
  // handleMessage returning nothing is how the connection loop decides to
  // hang up. Guessing at a malformed message is the one thing it must not do.
  EXPECT_FALSE(Daemon->handleMessage("\x7f garbage").has_value());
  EXPECT_FALSE(Daemon->handleMessage("").has_value());
}

} // namespace
