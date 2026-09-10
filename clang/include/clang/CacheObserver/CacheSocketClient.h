//===- CacheSocketClient.h -------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CACHEOBSERVER_CACHESOCKETCLIENT_H
#define LLVM_CLANG_CACHEOBSERVER_CACHESOCKETCLIENT_H

#include "clang/CacheObserver/CacheWireFormat.h"
#include "clang/CacheObserver/TokenStreamCacheObserver.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <vector>

namespace llvm {
class raw_socket_stream;
}

namespace clang {
namespace cache_observer {

/// Production CachePayloadSink: serializes each payload and blasts it down
/// a persistent Unix-domain-socket connection to a caching daemon. The
/// connection is opened once and held for the lifetime of this object, so
/// it should be constructed once per translation unit (e.g. owned by the
/// driving FrontendAction/PluginASTAction) rather than per header.
class SocketCachePayloadSink : public CachePayloadSink {
public:
  ~SocketCachePayloadSink() override;

  static llvm::Expected<std::unique_ptr<SocketCachePayloadSink>>
  connect(StringRef SocketPath);

  void send(const HeaderCachePayload &Payload) override;

private:
  explicit SocketCachePayloadSink(std::unique_ptr<llvm::raw_socket_stream> S);

  std::unique_ptr<llvm::raw_socket_stream> Stream;
};

/// Testing CachePayloadSink: just accumulates every payload it receives.
class InMemoryCachePayloadSink : public CachePayloadSink {
public:
  void send(const HeaderCachePayload &Payload) override {
    Payloads.push_back(Payload);
  }

  const std::vector<HeaderCachePayload> &payloads() const { return Payloads; }

private:
  std::vector<HeaderCachePayload> Payloads;
};

} // namespace cache_observer
} // namespace clang

#endif // LLVM_CLANG_CACHEOBSERVER_CACHESOCKETCLIENT_H
