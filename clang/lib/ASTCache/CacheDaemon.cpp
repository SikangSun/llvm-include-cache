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
  if (!ListedSet.insert(Path).second)
    return false;
  Listed.push_back(Path.str());
  Counters.HeadersListed = Listed.size();
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

llvm::SmallVector<char, 0>
CacheDaemon::handleTiming(const TimingReport &Report) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    ++Counters.TimingReports;
    bool Changed = false;
    for (const HeaderTiming &H : Report.Headers) {
      if (H.Path.empty() || !FirstTiming.try_emplace(H.Path, H.Micros).second)
        continue;
      if (H.Micros >= Opts.AskCutoffMicros)
        Changed |= listLocked(H.Path);
    }
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
    ++Counters.Lookups;
    auto It = Table.find(Req.Filename);
    if (It != Table.end()) {
      ++Counters.Hits;
      // Every stored entry for the name is returned, with no comparison of
      // its record here. The daemon has no way to evaluate a dependency --
      // that needs the client's live macro table and its Sema -- and sending
      // the client's whole state here instead would be far larger than the
      // handful of names a header actually depends on.
      Resp.Entries.reserve(It->second.size());
      for (const StoredEntry &E : It->second)
        Resp.Entries.push_back({E.Deps, E.Offset, E.Length, E.NamesOffset,
                                E.NamesLength, E.EffectsOffset, E.EffectsLength,
                                E.ModuleName});
    }
  }

  llvm::SmallVector<char, 0> Out;
  encodeLookupResponse(Resp, Out);
  return Out;
}

llvm::SmallVector<char, 0> CacheDaemon::handlePush(const PushRequest &Req) {
  llvm::SmallVector<char, 256> Encoded;
  encodeDependencySet(Req.Deps, Encoded);
  llvm::StringRef EncodedRef(Encoded.data(), Encoded.size());

  // The names the record requires not to be macros are several hundred for a
  // system header. They go into the region beside the stored result, so a
  // lookup reply carries where they are instead of the names themselves.
  HeaderDependencySet Kept = Req.Deps;
  std::string Names = extractNotAMacroNames(Kept);
  // The macros the header leaves defined or undefined: thousands for a header
  // that pulls in a system header, and needed only on a hit.
  std::string Effects = extractMacroEffects(Kept);

  {
    std::lock_guard<std::mutex> Lock(Mutex);
    ++Counters.Pushes;
    std::vector<StoredEntry> &Entries = Table[Req.Filename];

    for (const StoredEntry &E : Entries) {
      if (E.EncodedDeps == EncodedRef && E.ModuleName == Req.ModuleName) {
        // Another client stored this exact result while this one was still
        // compiling. Discard it and acknowledge: the client that lost the
        // race already holds its own result and has no use for the winner.
        ++Counters.Duplicates;
        llvm::SmallVector<char, 0> Ack;
        encodePushAck(Ack);
        return Ack;
      }
    }

    // An entry is recorded only with its names and its effects. Kept without
    // the names, it would match compiles that define those names as macros;
    // without the effects, a hit would leave the macro table as if the header
    // had not been included.
    auto AppendList = [&](const std::string &L) {
      return L.empty() ? std::optional<uint64_t>(0) : Region.append(L);
    };
    std::optional<uint64_t> NamesAt = AppendList(Names);
    std::optional<uint64_t> EffectsAt =
        NamesAt ? AppendList(Effects) : std::nullopt;
    std::optional<uint64_t> Offset =
        EffectsAt ? Region.append(Req.Data) : std::nullopt;
    if (Offset) {
      StoredEntry E;
      E.Deps = std::move(Kept);
      E.EncodedDeps = EncodedRef.str();
      E.ModuleName = Req.ModuleName;
      E.Offset = *Offset;
      E.Length = Req.Data.size();
      E.NamesOffset = *NamesAt;
      E.NamesLength = Names.size();
      E.EffectsOffset = *EffectsAt;
      E.EffectsLength = Effects.size();
      Entries.push_back(std::move(E));
      Counters.BytesStored += Req.Data.size() + Names.size() + Effects.size();
      Counters.PartBytes += Req.Data.size();
      Counters.NameBytes += Names.size();
      Counters.EffectBytes += Effects.size();
      Counters.MetadataBytes += EncodedRef.size();
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
    if (std::optional<PushRequest> Req = decodePushRequest(Body))
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
