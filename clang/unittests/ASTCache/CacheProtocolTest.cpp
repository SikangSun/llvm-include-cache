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
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
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
  E.MetadataHash = 0xfeedface12345678;
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
  EXPECT_EQ(Got->Entries[0].MetadataHash, 0xfeedface12345678u);
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

TEST(CacheProtocolTest, TimingReportCarriesTheCombinationsHitsLoaded) {
  // CLANG_HEADER_CACHE_MERGE: each hit that loaded links, by the copy it
  // started from, the combination of copies it loaded, and the
  // instructions it took.
  TimingReport Report;
  Report.Used = {4096, 8192};
  Report.Combos = {{4096, 0x1234abcdull, 61400000}, {8192, 7, 0}};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Combos.size(), 2u);
  EXPECT_EQ(Got->Combos[0].Copy, 4096u);
  EXPECT_EQ(Got->Combos[0].Combo, 0x1234abcdull);
  EXPECT_EQ(Got->Combos[0].Instructions, 61400000u);
  EXPECT_EQ(Got->Combos[1].Combo, 7u);
  EXPECT_EQ(Got->Combos[1].Instructions, 0u);
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

TEST(CacheProtocolTest, TimingReportCarriesWhatTheFoldRuleReads) {
  // Each header read as text with its own time, the headers it included
  // directly, and whether a copy was asked for first; each link a hit
  // loaded.
  TimingReport Report;
  Report.Headers = {{"/src/server.h", 41250, 27068, 35, true},
                    {"/src/sds.h", 900, 640, 0, false}};
  Report.Links = {"/src/sds.h", "/src/sds.h", "/src/zmalloc.h"};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Headers.size(), 2u);
  EXPECT_EQ(Got->Headers[0].OwnMicros, 27068u);
  EXPECT_EQ(Got->Headers[0].Children, 35u);
  EXPECT_TRUE(Got->Headers[0].Asked);
  EXPECT_FALSE(Got->Headers[1].Asked);
  EXPECT_EQ(Got->Links, Report.Links);
}

TEST(CacheProtocolTest, TimingReportCarriesWhatTheMeasuredCutoffReads) {
  // The lines a copy of each header holds besides its own, and what the
  // compile's requests and loads cost it.
  TimingReport Report;
  Report.Headers = {{"/src/server.h", 41250, 27068, 35, true, 1500}};
  Report.Requests = {{"/src/server.h", 310}, {"/src/sds.h", 95}};
  Report.Loads = {{"/src/sds.h", 40}};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Headers.size(), 1u);
  EXPECT_EQ(Got->Headers[0].FoldedMicros, 1500u);
  ASSERT_EQ(Got->Requests.size(), 2u);
  EXPECT_EQ(Got->Requests[1].Path, "/src/sds.h");
  EXPECT_EQ(Got->Requests[1].Micros, 95u);
  ASSERT_EQ(Got->Loads.size(), 1u);
  EXPECT_EQ(Got->Loads[0].Micros, 40u);
}

TEST(CacheProtocolTest, TimingReportCarriesWhatTheMergePickReads) {
  // Each request in order: whether the .c file made it, and for a hit the
  // copies it loaded besides its own, their bytes, and the load time.
  TimingReport Report;
  Report.Asks = {{"/src/server.h", true, true, 31, 1611000, 2400},
                 {"/src/sds.h", false, false, 0, 0, 0}};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  ASSERT_EQ(Got->Asks.size(), 2u);
  EXPECT_EQ(Got->Asks[0].Path, "/src/server.h");
  EXPECT_TRUE(Got->Asks[0].FromMain);
  EXPECT_TRUE(Got->Asks[0].Hit);
  EXPECT_EQ(Got->Asks[0].Links, 31u);
  EXPECT_EQ(Got->Asks[0].Bytes, 1611000u);
  EXPECT_EQ(Got->Asks[0].LoadMicros, 2400u);
  EXPECT_FALSE(Got->Asks[1].FromMain);
  EXPECT_FALSE(Got->Asks[1].Hit);
}

TEST(CacheProtocolTest, HelloReplyRoundTrips) {
  llvm::SmallVector<char, 0> Out;
  encodeHelloReply({"/dev/shm/x.region", 0x0123456789abcdefull}, Out);
  auto Got = decodeHelloReply(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->RegionPath, "/dev/shm/x.region");
  EXPECT_EQ(Got->InstanceId, 0x0123456789abcdefull);

  EXPECT_EQ(Got->DecisionsPath, "");

  // The decisions of the daemon's offline step, once it wrote them.
  Out.clear();
  encodeHelloReply({"/dev/shm/x.region", 2, "/dev/shm/x.decisions"}, Out);
  Got = decodeHelloReply(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->DecisionsPath, "/dev/shm/x.decisions");
  EXPECT_EQ(Got->RecordDir, "");

  // Before them, where compiles record what the offline step reads.
  Out.clear();
  encodeHelloReply({"/dev/shm/x.region", 2, "", "/dev/shm/x.record"}, Out);
  Got = decodeHelloReply(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->DecisionsPath, "");
  EXPECT_EQ(Got->RecordDir, "/dev/shm/x.record");
  EXPECT_FALSE(Got->WantAsks);

  // A daemon that picks headers to merge wants the compile's requests.
  Out.clear();
  encodeHelloReply({"/dev/shm/x.region", 2, "", "", true}, Out);
  Got = decodeHelloReply(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_TRUE(Got->WantAsks);

  // A reply naming no file tells the client nothing it can attach to.
  Out.clear();
  encodeHelloReply({"", 1}, Out);
  EXPECT_FALSE(decodeHelloReply(body(Out)).has_value());
}

TEST(CacheProtocolTest, HelloSaysWhetherTheOfflineStepRunsTheCompile) {
  llvm::SmallVector<char, 0> Out;
  encodeHello(Out);
  auto Got = decodeHello(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_FALSE(Got->Offline);
  EXPECT_EQ(Got->Compile, "");
  EXPECT_EQ(Got->Build, 0u);
  Out.clear();
  encodeHello(Out, {/*Offline=*/true});
  Got = decodeHello(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_TRUE(Got->Offline);
}

TEST(CacheProtocolTest, HelloNamesTheCompile) {
  // Its directory, main file and options, with NUL bytes between them.
  std::string Compile = std::string("/src/redis\0server.c\0", 20) + "4F2A";
  llvm::SmallVector<char, 0> Out;
  encodeHello(Out, {false, Compile, /*Build=*/0x77});
  auto Got = decodeHello(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_FALSE(Got->Offline);
  EXPECT_EQ(Got->Compile, Compile);
  EXPECT_EQ(Got->Build, 0x77u);
}

TEST(CacheProtocolTest, HelloReplyGivesTheHeadersSkippedLastBuild) {
  HelloReply Reply{"/dev/shm/x.region", 2};
  Reply.NotListedBefore = {"/usr/include/pthread.h", "/src/config.h"};
  llvm::SmallVector<char, 0> Out;
  encodeHelloReply(Reply, Out);
  auto Got = decodeHelloReply(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->NotListedBefore, Reply.NotListedBefore);
}

TEST(CacheProtocolTest, TimingReportNamesTheCompileAndTheHeadersNotListed) {
  TimingReport Report;
  Report.Headers = {{"/usr/include/pthread.h", 900}};
  Report.Compile = std::string("/src/redis\0server.c\0h", 21);
  Report.NotListed = {"/usr/include/pthread.h", "/src/config.h"};
  llvm::SmallVector<char, 0> Out;
  encodeTimingReport(Report, Out);
  auto Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->Compile, Report.Compile);
  EXPECT_EQ(Got->NotListed, Report.NotListed);
  ASSERT_EQ(Got->Headers.size(), 1u);

  // A report from a compile that names none carries neither.
  Out.clear();
  encodeTimingReport({{{"/src/a.h", 1}}}, Out);
  Got = decodeTimingReport(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->Compile, "");
  EXPECT_TRUE(Got->NotListed.empty());
}

TEST(CacheProtocolTest, BuildHintRoundTrips) {
  llvm::SmallVector<char, 0> Out;
  encodeBuildHint({"/src/git/git"}, Out);
  EXPECT_EQ(peekMessageKind(body(Out)), MessageKind::BuildHint);
  auto Got = decodeBuildHint(body(Out));
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(Got->Output, "/src/git/git");

  Out.clear();
  encodeBuildHintAck(Out);
  EXPECT_EQ(peekMessageKind(body(Out)), MessageKind::BuildHintAck);
  EXPECT_FALSE(decodeBuildHint(body(Out)).has_value());
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
  /// The fold rule with an ask of 92 us, a load of 50 and a write of 519:
  /// the floor is 142 us, a miss loses 611 us.
  static CacheDaemon::Options fold(double Budget) {
    CacheDaemon::Options O;
    O.Fold = true;
    O.FoldBudget = Budget;
    return O;
  }
  void reportLinks(std::vector<std::string> Links) {
    TimingReport R;
    R.Links = std::move(Links);
    llvm::SmallVector<char, 0> Msg;
    encodeTimingReport(R, Msg);
    ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());
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

TEST_F(DaemonTest, ACompileGetsBackTheHeadersItReadWithoutAsking) {
  // CLANG_HEADER_CACHE_SAME_ASKS: the compile read pthread.h and config.h
  // without asking, as nothing was listed yet. Its report lists pthread.h;
  // in the next build the same compile is told to skip both again.
  auto Hello = [&](llvm::StringRef Compile) {
    llvm::SmallVector<char, 0> Msg;
    encodeHello(Msg, {false, Compile.str()});
    auto Reply = Daemon->handleMessage(body(Msg));
    EXPECT_TRUE(Reply.has_value());
    auto R = decodeHelloReply(body(*Reply));
    EXPECT_TRUE(R.has_value());
    return R.value_or(HelloReply{}).NotListedBefore;
  };
  using Names = std::vector<std::string>;
  EXPECT_EQ(Hello("c1"), Names{}) << "a compile not seen before";
  TimingReport Report;
  Report.Headers = {{"/src/pthread.h", 5000}, {"/src/config.h", 40}};
  Report.Compile = "c1";
  Report.NotListed = {"/src/pthread.h", "/src/config.h"};
  llvm::SmallVector<char, 0> Msg;
  encodeTimingReport(Report, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());
  EXPECT_EQ(askList(), Names{"/src/pthread.h"});
  EXPECT_EQ(Hello("c1"), (Names{"/src/pthread.h", "/src/config.h"}));
  EXPECT_EQ(Hello("c2"), Names{}) << "another compile";
  EXPECT_EQ(Hello(""), Names{}) << "a compile that names none";

  // The last build's report replaces the one before.
  Report.NotListed = {"/src/config.h"};
  Msg.clear();
  encodeTimingReport(Report, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());
  EXPECT_EQ(Hello("c1"), Names{"/src/config.h"});
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

TEST_F(DaemonTest, TheFoldFloorReadsTheHeadersOwnTimeAndItsChildren) {
  // A header's own lines and the asks for the headers it includes are what
  // a hit saves; under an ask and a load (142 us) it never gains. The time
  // with the headers inside it no longer decides.
  useDaemon(fold(8));
  report({{"/src/wrapper.h", 90000, 100, 0, false},
          {"/src/includes.h", 3000, 10, 2, false},
          {"/src/big.h", 5000, 5000, 0, false},
          {"/src/edge.h", 142, 142, 0, false}});
  EXPECT_EQ(askList(), (std::vector<std::string>{"/src/includes.h",
                                                 "/src/big.h", "/src/edge.h"}))
      << "10 us + 2 asks = 194 us passes; 100 us alone does not";
  EXPECT_EQ(Daemon->stats().HeadersBelowFloor, 1u);
}

TEST_F(DaemonTest, AHeaderIsFoldedOnceItsLossesPassTheBudget) {
  // Budget 2 writes = 1038 us. A miss loses an ask and a write (611 us).
  useDaemon(fold(2));
  report({{"/src/h.h", 300, 300, 0, false}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/h.h"});
  report({{"/src/h.h", 300, 300, 0, true}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/h.h"}) << "-611 us";
  report({{"/src/h.h", 300, 300, 0, true}});
  EXPECT_TRUE(askList().empty()) << "-1222 us is past the budget";
  EXPECT_EQ(Daemon->stats().HeadersFolded, 1u);
  // Folding is final, as listing is.
  report({}, {{"/src/h.h", 100}});
  report({{"/src/h.h", 90000, 90000, 0, false}});
  EXPECT_TRUE(askList().empty());
}

TEST_F(DaemonTest, HitsPayForMissesAndLinkLoadsCost) {
  // A hit of big.h gains its own 5000 us less an ask and a load (4858 us);
  // each load of its copy as another header's link loses a load (50 us).
  useDaemon(fold(2));
  report({{"/src/big.h", 5000, 5000, 0, false}});
  report({{"/src/big.h", 5000, 5000, 0, true}}, {{"/src/big.h", 300}});
  report({{"/src/big.h", 5000, 5000, 0, true}});
  report({{"/src/big.h", 5000, 5000, 0, true}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/big.h"})
      << "4858 - 3 x 611 = 3025 us";
  reportLinks(std::vector<std::string>(81, "/src/big.h"));
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/big.h"})
      << "3025 - 81 x 50 = -1025 us is within the budget";
  reportLinks({"/src/big.h"});
  EXPECT_TRUE(askList().empty()) << "-1075 us is past it";
}

TEST_F(DaemonTest, TheFoldFloorCanReadTheOwnTimeOrTodaysRule) {
  // -fold-list=own: the own time alone against an ask and a load (142 us);
  // -fold-list=inclusive: the time with the headers inside it against the
  // ask cutoff (226 us), as without -fold.
  CacheDaemon::Options O = fold(8);
  O.FoldListBy = CacheDaemon::Options::FoldListing::Own;
  useDaemon(O);
  report({{"/src/includes.h", 3000, 10, 2, false},
          {"/src/big.h", 5000, 5000, 0, false}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/big.h"});
  O.FoldListBy = CacheDaemon::Options::FoldListing::Inclusive;
  useDaemon(O);
  report({{"/src/includes.h", 3000, 10, 2, false},
          {"/src/small.h", 200, 200, 0, false}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/includes.h"});
}

TEST_F(DaemonTest, LinkLoadsCanCostNothing) {
  // -fold-link-us=0: a header loaded only as other headers' links is not
  // folded for it.
  CacheDaemon::Options O = fold(2);
  O.FoldLinkMicros = 0;
  useDaemon(O);
  report({{"/src/stdio.h", 4000, 4000, 12, false}});
  reportLinks(std::vector<std::string>(500, "/src/stdio.h"));
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/stdio.h"});
  EXPECT_EQ(Daemon->stats().HeadersFolded, 0u);
}

TEST_F(DaemonTest, WithTheMeasuredCutoffAHeaderIsAskedForAtFirstByTheDefault) {
  // A header no compile has asked for or loaded has no cost of its own to
  // compare with: its load costs the default, -ask-cutoff-us (226 us), and
  // its requests nothing. wrapper.h's 400 us pay for it; small.h's 3 us do
  // not.
  CacheDaemon::Options O;
  O.MeasuredCutoff = true;
  useDaemon(O);
  report({{"/src/wrapper.h", 400, 20, 3, false, 300},
          {"/src/small.h", 3, 3, 0, false, 0}});
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/wrapper.h"});
}

TEST_F(DaemonTest, TheMeasuredCutoffIsTheHeadersOwnRequestAndLoad) {
  // A hit on a header costs its own mean request and mean load; it saves
  // the lines its copy holds. Each report judges every header again.
  CacheDaemon::Options O;
  O.MeasuredCutoff = true;
  useDaemon(O);
  report({{"/src/mid.h", 300, 300, 0, false, 0},
          {"/src/big.h", 5000, 5000, 0, false, 0},
          {"/src/tiny.h", 30, 30, 0, false, 0}});
  llvm::SmallVector<char, 0> Msg;
  TimingReport R;
  R.Headers = {{"/src/big.h", 5000, 5000, 0, true, 0}};
  R.Requests = {{"/src/mid.h", 250},
                {"/src/mid.h", 350},
                {"/src/big.h", 900},
                {"/src/tiny.h", 40}};
  R.Loads = {{"/src/mid.h", 60}, {"/src/big.h", 400}};
  encodeTimingReport(R, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());
  EXPECT_EQ(askList(), std::vector<std::string>{"/src/big.h"})
      << "mid.h: 300 us is under 300 + 60 us; tiny.h: 30 us is under one "
         "40 us request and the default load, 226 us, as no load is measured";
  EXPECT_EQ(Daemon->stats().HeadersUnlisted, 1u)
      << "only mid.h was listed first: tiny.h's 30 us is under 226 us";
  // mid.h now holds a header it used to link: 300 + 100 us.
  report({{"/src/mid.h", 400, 300, 1, false, 100}});
  EXPECT_EQ(askList(), (std::vector<std::string>{"/src/big.h", "/src/mid.h"}));
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

TEST_F(DaemonTest, TheRegionsIndexListsEachCopyNewestFirst) {
  // A compile finds a header's copies in the region, with no request: the
  // index gives the newest copy's record, and each record the one stored
  // before it.
  push("h.h", makeRecord("FEATURE", true), "AAA");
  push("h.h", makeRecord("FEATURE", false), "BBBB");
  push("other.h", makeRecord("OTHER", true), "CC");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 2u);

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  const IndexedCopy *Newest = Attached->copyAt(Attached->newestCopy("h.h"));
  ASSERT_NE(Newest, nullptr);
  EXPECT_EQ(Newest->Length, 4u);
  EXPECT_EQ(Newest->MetadataOffset, R.Entries[1].MetadataOffset);
  EXPECT_EQ(Newest->MetadataHash, R.Entries[1].MetadataHash);
  const IndexedCopy *Older = Attached->copyAt(Newest->Next);
  ASSERT_NE(Older, nullptr);
  EXPECT_EQ(Older->Length, 3u);
  EXPECT_EQ(Older->Offset, R.Entries[0].Offset);
  EXPECT_EQ(Older->Next, 0u);
  EXPECT_EQ(Attached->read(Older->Offset, Older->Length), "AAA");
  EXPECT_EQ(Attached->copyAt(Attached->newestCopy("other.h"))->Length, 2u);
  EXPECT_EQ(Attached->newestCopy("never-stored.h"), 0u);
  EXPECT_EQ(Attached->copyAt(0), nullptr);
}

TEST_F(DaemonTest, AUseInACompilesReportStampsItsCopyInTheIndex) {
  // The copy used most recently is checked first. A compile that reads the
  // index sends what it used with its timing report, and the daemon stamps
  // the copy's record.
  push("h.h", makeRecord("FEATURE", true), "AAA");
  push("h.h", makeRecord("FEATURE", false), "BBBB");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 2u);
  TimingReport Report;
  Report.Used = {R.Entries[0].MetadataOffset};
  llvm::SmallVector<char, 0> Msg;
  encodeTimingReport(Report, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  const IndexedCopy *Newest = Attached->copyAt(Attached->newestCopy("h.h"));
  ASSERT_NE(Newest, nullptr);
  const IndexedCopy *Older = Attached->copyAt(Newest->Next);
  ASSERT_NE(Older, nullptr);
  EXPECT_EQ(Newest->Stamp.load(), 0u);
  EXPECT_GT(Older->Stamp.load(), 0u);
  // The daemon's own reply gives the same order.
  EXPECT_EQ(lookup("h.h").Entries[0].MetadataOffset, Older->MetadataOffset);
}

TEST_F(DaemonTest, EachCombinationAHitLoadedIsCountedAndMergedOnce) {
  // CLANG_HEADER_CACHE_MERGE: hits that started from one copy and loaded
  // two combinations of its links are counted apart. A merge of each is
  // granted once; the copy pushed on that connection is its merged copy.
  push("h.h", makeRecord("FEATURE", true), "AAA");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 1u);
  uint64_t Copy = R.Entries[0].MetadataOffset;
  TimingReport Report;
  Report.Used = {Copy, Copy, Copy};
  Report.Combos = {{Copy, 11, 1000}, {Copy, 22, 0}, {Copy, 11, 500}};
  llvm::SmallVector<char, 0> Msg;
  encodeTimingReport(Report, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  uint64_t At = Attached->newestCopy("h.h");
  const IndexedCopy *C = Attached->copyAt(At);
  ASSERT_NE(C, nullptr);
  EXPECT_EQ(C->Serves.load(), 3u);
  const MergeRecord *First = Attached->comboOf(*C, 11);
  const MergeRecord *Second = Attached->comboOf(*C, 22);
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  EXPECT_EQ(First->Serves.load(), 2u);
  EXPECT_EQ(Second->Serves.load(), 1u);
  EXPECT_EQ(First->Instructions.load(), 1500u);
  EXPECT_EQ(Second->Instructions.load(), 0u);
  EXPECT_EQ(Attached->comboOf(*C, 33), nullptr);

  auto Claim = [&](uint64_t Combo, uint64_t Conn) {
    llvm::SmallVector<char, 0> M;
    encodeClaimRequest({"h.h", "merge:" + llvm::utohexstr(At) + ":" +
                                   llvm::utohexstr(Combo),
                        false},
                       M);
    std::optional<llvm::SmallVector<char, 0>> Reply =
        Daemon->handleMessage(body(M), Conn);
    std::optional<bool> Granted =
        Reply ? decodeClaimReply(body(*Reply)) : std::nullopt;
    return Granted && *Granted;
  };
  EXPECT_TRUE(Claim(11, 7));
  EXPECT_FALSE(Claim(11, 8)) << "the same combination was merged twice";
  EXPECT_TRUE(Claim(22, 8));
  EXPECT_EQ(First->Merged.load(), 1u);

  PushRequest Merged;
  Merged.Filename = "h.h";
  Merged.ModuleName = "merged";
  Merged.Deps = makeRecord("FEATURE", true);
  Merged.Data = "MERGED";
  llvm::SmallVector<char, 0> P;
  encodePushRequest(Merged, P);
  ASSERT_TRUE(Daemon->handleMessage(body(P), 7).has_value());
  EXPECT_EQ(First->Merged.load(), Attached->newestCopy("h.h"));
  EXPECT_EQ(Second->Merged.load(), 1u);
}

TEST_F(DaemonTest, WithOneMergeAHeaderMergesOneCombination) {
  // -pick-one-merge (measurement): once a merge of h.h is granted, a merge
  // of another combination of its links is refused, until the granted one
  // is given back.
  CacheDaemon::Options O;
  O.PickOneMerge = true;
  useDaemon(O);
  push("h.h", makeRecord("FEATURE", true), "AAA");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 1u);
  uint64_t Copy = R.Entries[0].MetadataOffset;
  TimingReport Report;
  Report.Used = {Copy, Copy};
  Report.Combos = {{Copy, 11, 1000}, {Copy, 22, 0}};
  llvm::SmallVector<char, 0> Msg;
  encodeTimingReport(Report, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  uint64_t At = Attached->newestCopy("h.h");
  auto Claim = [&](uint64_t Combo, uint64_t Conn, bool Release = false) {
    llvm::SmallVector<char, 0> M;
    encodeClaimRequest({"h.h", "merge:" + llvm::utohexstr(At) + ":" +
                                   llvm::utohexstr(Combo),
                        Release},
                       M);
    std::optional<llvm::SmallVector<char, 0>> Reply =
        Daemon->handleMessage(body(M), Conn);
    std::optional<bool> Granted =
        Reply ? decodeClaimReply(body(*Reply)) : std::nullopt;
    return Granted && *Granted;
  };
  EXPECT_TRUE(Claim(11, 7));
  EXPECT_FALSE(Claim(22, 8)) << "h.h merged a second combination";
  EXPECT_EQ(Daemon->stats().MergesRefusedOnce, 1u);
  // The first merged copy was declined: h.h may merge another combination.
  Claim(11, 7, /*Release=*/true);
  EXPECT_TRUE(Claim(22, 8));
  EXPECT_EQ(Daemon->stats().MergesClaimed, 2u);
}

TEST_F(DaemonTest, WhatACopysSearchesFoundIsInTheIndex) {
  // REPORT.md I9: within one build, what a copy's #include searches find
  // is shared between compiles. With no request, it is in the index.
  push("h.h", makeRecord("FEATURE", true), "AAA");
  LookupResponse R = lookup("h.h");
  ASSERT_EQ(R.Entries.size(), 1u);
  TimingReport Report;
  Report.Build = 7;
  Report.Learned.push_back({"h.h", R.Entries[0].MetadataOffset, {5, 2}});
  Report.Learned.push_back({"h.h", R.Entries[0].MetadataOffset, {9, 0}});
  llvm::SmallVector<char, 0> Msg;
  encodeTimingReport(Report, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());

  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  const IndexedCopy *C = Attached->copyAt(Attached->newestCopy("h.h"));
  ASSERT_NE(C, nullptr);
  std::optional<StoredSearches> F = Attached->searchesAt(C->Searches.load());
  ASSERT_TRUE(F.has_value());
  EXPECT_EQ(F->Build, 7u);
  EXPECT_EQ(F->Pairs.vec(), (std::vector<uint64_t>{5, 2, 9, 0}));
}

TEST_F(DaemonTest, ACopyTheIndexCannotHoldIsNotStored) {
  // A 1 MiB region's index has 256 slots. A copy of a 257th header is not
  // stored: a compile that reads the index would not find it.
  for (int I = 0; I != 257; ++I)
    push("h" + std::to_string(I) + ".h", makeRecord("X", true), "D");
  EXPECT_EQ(Daemon->stats().Entries, 256u);
  EXPECT_EQ(Daemon->stats().Dropped, 1u);
  auto Attached = SharedRegion::attach(RegionPath);
  ASSERT_TRUE(bool(Attached)) << llvm::toString(Attached.takeError());
  EXPECT_EQ(Attached->newestCopy("h256.h"), 0u);
  EXPECT_NE(Attached->newestCopy("h255.h"), 0u);
}

/// The five-counter merge pick (CacheDaemonOptions::MergePicks).
class PickTest : public DaemonTest {
protected:
  void usePicks() {
    CacheDaemon::Options O;
    O.MergePicks = true;
    O.PicksPath = TestDir + "/picks";
    useDaemon(O);
  }
  HelloReply hello(uint64_t Build = 0) {
    llvm::SmallVector<char, 0> Msg;
    encodeHello(Msg, {false, "", Build});
    auto Reply = Daemon->handleMessage(body(Msg));
    EXPECT_TRUE(Reply.has_value());
    return decodeHelloReply(body(*Reply)).value_or(HelloReply{});
  }
  /// One compile's report: core.h first from the .c file, a hit loading
  /// \p CoreLinks links, then 58 small headers from inside it, each a hit
  /// with no links; with \p Big, big.h too, from inside, a hit that loads
  /// more links and bytes than core.h but a ninth of its load time. With
  /// \p CoreFirst false, core.h comes from inside a header, and the .c file
  /// asks for no header.
  void compile(uint32_t CoreLinks = 30, bool Big = false,
               bool CoreFirst = true, uint64_t Build = 0) {
    TimingReport R;
    R.Build = Build;
    R.Asks.push_back({"/src/core.h", CoreFirst, true, CoreLinks, 500000, 900});
    if (Big)
      R.Asks.push_back({"/src/big.h", false, true, 40, 900000, 100});
    for (int I = 0; I != 58; ++I)
      R.Asks.push_back(
          {"/src/small" + std::to_string(I) + ".h", false, true, 0, 1000, 10});
    llvm::SmallVector<char, 0> Msg;
    encodeTimingReport(R, Msg);
    ASSERT_TRUE(Daemon->handleMessage(body(Msg)).has_value());
  }
  std::string picks() {
    auto F = llvm::MemoryBuffer::getFile(TestDir + "/picks");
    return F ? (*F)->getBuffer().str() : "";
  }
};

TEST_F(PickTest, PicksAreOffByDefault) {
  // The default daemon wants no request lists and names no picks.
  HelloReply H = hello();
  EXPECT_FALSE(H.WantAsks);
  for (int I = 0; I != 12; ++I)
    compile();
  EXPECT_EQ(Daemon->stats().Picks, 0u);
  EXPECT_EQ(hello().DecisionsPath, "");
}

TEST_F(PickTest, AHeaderOnTopOfAllFiveIsListedWhileTheyAreSteady) {
  // core.h tops all five counters: its hits take 61% of the load time
  // (900 of 900 + 58 x 10 us), load 31 copies and 500 KB a hit against the
  // median header's 1 copy and 1 KB, every compile asks for it, and the .c
  // file asks for it first. The small headers are high only on the share
  // of compiles asking, which every header shares. Every compile is alike,
  // so the counters are steady from the first check, the 10th compile.
  usePicks();
  EXPECT_TRUE(hello().WantAsks);
  for (int I = 0; I != 9; ++I)
    compile();
  EXPECT_EQ(Daemon->stats().Picks, 0u) << "before the 10th compile";
  EXPECT_EQ(hello().DecisionsPath, "");
  compile();
  EXPECT_EQ(Daemon->stats().Picks, 1u);
  EXPECT_EQ(picks(), "merge-root\t/src/core.h\n");
  EXPECT_EQ(hello().DecisionsPath, TestDir + "/picks")
      << "a hit on core.h in a compile that starts now merges its tree";
  // It stays on the list while it passes.
  compile();
  EXPECT_EQ(Daemon->stats().Picks, 1u);
  EXPECT_EQ(picks(), "merge-root\t/src/core.h\n");
}

TEST_F(PickTest, AHeaderLeavesTheListWhenACounterMoves) {
  usePicks();
  for (int I = 0; I != 10; ++I)
    compile();
  EXPECT_EQ(picks(), "merge-root\t/src/core.h\n");
  // Then core.h's links double every compile. The top of copies per hit,
  // core.h's own, moves by more than 20% over the last tenth of the build,
  // so nothing passes.
  for (unsigned K = 1; K != 5; ++K)
    compile(/*CoreLinks=*/30u << K);
  EXPECT_EQ(picks(), "");
  EXPECT_EQ(hello().DecisionsPath, "") << "compiles that start now merge none";
  EXPECT_EQ(Daemon->stats().Picks, 1u) << "headers that were ever on it";
}

TEST_F(PickTest, ThePickRunsInTheFirstBuildOnly) {
  // User 2026-11-06: the merge heuristic runs in the first build only. The
  // daemon takes the first build a compile names when it connects. A
  // compile of a later build lists no requests and takes no list, and its
  // report counts for nothing: core.h's links double in each compile of
  // build 9, which in build 5 takes it off the list
  // (AHeaderLeavesTheListWhenACounterMoves).
  usePicks();
  EXPECT_TRUE(hello(5).WantAsks);
  for (int I = 0; I != 10; ++I)
    compile(30, false, true, /*Build=*/5);
  EXPECT_EQ(picks(), "merge-root\t/src/core.h\n");
  EXPECT_EQ(hello(5).DecisionsPath, TestDir + "/picks");
  HelloReply Next = hello(9);
  EXPECT_FALSE(Next.WantAsks);
  EXPECT_EQ(Next.DecisionsPath, "") << "a hit in build 9 merges nothing";
  for (unsigned K = 1; K != 5; ++K)
    compile(/*CoreLinks=*/30u << K, false, true, /*Build=*/9);
  EXPECT_EQ(picks(), "merge-root\t/src/core.h\n");
  // A compile that names no build is taken to be of build 5.
  EXPECT_TRUE(hello().WantAsks);
}

TEST_F(PickTest, NothingIsListedWhileACounterMoves) {
  // core.h's links double every compile, so the top of copies per hit
  // moves by far more than 20% over each last tenth of the build.
  usePicks();
  for (int I = 0; I != 14; ++I)
    compile(/*CoreLinks=*/1u << I);
  EXPECT_EQ(Daemon->stats().Picks, 0u);
  EXPECT_EQ(Daemon->stats().PickSteadyReports, 0u);
  EXPECT_EQ(picks(), "");
}

TEST_F(PickTest, AllFiveCountersMustCallAHeaderHigh) {
  // core.h tops four counters, but the .c file asks for no header first,
  // so no header is high on the first-request share. Nothing passes,
  // though the counters are steady.
  usePicks();
  for (int I = 0; I != 12; ++I)
    compile(30, /*Big=*/false, /*CoreFirst=*/false);
  EXPECT_EQ(Daemon->stats().Picks, 0u);
  EXPECT_GT(Daemon->stats().PickSteadyReports, 0u);
  EXPECT_EQ(picks(), "");
  // big.h tops copies and bytes per hit; core.h tops the load-time share
  // and the .c file's first request; both share the top of compiles
  // asking. Each is high on three counters, so neither passes.
  usePicks();
  for (int I = 0; I != 12; ++I)
    compile(30, /*Big=*/true);
  EXPECT_EQ(Daemon->stats().Picks, 0u);
  EXPECT_EQ(picks(), "");
}

/// The daemon's offline step, with a shell script for offline.py: run after
/// a link's hint with the record directory and the decisions file last.
class OfflineTest : public DaemonTest {
protected:
  void useOffline(llvm::StringRef Script) {
    CacheDaemon::Options O;
    O.OfflineCommand = {"/bin/sh", "-c", Script.str(), "sh"};
    O.OfflineRecord = TestDir + "/record";
    O.OfflineDecisions = TestDir + "/decisions";
    useDaemon(O);
  }
  HelloReply hello() {
    llvm::SmallVector<char, 0> Msg;
    encodeHello(Msg);
    auto Reply = Daemon->handleMessage(body(Msg));
    EXPECT_TRUE(Reply.has_value());
    return decodeHelloReply(body(*Reply)).value_or(HelloReply{});
  }
  void hint() {
    llvm::SmallVector<char, 0> Msg;
    encodeBuildHint({"/src/a.out"}, Msg);
    auto Reply = Daemon->handleMessage(body(Msg));
    ASSERT_TRUE(Reply.has_value());
    EXPECT_EQ(peekMessageKind(body(*Reply)), MessageKind::BuildHintAck);
  }
  /// Waits up to 20 s for \p Done.
  template <typename F> bool waitFor(F Done) {
    for (int I = 0; I != 2000 && !Done(); ++I)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return Done();
  }
};

TEST_F(OfflineTest, AHintRunsTheStepAndLaterCompilesTakeItsDecisions) {
  useOffline("test -d \"$1\" && printf 'top\\t/x.h\\n' > \"$2\"");
  // Before: compiles record, and there are no decisions to take.
  HelloReply Before = hello();
  EXPECT_EQ(Before.RecordDir, TestDir + "/record");
  EXPECT_EQ(Before.DecisionsPath, "");
  EXPECT_TRUE(llvm::sys::fs::is_directory(TestDir + "/record"));

  hint();
  ASSERT_TRUE(waitFor([&] { return Daemon->stats().OfflineFinished == 1; }));
  CacheDaemon::Stats S = Daemon->stats();
  EXPECT_EQ(S.BuildHints, 1u);
  EXPECT_EQ(S.OfflineStarted, 1u);
  EXPECT_EQ(S.OfflineFailed, 0u);
  HelloReply After = hello();
  EXPECT_EQ(After.DecisionsPath, TestDir + "/decisions");
  EXPECT_EQ(After.RecordDir, "");
  // The log ends with the run's wall time.
  auto Log = llvm::MemoryBuffer::getFile(TestDir + "/decisions.log");
  ASSERT_TRUE(bool(Log));
  EXPECT_TRUE((*Log)->getBuffer().starts_with("offline run 1: "));

  // Decided: a later hint runs nothing.
  hint();
  EXPECT_EQ(Daemon->stats().OfflineStarted, 1u);
  EXPECT_EQ(Daemon->stats().BuildHints, 2u);
}

TEST_F(OfflineTest, ALookupStopsARunAndTheNextHintRunsItAgain) {
  useOffline("if [ -e \"$1/go\" ]; then printf 'top\\t/x.h\\n' > \"$2\"; "
             "else touch \"$1/go\"; exec sleep 30; fi");
  hint();
  ASSERT_TRUE(waitFor(
      [&] { return llvm::sys::fs::exists(TestDir + "/record/go"); }));
  // The build goes on: the run reads a record still growing.
  lookup("h.h");
  EXPECT_EQ(Daemon->stats().OfflineStopped, 1u);
  EXPECT_FALSE(llvm::sys::fs::exists(TestDir + "/decisions"));
  EXPECT_EQ(hello().DecisionsPath, "");

  hint();
  ASSERT_TRUE(waitFor([&] { return Daemon->stats().OfflineFinished == 1; }));
  EXPECT_EQ(Daemon->stats().OfflineStarted, 2u);
  EXPECT_EQ(hello().DecisionsPath, TestDir + "/decisions");
}

TEST_F(OfflineTest, ALaterHintStopsTheRunInFlight) {
  // The next link's hint: the run in flight read a record still growing.
  useOffline("if [ -e \"$1/go\" ]; then printf 'top\\t/x.h\\n' > \"$2\"; "
             "else touch \"$1/go\"; exec sleep 30; fi");
  hint();
  ASSERT_TRUE(waitFor(
      [&] { return llvm::sys::fs::exists(TestDir + "/record/go"); }));
  hint();
  ASSERT_TRUE(waitFor([&] { return Daemon->stats().OfflineFinished == 1; }));
  CacheDaemon::Stats S = Daemon->stats();
  EXPECT_EQ(S.OfflineStarted, 2u);
  EXPECT_EQ(S.OfflineStopped, 1u);
  EXPECT_EQ(S.OfflineFailed, 0u);
}

TEST_F(OfflineTest, TheStepsOwnCompilesNeitherStopItNorRecord) {
  // The step's merges run compiles (CLANG_HEADER_CACHE_OFFLINE=1): their
  // lookups do not stop it, and they record nothing.
  useOffline("touch \"$1/go\"; while [ ! -e \"$1/end\" ]; do sleep 0.01; done; "
             "printf 'top\\t/x.h\\n' > \"$2\"");
  hint();
  ASSERT_TRUE(waitFor(
      [&] { return llvm::sys::fs::exists(TestDir + "/record/go"); }));
  const uint64_t Own = 7;
  llvm::SmallVector<char, 0> Msg;
  encodeHello(Msg, {/*Offline=*/true});
  auto Reply = Daemon->handleMessage(body(Msg), Own);
  ASSERT_TRUE(Reply.has_value());
  auto Hello = decodeHelloReply(body(*Reply));
  ASSERT_TRUE(Hello.has_value());
  EXPECT_EQ(Hello->RecordDir, "");
  EXPECT_EQ(Hello->DecisionsPath, "");
  Msg.clear();
  encodeLookupRequest({"h.h"}, Msg);
  ASSERT_TRUE(Daemon->handleMessage(body(Msg), Own).has_value());
  EXPECT_EQ(Daemon->stats().OfflineStopped, 0u);
  {
    std::error_code EC;
    llvm::raw_fd_ostream End(TestDir + "/record/end", EC);
    ASSERT_FALSE(EC);
  }
  ASSERT_TRUE(waitFor([&] { return Daemon->stats().OfflineFinished == 1; }));
  EXPECT_EQ(Daemon->stats().OfflineStopped, 0u);
}

TEST_F(OfflineTest, AFailedRunWritesNoDecisions) {
  useOffline("exit 3");
  hint();
  ASSERT_TRUE(waitFor([&] { return Daemon->stats().OfflineFailed == 1; }));
  EXPECT_EQ(Daemon->stats().OfflineFinished, 0u);
  EXPECT_EQ(hello().DecisionsPath, "");
  EXPECT_EQ(hello().RecordDir, TestDir + "/record");
}

TEST_F(DaemonTest, GarbageMessageClosesTheConnection) {
  // handleMessage returning nothing is how the connection loop decides to
  // hang up. Guessing at a malformed message is the one thing it must not do.
  EXPECT_FALSE(Daemon->handleMessage("\x7f garbage").has_value());
  EXPECT_FALSE(Daemon->handleMessage("").has_value());
}

} // namespace
