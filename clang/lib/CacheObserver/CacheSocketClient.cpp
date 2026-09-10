//===- CacheSocketClient.cpp -----------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CacheObserver/CacheSocketClient.h"
#include "llvm/Support/raw_socket_stream.h"

using namespace clang;
using namespace clang::cache_observer;

SocketCachePayloadSink::SocketCachePayloadSink(
    std::unique_ptr<llvm::raw_socket_stream> S)
    : Stream(std::move(S)) {}

SocketCachePayloadSink::~SocketCachePayloadSink() = default;

llvm::Expected<std::unique_ptr<SocketCachePayloadSink>>
SocketCachePayloadSink::connect(StringRef SocketPath) {
  llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> Stream =
      llvm::raw_socket_stream::createConnectedUnix(SocketPath);
  if (!Stream)
    return Stream.takeError();
  return std::unique_ptr<SocketCachePayloadSink>(
      new SocketCachePayloadSink(std::move(*Stream)));
}

void SocketCachePayloadSink::send(const HeaderCachePayload &Payload) {
  llvm::SmallVector<char, 0> Bytes = serialize(Payload);
  // Length-prefix each message so the daemon can frame a stream of
  // payloads sent over the same persistent connection.
  uint32_t Len = static_cast<uint32_t>(Bytes.size());
  Stream->write(reinterpret_cast<const char *>(&Len), sizeof(Len));
  Stream->write(Bytes.data(), Bytes.size());
  Stream->flush();
}
