//===- CacheProtocol.h -----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The compiler/daemon wire protocol. Two request types:
//
//   LOOKUP(filename)
//     -> every entry stored under that filename, each carrying the dependency
//        record it was built against and where its stored result sits in the
//        shared region. The names the record requires not to be macros --
//        most of it, by size -- are in the region too, and the reply says
//        where rather than carrying them.
//
//   PUSH(filename, record, data)
//     -> a bare acknowledgment.
//
// The daemon never interprets a record. It matches on filename only, hands
// back every candidate, and the client decides which -- if any -- of them its
// own compile matches. That keeps the daemon simple, and it avoids sending
// the client's entire macro table across the socket when a header depends on
// a handful of names.
//
// Every entry's shared-memory location is returned, not just the one that
// turns out to match: mapping is cheap, and it saves a second round trip once
// the client has chosen. Whether that beats an ask-then-fetch protocol is a
// question for measurement, not for argument.
//
// Framing is a 4-byte little-endian length followed by that many bytes, whose
// first byte is the message type. A length prefix is what lets a single
// connection carry a run of lookups followed, after an arbitrary delay, by a
// push -- the daemon has no other way to know where one message ends.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_CACHEPROTOCOL_H
#define LLVM_CLANG_ASTCACHE_CACHEPROTOCOL_H

#include "clang/ASTCache/DependencyRecord.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class raw_socket_stream;
}

namespace clang {
namespace ast_cache {

enum class MessageKind : uint8_t {
  LookupRequest = 1,
  LookupResponse = 2,
  PushRequest = 3,
  PushAck = 4,
};

/// A stored result the daemon knows about: what it was built against, and
/// where to find it. Offset and Length address the shared region, not a file.
struct CacheEntryRef {
  /// The record, less its NotAMacro syntactic dependencies.
  HeaderDependencySet Deps;
  uint64_t Offset = 0;
  uint64_t Length = 0;
  /// Where those NotAMacro names sit in the shared region, as a list built by
  /// extractNotAMacroNames. A zero length means the record has none.
  uint64_t NamesOffset = 0;
  uint64_t NamesLength = 0;
};

struct LookupRequest {
  std::string Filename;
};

struct LookupResponse {
  /// Empty means nothing is stored under that filename -- a clean miss, not
  /// an error.
  std::vector<CacheEntryRef> Entries;
};

struct PushRequest {
  std::string Filename;
  /// The whole record. The daemon moves its NotAMacro names into the region.
  HeaderDependencySet Deps;
  /// The serialized parsed header. Copied into the shared region by the
  /// daemon, so it crosses the socket once.
  std::string Data;
};

/// Largest message accepted, in bytes. A parsed header is the big payload
/// here and runs to a few megabytes; the cap exists so a corrupt length
/// prefix fails immediately instead of trying to allocate whatever 32 bits
/// happened to say.
constexpr uint32_t MaxMessageSize = 256u * 1024u * 1024u;

//===----------------------------------------------------------------------===//
// Encoding. Each produces a complete framed message, length prefix included.
//===----------------------------------------------------------------------===//

void encodeLookupRequest(const LookupRequest &Req,
                         llvm::SmallVectorImpl<char> &Out);
void encodeLookupResponse(const LookupResponse &Resp,
                          llvm::SmallVectorImpl<char> &Out);
void encodePushRequest(const PushRequest &Req,
                       llvm::SmallVectorImpl<char> &Out);
void encodePushAck(llvm::SmallVectorImpl<char> &Out);

//===----------------------------------------------------------------------===//
// Decoding. \p Body is one message with the length prefix already stripped;
// each returns std::nullopt on a wrong message kind or a malformed body,
// never a partly filled result.
//===----------------------------------------------------------------------===//

/// The message kind of \p Body, or std::nullopt if it is empty or names a
/// kind this build does not know.
std::optional<MessageKind> peekMessageKind(llvm::StringRef Body);

std::optional<LookupRequest> decodeLookupRequest(llvm::StringRef Body);
std::optional<LookupResponse> decodeLookupResponse(llvm::StringRef Body);
std::optional<PushRequest> decodePushRequest(llvm::StringRef Body);

//===----------------------------------------------------------------------===//
// Framed transport over a connected socket.
//===----------------------------------------------------------------------===//

/// Writes \p Message -- already framed by one of the encoders -- and flushes.
/// Returns false if the stream reported an error.
bool sendMessage(llvm::raw_socket_stream &S, llvm::ArrayRef<char> Message);

/// Reads exactly one message and returns its body with the length prefix
/// stripped. Returns std::nullopt when the peer closed the connection, when
/// the length prefix exceeds MaxMessageSize, or on a read error.
///
/// Blocks indefinitely by design. After issuing its lookups a client moves on
/// to code generation and may not touch the socket again for a long time;
/// that gap is ordinary, not a hung connection, and a thread parked on a read
/// costs nothing while it waits.
std::optional<std::string> receiveMessage(llvm::raw_socket_stream &S);

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_CACHEPROTOCOL_H
