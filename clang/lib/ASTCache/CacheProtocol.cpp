//===- CacheProtocol.cpp ----------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/ByteStream.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "llvm/Support/raw_socket_stream.h"

#ifndef _WIN32
#include <sys/un.h>
#endif

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// Reserves the length prefix, lets \p Body write the payload, then patches
/// the prefix with what was actually written. Writing the length first would
/// mean encoding the payload twice, once to measure it.
template <typename Fn>
void frame(llvm::SmallVectorImpl<char> &Out, MessageKind Kind, Fn Body) {
  size_t PrefixAt = Out.size();
  ByteWriter W(Out);
  W.u32(0);
  W.u8(static_cast<uint8_t>(Kind));
  Body(W);
  llvm::support::endian::write32le(Out.data() + PrefixAt,
                                   static_cast<uint32_t>(Out.size() - PrefixAt -
                                                          sizeof(uint32_t)));
}

void writeRecord(ByteWriter &W, const HeaderDependencySet &Deps) {
  llvm::SmallVector<char, 256> Encoded;
  encodeDependencySet(Deps, Encoded);
  W.str(llvm::StringRef(Encoded.data(), Encoded.size()));
}

/// Returns false if the nested record could not be decoded, which fails the
/// whole message: an entry whose record is unreadable must not be offered as
/// a candidate, since a candidate with no constraints matches everything.
bool readRecord(ByteReader &R, HeaderDependencySet &Out) {
  llvm::StringRef Encoded = R.str();
  if (!R.ok())
    return false;
  std::optional<HeaderDependencySet> Deps = decodeDependencySet(Encoded);
  if (!Deps)
    return false;
  Out = std::move(*Deps);
  return true;
}

/// Strips and validates the kind byte, leaving \p R positioned at the body.
bool expectKind(ByteReader &R, MessageKind Want) {
  return R.u8() == static_cast<uint8_t>(Want) && R.ok();
}

} // namespace

//===----------------------------------------------------------------------===//
// Encoding
//===----------------------------------------------------------------------===//

static void writeLearned(ByteWriter &W, uint64_t Build,
                         const std::vector<LearnedSearch> &Learned) {
  W.u64(Build);
  W.u32(static_cast<uint32_t>(Learned.size()));
  for (const LearnedSearch &L : Learned) {
    W.str(L.Filename);
    W.str(L.ModuleName);
    W.u64(L.Found.Key);
    W.u8(L.Found.Outcome);
  }
}

static bool readLearned(ByteReader &R, uint64_t &Build,
                        std::vector<LearnedSearch> &Learned) {
  Build = R.u64();
  for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
    LearnedSearch L;
    L.Filename = R.str().str();
    L.ModuleName = R.str().str();
    L.Found.Key = R.u64();
    L.Found.Outcome = R.u8();
    Learned.push_back(std::move(L));
  }
  return R.ok();
}

void clang::ast_cache::encodeLookupRequest(const LookupRequest &Req,
                                           llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::LookupRequest, [&](ByteWriter &W) {
    W.str(Req.Filename);
    writeLearned(W, Req.Build, Req.Learned);
  });
}

void clang::ast_cache::encodeLookupResponse(const LookupResponse &Resp,
                                            llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::LookupResponse, [&](ByteWriter &W) {
    W.u32(static_cast<uint32_t>(Resp.Entries.size()));
    for (const CacheEntryRef &E : Resp.Entries) {
      writeRecord(W, E.Deps);
      W.u64(E.Offset);
      W.u64(E.Length);
      W.u64(E.NamesOffset);
      W.u64(E.NamesLength);
      W.u64(E.EffectsOffset);
      W.u64(E.EffectsLength);
      W.str(E.ModuleName);
      W.u32(static_cast<uint32_t>(E.Searches.size()));
      for (const SearchOutcome &O : E.Searches) {
        W.u64(O.Key);
        W.u8(O.Outcome);
      }
    }
  });
}

void clang::ast_cache::encodePushRequest(const PushRequest &Req,
                                         llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::PushRequest, [&](ByteWriter &W) {
    W.str(Req.Filename);
    W.str(Req.ModuleName);
    writeRecord(W, Req.Deps);
    W.str(Req.Data);
  });
}

void clang::ast_cache::encodePushAck(llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::PushAck, [](ByteWriter &) {});
}

void clang::ast_cache::encodeTimingReport(const TimingReport &Report,
                                          llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::TimingReport, [&](ByteWriter &W) {
    for (const std::vector<HeaderTiming> *L : {&Report.Headers, &Report.Hits}) {
      W.u32(static_cast<uint32_t>(L->size()));
      for (const HeaderTiming &H : *L) {
        W.str(H.Path);
        W.u64(H.Micros);
      }
    }
    writeLearned(W, Report.Build, Report.Learned);
  });
}

void clang::ast_cache::encodeTimingAck(llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::TimingAck, [](ByteWriter &) {});
}

void clang::ast_cache::encodeHello(llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::Hello, [](ByteWriter &) {});
}

void clang::ast_cache::encodeHelloReply(const HelloReply &Reply,
                                        llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::HelloReply, [&](ByteWriter &W) {
    W.str(Reply.RegionPath);
    W.u64(Reply.InstanceId);
  });
}

//===----------------------------------------------------------------------===//
// Decoding
//===----------------------------------------------------------------------===//

std::optional<MessageKind>
clang::ast_cache::peekMessageKind(llvm::StringRef Body) {
  if (Body.empty())
    return std::nullopt;
  switch (static_cast<uint8_t>(Body[0])) {
  case static_cast<uint8_t>(MessageKind::LookupRequest):
    return MessageKind::LookupRequest;
  case static_cast<uint8_t>(MessageKind::LookupResponse):
    return MessageKind::LookupResponse;
  case static_cast<uint8_t>(MessageKind::PushRequest):
    return MessageKind::PushRequest;
  case static_cast<uint8_t>(MessageKind::PushAck):
    return MessageKind::PushAck;
  case static_cast<uint8_t>(MessageKind::TimingReport):
    return MessageKind::TimingReport;
  case static_cast<uint8_t>(MessageKind::TimingAck):
    return MessageKind::TimingAck;
  case static_cast<uint8_t>(MessageKind::Hello):
    return MessageKind::Hello;
  case static_cast<uint8_t>(MessageKind::HelloReply):
    return MessageKind::HelloReply;
  default:
    return std::nullopt;
  }
}

std::optional<LookupRequest>
clang::ast_cache::decodeLookupRequest(llvm::StringRef Body) {
  ByteReader R(Body);
  if (!expectKind(R, MessageKind::LookupRequest))
    return std::nullopt;
  LookupRequest Req;
  Req.Filename = R.str().str();
  if (!readLearned(R, Req.Build, Req.Learned) || !R.atEnd())
    return std::nullopt;
  return Req;
}

std::optional<LookupResponse>
clang::ast_cache::decodeLookupResponse(llvm::StringRef Body) {
  ByteReader R(Body);
  if (!expectKind(R, MessageKind::LookupResponse))
    return std::nullopt;

  LookupResponse Resp;
  uint32_t N = R.count();
  for (uint32_t I = 0; I < N && R.ok(); ++I) {
    CacheEntryRef E;
    if (!readRecord(R, E.Deps))
      return std::nullopt;
    E.Offset = R.u64();
    E.Length = R.u64();
    E.NamesOffset = R.u64();
    E.NamesLength = R.u64();
    E.EffectsOffset = R.u64();
    E.EffectsLength = R.u64();
    E.ModuleName = R.str().str();
    for (uint32_t J = 0, M = R.count(); J < M && R.ok(); ++J) {
      SearchOutcome O;
      O.Key = R.u64();
      O.Outcome = R.u8();
      E.Searches.push_back(O);
    }
    Resp.Entries.push_back(std::move(E));
  }
  if (!R.atEnd())
    return std::nullopt;
  return Resp;
}

std::optional<PushRequest>
clang::ast_cache::decodePushRequest(llvm::StringRef Body) {
  ByteReader R(Body);
  if (!expectKind(R, MessageKind::PushRequest))
    return std::nullopt;

  PushRequest Req;
  Req.Filename = R.str().str();
  Req.ModuleName = R.str().str();
  if (!readRecord(R, Req.Deps))
    return std::nullopt;
  Req.Data = R.str().str();
  if (!R.atEnd())
    return std::nullopt;
  return Req;
}

std::optional<TimingReport>
clang::ast_cache::decodeTimingReport(llvm::StringRef Body) {
  ByteReader R(Body);
  if (!expectKind(R, MessageKind::TimingReport))
    return std::nullopt;
  TimingReport Report;
  for (std::vector<HeaderTiming> *L : {&Report.Headers, &Report.Hits}) {
    for (uint32_t I = 0, N = R.count(); I < N && R.ok(); ++I) {
      HeaderTiming H;
      H.Path = R.str().str();
      H.Micros = R.u64();
      L->push_back(std::move(H));
    }
  }
  if (!readLearned(R, Report.Build, Report.Learned) || !R.ok() || !R.atEnd())
    return std::nullopt;
  return Report;
}

std::optional<HelloReply>
clang::ast_cache::decodeHelloReply(llvm::StringRef Body) {
  ByteReader R(Body);
  if (!expectKind(R, MessageKind::HelloReply))
    return std::nullopt;
  HelloReply Reply;
  Reply.RegionPath = R.str().str();
  Reply.InstanceId = R.u64();
  if (!R.ok() || !R.atEnd() || Reply.RegionPath.empty())
    return std::nullopt;
  return Reply;
}

//===----------------------------------------------------------------------===//
// Transport
//===----------------------------------------------------------------------===//

bool clang::ast_cache::sendMessage(llvm::raw_socket_stream &S,
                                   llvm::ArrayRef<char> Message) {
  S.write(Message.data(), Message.size());
  S.flush();
  if (S.has_error()) {
    S.clear_error();
    return false;
  }
  return true;
}

std::optional<std::string>
clang::ast_cache::receiveMessage(llvm::raw_socket_stream &S) {
  // A socket read returns whatever has arrived, not what was asked for, so
  // both the prefix and the body are read in a loop. A large payload is
  // reliably split across several reads.
  auto readExactly = [&](char *Dest, size_t Size) -> bool {
    size_t Got = 0;
    while (Got < Size) {
      ssize_t N = S.read(Dest + Got, Size - Got);
      if (N <= 0)
        return false; // peer closed, or a read error
      Got += static_cast<size_t>(N);
    }
    return true;
  };

  char Prefix[4];
  if (!readExactly(Prefix, sizeof(Prefix)))
    return std::nullopt;

  uint32_t Len = llvm::support::endian::read32le(Prefix);
  if (Len == 0 || Len > MaxMessageSize)
    return std::nullopt;

  std::string Body;
  Body.resize(Len);
  if (!readExactly(Body.data(), Len))
    return std::nullopt;
  return Body;
}

llvm::Error clang::ast_cache::checkSocketPath(llvm::StringRef Path) {
#ifdef _WIN32
  // UNIX_PATH_MAX in afunix.h.
  constexpr size_t PathField = 108;
#else
  constexpr size_t PathField = sizeof(sockaddr_un::sun_path);
#endif
  // One byte of the field is the terminating NUL.
  if (Path.size() < PathField)
    return llvm::Error::success();
  return llvm::createStringError(
      llvm::inconvertibleErrorCode(),
      "socket path is " + llvm::Twine(Path.size()) +
          " bytes; a Unix socket address holds at most " +
          llvm::Twine(PathField - 1) + ": " + Path);
}
