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
#include <chrono>
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
  for (uint64_t U : Used) {
    LastUsed[U] = ++UseClock;
    Usage &X = Usages[U];
    uint64_t C = Counters.TimingReports;
    ++X.Uses;
    X.First = std::min(X.First, C);
    X.Last = std::max(X.Last, C);
    if (X.LastCompile != C) {
      ++X.Compiles;
      X.LastCompile = C;
    }
  }
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

void CacheDaemon::dumpEntries(llvm::raw_ostream &OS) const {
  std::lock_guard<std::mutex> Lock(Mutex);
  for (const auto &[Path, Count] : LookupsOf)
    OS << "header\t" << Path << "\t" << Count << "\t"
       << (Table.count(Path) ? Table.find(Path)->second.size() : 0) << "\n";
  for (const auto &[Path, Entries] : Table)
    for (const StoredEntry &E : Entries) {
      Usage U = Usages.lookup(E.MetadataOffset);
      std::optional<MetadataView> M =
          MetadataView::open(Region.read(E.MetadataOffset, E.MetadataLength));
      OS << "entry\t" << Path << "\t" << (M ? M->moduleName() : "") << "\t"
         << E.Length << "\t" << E.MetadataLength << "\t" << U.PushedAt
         << "\t" << U.Uses << "\t" << U.Compiles << "\t"
         << (U.Uses ? U.First : 0) << "\t" << U.Last << "\t";
      if (M)
        OS << M->conditional().size() << "," << M->expandsTo().size() << ","
           << M->notAMacro().size() << "," << M->symbol().size() << ","
           << M->inclusion().size() << "," << M->files().size() << ","
           << M->includes().size() << "," << M->declarations().size() << ","
           << M->macroEffects().size() << "," << M->links().size();
      OS << "\t";
      if (M) {
        bool First = true;
        for (const ConditionalView &C : M->conditional()) {
          OS << (First ? "" : ",") << C.MacroName << (C.WasDefined ? "+" : "-");
          First = false;
        }
      }
      OS << "\n";
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

void CacheDaemon::endWritingLocked(uint64_t Conn, llvm::StringRef Filename) {
  auto End = [&](Writers &W) {
    size_t Before = W.List.size();
    llvm::erase_if(W.List, [&](const Writer &X) { return X.Conn == Conn; });
    if (W.List.size() != Before)
      W.Arrived.notify_all();
  };
  if (!Filename.empty()) {
    if (auto It = WritersOf.find(Filename); It != WritersOf.end())
      End(*It->second);
    return;
  }
  for (auto &KV : WritersOf)
    End(*KV.second);
}

void CacheDaemon::settleWaitLocked(uint64_t Conn,
                                   const std::vector<uint64_t> &Used) {
  auto It = WaitedFor.find(Conn);
  if (It == WaitedFor.end())
    return;
  WaitHistory &H = WaitsOf[It->second.first];
  bool Fit = llvm::any_of(It->second.second, [&](uint64_t Offset) {
    return llvm::is_contained(Used, Offset);
  });
  ++(Fit ? H.Fit : H.Missed);
  WaitedFor.erase(It);
}

bool CacheDaemon::waitForWriterLocked(std::unique_lock<std::mutex> &Lock,
                                      const LookupRequest &Req,
                                      uint64_t Conn) {
  auto W = WritersOf.find(Req.Filename);
  if (W == WritersOf.end())
    return false;
  Writers *Ws = W->second.get();
  auto Other = [&] {
    return llvm::any_of(Ws->List, [&](const Writer &X) {
      return X.ClientId != Req.ClientId;
    });
  };
  if (!Other())
    return false;
  // The gate: once a few waits for this header ended in a copy the waiting
  // compile could not use, its copies differ from compile to compile, and
  // parsing beats waiting.
  if (auto H = WaitsOf.find(Req.Filename); H != WaitsOf.end()) {
    unsigned Checked = H->second.Fit + H->second.Missed;
    if (Checked >= 3 && 2 * H->second.Fit < Checked) {
      ++Counters.WaitsGated;
      return false;
    }
  }
  // A writer's copy arrives once it has parsed the header and written the
  // copy; it began before this compile asked, so waiting for it ends sooner
  // than parsing and writing here would. Twice the header's parse time,
  // from the earliest writer's start, covers both, unless it stalled.
  auto Parse = FirstTiming.find(Req.Filename);
  if (Parse == FirstTiming.end() || !Parse->second)
    return false;
  auto Stored = [&] {
    auto T = Table.find(Req.Filename);
    return T == Table.end() ? size_t(0) : T->second.size();
  };
  size_t Before = Stored();
  auto Start = std::chrono::steady_clock::now();
  auto Earliest = Start;
  for (const Writer &X : Ws->List)
    if (X.ClientId != Req.ClientId)
      Earliest = std::min(Earliest, X.Since);
  auto Deadline =
      std::max(Start + std::chrono::milliseconds(1),
               Earliest + 2 * std::chrono::microseconds(Parse->second));
  ++Counters.Waits;
  while (Other() && Stored() == Before)
    if (Ws->Arrived.wait_until(Lock, Deadline) == std::cv_status::timeout) {
      ++Counters.WaitsTimedOut;
      break;
    }
  if (size_t After = Stored(); After != Before) {
    ++Counters.WaitsServed;
    std::vector<uint64_t> New;
    const std::vector<StoredEntry> &Entries = Table.find(Req.Filename)->second;
    for (size_t I = Before; I != After; ++I)
      New.push_back(Entries[I].MetadataOffset);
    WaitedFor[Conn] = {Req.Filename, std::move(New)};
  }
  Counters.WaitMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - Start)
                             .count();
  return true;
}

llvm::SmallVector<char, 0>
CacheDaemon::handleLookup(const LookupRequest &Req, uint64_t Conn) {
  LookupResponse Resp;
  {
    std::unique_lock<std::mutex> Lock(Mutex);
    settleWaitLocked(Conn, Req.Used);
    learnLocked(Req.Build, Req.Learned, Req.Used);
    ++Counters.Lookups;
    ++LookupsOf[Req.Filename];
    // CLANG_HEADER_CACHE_WAIT: a copy another compile is writing may serve
    // this one; the reply then holds it.
    if (Req.Wait)
      Resp.Waited = waitForWriterLocked(Lock, Req, Conn);
    // Measurement (CLANG_HEADER_CACHE_PROBE_REUSE): "*<suffix>" asks for
    // the entries of every name ending in the suffix.
    std::vector<StoredEntry> Matching;
    llvm::StringRef Suffix;
    if (Req.Filename.size() > 1 && Req.Filename[0] == '*') {
      Suffix = llvm::StringRef(Req.Filename).drop_front();
      for (const auto &KV : Table)
        if (KV.getKey().ends_with(Suffix))
          llvm::append_range(Matching, KV.getValue());
    }
    auto It = Table.find(Req.Filename);
    if (!Suffix.empty()) {
      for (const StoredEntry &E : Matching)
        Resp.Entries.push_back({E.MetadataOffset, E.MetadataLength, E.Offset,
                                E.Length, std::vector<SearchOutcome>()});
    } else if (It != Table.end()) {
      ++Counters.Hits;
      // Where every stored entry for the name is, with no comparison of its
      // metadata here. The daemon has no way to evaluate a dependency --
      // that needs the client's live macro table and its Sema -- and sending
      // the client's whole state here instead would be far larger than the
      // handful of names a header actually depends on.
      // The most recently used first: a compile checks them in order and
      // takes the first that fits, which is most often the one that fit
      // the compile before it (REPORT.md I15).
      auto Recent = [&](const std::vector<StoredEntry> &Stored,
                        std::vector<CacheEntryRef> &Out) {
        std::vector<const StoredEntry *> Order;
        Order.reserve(Stored.size());
        for (const StoredEntry &E : Stored)
          Order.push_back(&E);
        std::stable_sort(Order.begin(), Order.end(),
                         [&](const StoredEntry *A, const StoredEntry *B) {
                           return LastUsed.lookup(A->MetadataOffset) >
                                  LastUsed.lookup(B->MetadataOffset);
                         });
        Out.reserve(Order.size());
        for (const StoredEntry *E : Order)
          Out.push_back({E->MetadataOffset, E->MetadataLength, E->Offset,
                         E->Length,
                         Req.Build && E->SearchBuild == Req.Build
                             ? E->Searches
                             : std::vector<SearchOutcome>()});
      };
      Recent(It->second, Resp.Entries);
      // CLANG_HEADER_CACHE_PIECES: the versions of its copies' sections, in
      // the same reply.
      if (auto V = VersionsOf.find(Req.Filename); V != VersionsOf.end())
        for (const std::string &Name : V->second) {
          auto Stored = Table.find(Name);
          if (Stored == Table.end())
            continue;
          LookupResponse::SectionVersions &Out = Resp.Versions.emplace_back();
          Out.Section = Name.substr(Req.Filename.size() + 9);
          Recent(Stored->second, Out.Entries);
        }
    }
  }

  llvm::SmallVector<char, 0> Out;
  encodeLookupResponse(Resp, Out);
  return Out;
}

llvm::SmallVector<char, 0> CacheDaemon::handlePush(const ReceivedPush &Req,
                                                   uint64_t Conn) {
  uint64_t Hash = llvm::xxh3_64bits(Req.Metadata);
  std::optional<MetadataView> V = MetadataView::open(Req.Metadata);
  std::string Name = V ? V->moduleName().str() : std::string();
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    // Whether stored or a duplicate, a copy of this header now exists: the
    // pushing compile's writing ends, and a claim on the copy is settled.
    auto Done = [&] {
      if (!Name.empty()) {
        PushedNames.insert(Name);
        Claimed.erase(Name);
      }
      endWritingLocked(Conn, Req.Filename);
    };
    ++Counters.Pushes;
    auto [Slot, NewName] = Table.try_emplace(Req.Filename);
    std::vector<StoredEntry> &Entries = Slot->second;
    // CLANG_HEADER_CACHE_PIECES: a version of a section, kept with the
    // header whose copy holds the section.
    if (NewName) {
      size_t At = Req.Filename.find("#section:");
      if (At != std::string::npos)
        VersionsOf[Req.Filename.take_front(At)].push_back(
            Req.Filename.str());
    }

    for (const StoredEntry &E : Entries) {
      if (E.Hash == Hash &&
          Region.read(E.MetadataOffset, E.MetadataLength) == Req.Metadata) {
        // Another client stored this exact result while this one was still
        // compiling. Discard it and acknowledge: the client that lost the
        // race already holds its own result and has no use for the winner.
        ++Counters.Duplicates;
        Done();
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
      Usages[*MetadataAt].PushedAt = Counters.TimingReports;
      Entries.push_back(std::move(E));
      Counters.BytesStored += Req.Data.size() + Req.Metadata.size();
      Counters.PartBytes += Req.Data.size();
      Counters.MetadataBytes += Req.Metadata.size();
      ++Counters.Entries;
      Counters.Headers = Table.size();
      if (Opts.ListStored && listLocked(Req.Filename))
        publishLocked();
    }
    Done();
    // A full region is not an error the client can act on -- it already has
    // its own result -- so a failed append is acknowledged like any other
    // push and the entry is simply not recorded.
  }

  llvm::SmallVector<char, 0> Ack;
  encodePushAck(Ack);
  return Ack;
}

llvm::SmallVector<char, 0>
CacheDaemon::handleNotice(const WritingNotice &Notice, uint64_t Conn) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Notice.Ended) {
      endWritingLocked(Conn, Notice.Filename);
    } else {
      std::unique_ptr<Writers> &W = WritersOf[Notice.Filename];
      if (!W)
        W = std::make_unique<Writers>();
      W->List.push_back(
          {Conn, Notice.ClientId, std::chrono::steady_clock::now()});
    }
  }
  llvm::SmallVector<char, 0> Ack;
  encodeNoticeAck(Ack);
  return Ack;
}

llvm::SmallVector<char, 0> CacheDaemon::handleClaim(const ClaimRequest &Req,
                                                    uint64_t Conn) {
  bool Granted = true;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Req.Release) {
      if (auto It = Claimed.find(Req.ModuleName);
          It != Claimed.end() && It->second == Conn)
        Claimed.erase(It);
    } else if (PushedNames.contains(Req.ModuleName) ||
               Claimed.contains(Req.ModuleName)) {
      Granted = false;
      ++Counters.ClaimsRefused;
    } else {
      Claimed[Req.ModuleName] = Conn;
      ++Counters.ClaimsGranted;
    }
  }
  llvm::SmallVector<char, 0> Out;
  encodeClaimReply(Granted, Out);
  return Out;
}

void CacheDaemon::connectionClosed(uint64_t Conn) {
  std::lock_guard<std::mutex> Lock(Mutex);
  endWritingLocked(Conn, /*Filename=*/{});
  // A claim still held when its compile ends is for a copy it wrote: one
  // declined is given back. Claims by the whole dependency list
  // (CLANG_HEADER_CACHE_CLAIM=deps) are not module names, which pushes mark.
  for (auto It = Claimed.begin(); It != Claimed.end();) {
    auto Next = std::next(It);
    if (It->second == Conn) {
      PushedNames.insert(It->getKey());
      Claimed.erase(It);
    }
    It = Next;
  }
  WaitedFor.erase(Conn);
}

std::optional<llvm::SmallVector<char, 0>>
CacheDaemon::handleMessage(llvm::StringRef Body, uint64_t Conn) {
  std::optional<MessageKind> Kind = peekMessageKind(Body);
  if (!Kind)
    return std::nullopt;

  switch (*Kind) {
  case MessageKind::LookupRequest:
    if (std::optional<LookupRequest> Req = decodeLookupRequest(Body))
      return handleLookup(*Req, Conn);
    return std::nullopt;
  case MessageKind::WritingNotice:
    if (std::optional<WritingNotice> Notice = decodeWritingNotice(Body))
      return handleNotice(*Notice, Conn);
    return std::nullopt;
  case MessageKind::ClaimRequest:
    if (std::optional<ClaimRequest> Req = decodeClaimRequest(Body))
      return handleClaim(*Req, Conn);
    return std::nullopt;
  case MessageKind::PushRequest:
    // The metadata is checked whole once, here, outside the lock; every
    // compile after reads it as stored. Metadata that is not well formed
    // must not be offered: a candidate missing constraints matches compiles
    // it must not.
    if (std::optional<ReceivedPush> Req = decodePushRequest(Body))
      if (std::optional<MetadataView> V = MetadataView::open(Req->Metadata))
        if (V->validate())
          return handlePush(*Req, Conn);
    return std::nullopt;
  case MessageKind::TimingReport:
    if (std::optional<TimingReport> Report = decodeTimingReport(Body)) {
      {
        std::lock_guard<std::mutex> Lock(Mutex);
        settleWaitLocked(Conn, Report->Used);
      }
      return handleTiming(*Report);
    }
    return std::nullopt;
  case MessageKind::Hello:
    return handleHello();
  case MessageKind::SeenRequest:
    if (std::optional<SeenRequest> Req = decodeSeenRequest(Body)) {
      bool Seen;
      {
        std::lock_guard<std::mutex> Lock(Mutex);
        Seen = !SeenOnce.insert(Req->Filename + '\0' + Req->ModuleName).second;
        ++(Seen ? Counters.SeenAgain : Counters.SeenFirst);
      }
      llvm::SmallVector<char, 0> Out;
      encodeSeenReply(Seen, Out);
      return Out;
    }
    return std::nullopt;
  case MessageKind::SeenReply:
  case MessageKind::NoticeAck:
  case MessageKind::ClaimReply:
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

  uint64_t Connections = 0;
  while (!Stopping.load(std::memory_order_acquire)) {
    llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> Conn = L->accept();
    if (!Conn) {
      llvm::consumeError(Conn.takeError());
      // A failed accept during shutdown is expected; otherwise keep serving.
      continue;
    }

    // A connection's start is the daemon accepting a socket and its end is
    // the socket closing -- there are no connect or disconnect messages.
    std::thread([this, S = std::move(*Conn), Id = ++Connections]() mutable {
      while (std::optional<std::string> Body = receiveMessage(*S)) {
        std::optional<llvm::SmallVector<char, 0>> Reply =
            handleMessage(*Body, Id);
        if (!Reply)
          break;
        if (!sendMessage(*S, *Reply))
          break;
      }
      connectionClosed(Id);
    }).detach();
  }

  Listener = nullptr;
  L->shutdown();
  return llvm::Error::success();
}
