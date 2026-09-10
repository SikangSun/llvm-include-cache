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

void clang::ast_cache::encodeLookupRequest(const LookupRequest &Req,
                                           llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::LookupRequest,
        [&](ByteWriter &W) { W.str(Req.Filename); });
}

void clang::ast_cache::encodeLookupResponse(const LookupResponse &Resp,
                                            llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::LookupResponse, [&](ByteWriter &W) {
    W.u32(static_cast<uint32_t>(Resp.Entries.size()));
    for (const CacheEntryRef &E : Resp.Entries) {
      writeRecord(W, E.Deps);
      W.u64(E.Offset);
      W.u64(E.Length);
    }
  });
}

void clang::ast_cache::encodePushRequest(const PushRequest &Req,
                                         llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::PushRequest, [&](ByteWriter &W) {
    W.str(Req.Filename);
    writeRecord(W, Req.Deps);
    W.str(Req.Data);
  });
}

void clang::ast_cache::encodePushAck(llvm::SmallVectorImpl<char> &Out) {
  frame(Out, MessageKind::PushAck, [](ByteWriter &) {});
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
  if (!R.atEnd())
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
  if (!readRecord(R, Req.Deps))
    return std::nullopt;
  Req.Data = R.str().str();
  if (!R.atEnd())
    return std::nullopt;
  return Req;
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
