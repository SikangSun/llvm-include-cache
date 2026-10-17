//===- CacheDaemon.cpp ------------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_socket_stream.h"
#include "llvm/Support/xxhash.h"
#include <thread>

using namespace clang;
using namespace clang::ast_cache;

llvm::Expected<std::unique_ptr<CacheDaemon>>
CacheDaemon::create(llvm::StringRef RegionPath, uint64_t RegionSize,
                    Options Opts) {
  llvm::SmallString<256> Absolute(RegionPath);
  if (std::error_code EC = llvm::sys::fs::make_absolute(Absolute))
    return llvm::createStringError(EC, "resolving the region path");
  llvm::Expected<SharedRegion> R = SharedRegion::create(Absolute, RegionSize);
  if (!R)
    return R.takeError();

  std::unique_ptr<CacheDaemon> D(new CacheDaemon());
  D->Region = std::move(*R);
  D->RegionPath = std::string(Absolute);
  D->Opts = Opts;
  return D;
}

bool CacheDaemon::listLocked(llvm::StringRef Path) {
  if (Dropped.contains(Path) || !ListedSet.insert(Path).second)
    return false;
  Listed.push_back(Path.str());
  Counters.HeadersListed = Listed.size();
  return true;
}

bool CacheDaemon::dropIfCostlyLocked(llvm::StringRef Path) {
  if (!Opts.DropCostly || !ListedSet.contains(Path))
    return false;
  auto Parse = FirstTiming.find(Path);
  auto Hits = HitCosts.find(Path);
  if (Parse == FirstTiming.end() || Hits == HitCosts.end())
    return false;
  // A hit replaces the parse, and costs its query, its replay and what the
  // loaded PCM adds to every later name lookup in the compile.
  uint64_t HitCost = Hits->second.second / Hits->second.first;
  if (Parse->second >= HitCost + Opts.LaterCostMicros)
    return false;
  ListedSet.erase(Path);
  llvm::erase(Listed, Path);
  Dropped.insert(Path);
  Counters.HeadersListed = Listed.size();
  ++Counters.HeadersDropped;
  return true;
}

void CacheDaemon::publishLocked() {
  // The whole list each time. It is at most a few hundred paths -- 778 for
  // qemu, 35 KB -- and changes only while a build's first compiles report,
  // after which membership is settled.
  std::string List;
  for (const std::string &P : Listed) {
    List += P;
    List += '\0';
  }
  // A full region leaves the previous list in place, which costs asks, never
  // correctness: the list only decides whether to ask.
  Region.publishAskList(List);
}

void CacheDaemon::learnLocked(uint64_t Build,
                              const std::vector<LearnedSearch> &Learned,
                              const std::vector<uint64_t> &Used) {
  for (uint64_t U : Used)
    LastUsed[U] = ++UseClock;
  if (!Build)
    return;
  for (const LearnedSearch &L : Learned) {
    auto It = Table.find(L.Filename);
    if (It == Table.end())
      continue;
    for (StoredEntry &E : It->second) {
      if (E.MetadataOffset != L.Entry)
        continue;
      if (E.SearchBuild != Build) {
        E.SearchBuild = Build;
        E.Searches.clear();
      }
      if (llvm::none_of(E.Searches, [&](const SearchOutcome &O) {
            return O.Key == L.Found.Key;
          }))
        E.Searches.push_back(L.Found);
    }
  }
}

llvm::SmallVector<char, 0>
CacheDaemon::handleTiming(const TimingReport &Report) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    learnLocked(Report.Build, Report.Learned, Report.Used);
    ++Counters.TimingReports;
    bool Changed = false;
    for (const HeaderTiming &H : Report.Headers) {
      if (H.Path.empty() || !FirstTiming.try_emplace(H.Path, H.Micros).second)
        continue;
      if (H.Micros >= Opts.AskCutoffMicros)
        Changed |= listLocked(H.Path);
    }
    // All of a compile's hits count before any header is judged: the first
    // hit on a header loads its PCM, a later one in the same compile reuses
    // it.
    llvm::StringSet<> HitHeaders;
    for (const HeaderTiming &H : Report.Hits) {
      if (H.Path.empty())
        continue;
      auto &[Count, Total] = HitCosts[H.Path];
      ++Count;
      Total += H.Micros;
      ++Counters.HitsTimed;
      HitHeaders.insert(H.Path);
    }
    for (const auto &H : HitHeaders)
      Changed |= dropIfCostlyLocked(H.getKey());
    Counters.HeadersTimed = FirstTiming.size();
    if (Changed)
      publishLocked();
  }
  llvm::SmallVector<char, 0> Ack;
  encodeTimingAck(Ack);
  return Ack;
}

llvm::SmallVector<char, 0> CacheDaemon::handleHello() {
  llvm::SmallVector<char, 0> Out;
  encodeHelloReply({RegionPath, Region.instanceId()}, Out);
  return Out;
}

llvm::SmallVector<char, 0>
CacheDaemon::handleLookup(const LookupRequest &Req) {
  LookupResponse Resp;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    learnLocked(Req.Build, Req.Learned, Req.Used);
    ++Counters.Lookups;
    auto It = Table.find(Req.Filename);
    if (It != Table.end()) {
      ++Counters.Hits;
      // Where every stored entry for the name is, with no comparison of its
      // metadata here. The daemon has no way to evaluate a dependency --
      // that needs the client's live macro table and its Sema -- and sending
      // the client's whole state here instead would be far larger than the
      // handful of names a header actually depends on.
      // The most recently used first: a compile checks them in order and
      // takes the first that fits, which is most often the one that fit
      // the compile before it (REPORT.md I15).
      std::vector<const StoredEntry *> Order;
      Order.reserve(It->second.size());
      for (const StoredEntry &E : It->second)
        Order.push_back(&E);
      std::stable_sort(Order.begin(), Order.end(),
                       [&](const StoredEntry *A, const StoredEntry *B) {
                         return LastUsed.lookup(A->MetadataOffset) >
                                LastUsed.lookup(B->MetadataOffset);
                       });
      Resp.Entries.reserve(Order.size());
      for (const StoredEntry *E : Order)
        Resp.Entries.push_back({E->MetadataOffset, E->MetadataLength,
                                E->Offset, E->Length,
                                Req.Build && E->SearchBuild == Req.Build
                                    ? E->Searches
                                    : std::vector<SearchOutcome>()});
    }
  }

  llvm::SmallVector<char, 0> Out;
  encodeLookupResponse(Resp, Out);
  return Out;
}

llvm::SmallVector<char, 0> CacheDaemon::handlePush(const ReceivedPush &Req) {
  uint64_t Hash = llvm::xxh3_64bits(Req.Metadata);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    ++Counters.Pushes;
    std::vector<StoredEntry> &Entries = Table[Req.Filename];

    for (const StoredEntry &E : Entries) {
      if (E.Hash == Hash &&
          Region.read(E.MetadataOffset, E.MetadataLength) == Req.Metadata) {
        // Another client stored this exact result while this one was still
        // compiling. Discard it and acknowledge: the client that lost the
        // race already holds its own result and has no use for the winner.
        ++Counters.Duplicates;
        llvm::SmallVector<char, 0> Ack;
        encodePushAck(Ack);
        return Ack;
      }
    }

    // An entry is recorded only with its metadata, which every compile reads
    // in place in the region, as it reads the copy.
    std::optional<uint64_t> MetadataAt = Region.append(Req.Metadata);
    std::optional<uint64_t> Offset =
        MetadataAt ? Region.append(Req.Data) : std::nullopt;
    if (Offset) {
      StoredEntry E;
      E.Hash = Hash;
      E.MetadataOffset = *MetadataAt;
      E.MetadataLength = Req.Metadata.size();
      E.Offset = *Offset;
      E.Length = Req.Data.size();
      Entries.push_back(std::move(E));
      Counters.BytesStored += Req.Data.size() + Req.Metadata.size();
      Counters.PartBytes += Req.Data.size();
      Counters.MetadataBytes += Req.Metadata.size();
      ++Counters.Entries;
      Counters.Headers = Table.size();
      if (Opts.ListStored && listLocked(Req.Filename))
        publishLocked();
    }
    // A full region is not an error the client can act on -- it already has
    // its own result -- so a failed append is acknowledged like any other
    // push and the entry is simply not recorded.
  }

  llvm::SmallVector<char, 0> Ack;
  encodePushAck(Ack);
  return Ack;
}

std::optional<llvm::SmallVector<char, 0>>
CacheDaemon::handleMessage(llvm::StringRef Body) {
  std::optional<MessageKind> Kind = peekMessageKind(Body);
  if (!Kind)
    return std::nullopt;

  switch (*Kind) {
  case MessageKind::LookupRequest:
    if (std::optional<LookupRequest> Req = decodeLookupRequest(Body))
      return handleLookup(*Req);
    return std::nullopt;
  case MessageKind::PushRequest:
    // The metadata is checked whole once, here, outside the lock; every
    // compile after reads it as stored. Metadata that is not well formed
    // must not be offered: a candidate missing constraints matches compiles
    // it must not.
    if (std::optional<ReceivedPush> Req = decodePushRequest(Body))
      if (std::optional<MetadataView> V = MetadataView::open(Req->Metadata))
        if (V->validate())
          return handlePush(*Req);
    return std::nullopt;
  case MessageKind::TimingReport:
    if (std::optional<TimingReport> Report = decodeTimingReport(Body))
      return handleTiming(*Report);
    return std::nullopt;
  case MessageKind::Hello:
    return handleHello();
  case MessageKind::LookupResponse:
  case MessageKind::PushAck:
  case MessageKind::TimingAck:
  case MessageKind::HelloReply:
    // Reply kinds; a client sending one is talking nonsense.
    return std::nullopt;
  }
  return std::nullopt;
}

CacheDaemon::Stats CacheDaemon::stats() const {
  std::lock_guard<std::mutex> Lock(Mutex);
  Stats S = Counters;
  return S;
}

void CacheDaemon::stop() {
  Stopping.store(true, std::memory_order_release);
  if (auto *L = static_cast<llvm::ListeningSocket *>(Listener))
    L->shutdown();
}

llvm::Error CacheDaemon::run(llvm::StringRef SocketPath) {
  if (llvm::Error E = checkSocketPath(SocketPath))
    return E;
  // A socket file left behind by a previous run would make bind fail.
  llvm::sys::fs::remove(SocketPath);

  llvm::Expected<llvm::ListeningSocket> L =
      llvm::ListeningSocket::createUnix(SocketPath);
  if (!L)
    return L.takeError();
  Listener = &*L;

  while (!Stopping.load(std::memory_order_acquire)) {
    llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> Conn = L->accept();
    if (!Conn) {
      llvm::consumeError(Conn.takeError());
      // A failed accept during shutdown is expected; otherwise keep serving.
      continue;
    }

    // A connection's start is the daemon accepting a socket and its end is
    // the socket closing -- there are no connect or disconnect messages.
    std::thread([this, S = std::move(*Conn)]() mutable {
      while (std::optional<std::string> Body = receiveMessage(*S)) {
        std::optional<llvm::SmallVector<char, 0>> Reply = handleMessage(*Body);
        if (!Reply)
          break;
        if (!sendMessage(*S, *Reply))
          break;
      }
    }).detach();
  }

  Listener = nullptr;
  L->shutdown();
  return llvm::Error::success();
}
