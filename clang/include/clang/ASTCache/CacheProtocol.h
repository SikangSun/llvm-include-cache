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
//     -> where each entry stored under that filename sits in the shared
//        region: its metadata and its saved copy. The reply carries no
//        metadata; the compile reads it in place there (MetadataView).
//
//   PUSH(filename, metadata, data)
//     -> a bare acknowledgment. The metadata crosses the socket once, in the
//        form it is stored and read in.
//
// The daemon never interprets metadata beyond checking it is well formed. It
// matches on filename only, hands back every candidate, and the client
// decides which -- if any -- of them its own compile matches. That keeps the
// daemon simple, and it avoids sending the client's entire macro table
// across the socket when a header depends on a handful of names.
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
#include "llvm/Support/Error.h"
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
  TimingReport = 5,
  TimingAck = 6,
  Hello = 7,
  HelloReply = 8,
};

/// What a copy's #include searches found in one build, under one search key
/// (REPORT.md I9): the check's MismatchReason, None if every search found
/// the file, and the name, the copy was made from. Within a build the files
/// in the include path's directories do not appear or go away, so the same
/// searches under the same key find the same: the include path, the place
/// the header was found, and the names the compile already reads the copy's
/// files by (CacheClient's matchesLiveState).
struct SearchOutcome {
  uint64_t Key = 0;
  uint8_t Outcome = 0;
};

/// A SearchOutcome a compile found, for the daemon to keep for the rest of
/// the build. The copy is the one stored for Filename whose metadata starts
/// at Entry (CacheEntryRef::MetadataOffset).
struct LearnedSearch {
  std::string Filename;
  uint64_t Entry = 0;
  SearchOutcome Found;
};

/// A stored copy the daemon knows about. Both ranges address the shared
/// region, not a file: the metadata, in encodeMetadata's form, and the saved
/// copy's PCM bytes.
struct CacheEntryRef {
  uint64_t MetadataOffset = 0;
  uint64_t MetadataLength = 0;
  uint64_t Offset = 0;
  uint64_t Length = 0;
  /// What the copy's searches found in the asking compile's build.
  std::vector<SearchOutcome> Searches;
};

struct LookupRequest {
  std::string Filename;
  /// The build the compile belongs to, 0 for none (CLANG_HEADER_CACHE_BUILD),
  /// and what its searches found since its last message.
  uint64_t Build = 0;
  std::vector<LearnedSearch> Learned;
  /// The copies, by metadata offset, the compile used since its last
  /// message: the daemon offers the most recently used first (REPORT.md
  /// I15).
  std::vector<uint64_t> Used;
};

struct LookupResponse {
  /// Empty means nothing is stored under that filename -- a clean miss, not
  /// an error.
  std::vector<CacheEntryRef> Entries;
};

struct PushRequest {
  std::string Filename;
  /// Names the build: the producer hashes the header, the compiler options
  /// and the context into it. Two pushes with the same metadata but
  /// different options are different entries -- the options are not part of
  /// the metadata -- so the name is stored inside the metadata, and the
  /// daemon's byte comparison for a duplicate compares it too.
  std::string ModuleName;
  /// The copy's metadata, encoded with ModuleName into the stored form as
  /// the push is sent.
  HeaderDependencySet Deps;
  /// The serialized parsed header. Copied into the shared region by the
  /// daemon, so it crosses the socket once.
  std::string Data;
};

/// A push as the daemon reads it: the metadata already in its stored form,
/// the module name inside it. Each field points into the message.
struct ReceivedPush {
  llvm::StringRef Filename;
  llvm::StringRef Metadata;
  llvm::StringRef Data;
};

/// How long a compile took to read one header, from entering it to leaving
/// it, headers it included on the way counted in (DECISIONS.md 2.14).
struct HeaderTiming {
  std::string Path;
  uint64_t Micros = 0;
};

/// Sent once when a compile's main file ends: every header the compile read,
/// with its time, so the daemon can list the slow ones; and every hit, with
/// what it cost -- the cache query and replaying the header's effects -- so
/// the daemon can drop a header whose hits cost more than they save.
struct TimingReport {
  std::vector<HeaderTiming> Headers;
  std::vector<HeaderTiming> Hits;
  /// As in LookupRequest: the compile's last searches and copies used.
  uint64_t Build = 0;
  std::vector<LearnedSearch> Learned;
  std::vector<uint64_t> Used;
};

/// The daemon's answer to a client that has just connected: the region file
/// its replies' offsets point into, and which instance of that file. A
/// client that mapped some other file -- one left behind by an earlier
/// daemon, or the default path while this daemon was started with another --
/// would read other bytes at those offsets, including the bytes it loads as
/// a saved copy.
struct HelloReply {
  std::string RegionPath;
  uint64_t InstanceId = 0;
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
void encodeTimingReport(const TimingReport &Report,
                        llvm::SmallVectorImpl<char> &Out);
void encodeTimingAck(llvm::SmallVectorImpl<char> &Out);
void encodeHello(llvm::SmallVectorImpl<char> &Out);
void encodeHelloReply(const HelloReply &Reply,
                      llvm::SmallVectorImpl<char> &Out);

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
std::optional<ReceivedPush> decodePushRequest(llvm::StringRef Body);
std::optional<TimingReport> decodeTimingReport(llvm::StringRef Body);
std::optional<HelloReply> decodeHelloReply(llvm::StringRef Body);

//===----------------------------------------------------------------------===//
// Framed transport over a connected socket.
//===----------------------------------------------------------------------===//

/// Writes \p Message -- already framed by one of the encoders -- and flushes.
/// Returns false if the stream reported an error.
bool sendMessage(llvm::raw_socket_stream &S, llvm::ArrayRef<char> Message);

/// Fails when \p Path does not fit in a Unix socket address, which holds 107
/// bytes on Linux and 103 on macOS. LLVM's socket classes cut a longer path
/// short without saying so: a daemon would listen at the shortened path, and
/// two long paths that share their first 107 bytes would name one socket.
/// Checked before every connect and bind.
llvm::Error checkSocketPath(llvm::StringRef Path);

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
