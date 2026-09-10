//===- CacheDaemon.cpp ------------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_socket_stream.h"
#include <thread>

using namespace clang;
using namespace clang::ast_cache;

llvm::Expected<std::unique_ptr<CacheDaemon>>
CacheDaemon::create(llvm::StringRef RegionPath, uint64_t RegionSize) {
  llvm::Expected<SharedRegion> R = SharedRegion::create(RegionPath, RegionSize);
  if (!R)
    return R.takeError();

  std::unique_ptr<CacheDaemon> D(new CacheDaemon());
  D->Region = std::move(*R);
  return D;
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
        Resp.Entries.push_back({E.Deps, E.Offset, E.Length});
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

  {
    std::lock_guard<std::mutex> Lock(Mutex);
    ++Counters.Pushes;
    std::vector<StoredEntry> &Entries = Table[Req.Filename];

    for (const StoredEntry &E : Entries) {
      if (E.EncodedDeps == EncodedRef) {
        // Another client stored this exact result while this one was still
        // compiling. Discard it and acknowledge: the client that lost the
        // race already holds its own result and has no use for the winner.
        ++Counters.Duplicates;
        llvm::SmallVector<char, 0> Ack;
        encodePushAck(Ack);
        return Ack;
      }
    }

    if (std::optional<uint64_t> Offset = Region.append(Req.Data)) {
      StoredEntry E;
      E.Deps = Req.Deps;
      E.EncodedDeps = EncodedRef.str();
      E.Offset = *Offset;
      E.Length = Req.Data.size();
      Entries.push_back(std::move(E));
      Counters.BytesStored += Req.Data.size();
      ++Counters.Entries;
      Counters.Headers = Table.size();
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
  case MessageKind::LookupResponse:
  case MessageKind::PushAck:
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
