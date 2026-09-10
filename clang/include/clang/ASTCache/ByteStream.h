//===- ByteStream.h --------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Little-endian read/write primitives shared by the dependency-record encoding
// (DependencySerialization.h) and the daemon protocol (CacheProtocol.h), which
// nest: a protocol message carries an encoded record.
//
// ByteReader never runs past its buffer. Once any read fails the reader stays
// failed and every later read returns zero, so a caller decodes a whole
// structure and checks ok() once at the end rather than after every field.
// That matters more than it looks: a decoder that stops at the first bad field
// and returns what it has produces a *partial* record, and a partial record
// carries fewer constraints than the compile it came from -- which makes it
// match compiles it must not.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ASTCACHE_BYTESTREAM_H
#define LLVM_CLANG_ASTCACHE_BYTESTREAM_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include <cstdint>

namespace clang {
namespace ast_cache {

class ByteWriter {
public:
  explicit ByteWriter(llvm::SmallVectorImpl<char> &Out) : Out(Out) {}

  void u8(uint8_t V) { Out.push_back(static_cast<char>(V)); }

  void u32(uint32_t V) {
    char Buf[4];
    llvm::support::endian::write32le(Buf, V);
    Out.append(Buf, Buf + 4);
  }

  void u64(uint64_t V) {
    char Buf[8];
    llvm::support::endian::write64le(Buf, V);
    Out.append(Buf, Buf + 8);
  }

  /// Length-prefixed, never NUL-terminated: a value dependency's token
  /// sequence contains embedded NULs by construction -- they are its token
  /// separator -- so a NUL-terminated encoding would truncate every one of
  /// them to its first token.
  void str(llvm::StringRef S) {
    u32(static_cast<uint32_t>(S.size()));
    Out.append(S.begin(), S.end());
  }

private:
  llvm::SmallVectorImpl<char> &Out;
};

class ByteReader {
public:
  explicit ByteReader(llvm::StringRef Data) : Data(Data) {}

  bool ok() const { return !Failed; }
  bool atEnd() const { return !Failed && Pos == Data.size(); }

  uint8_t u8() {
    if (Failed || Data.size() - Pos < 1) {
      Failed = true;
      return 0;
    }
    return static_cast<uint8_t>(Data[Pos++]);
  }

  uint32_t u32() {
    if (Failed || Data.size() - Pos < 4) {
      Failed = true;
      return 0;
    }
    uint32_t V = llvm::support::endian::read32le(Data.data() + Pos);
    Pos += 4;
    return V;
  }

  uint64_t u64() {
    if (Failed || Data.size() - Pos < 8) {
      Failed = true;
      return 0;
    }
    uint64_t V = llvm::support::endian::read64le(Data.data() + Pos);
    Pos += 8;
    return V;
  }

  llvm::StringRef str() {
    uint32_t Len = u32();
    if (Failed || Data.size() - Pos < Len) {
      Failed = true;
      return {};
    }
    llvm::StringRef S = Data.substr(Pos, Len);
    Pos += Len;
    return S;
  }

  /// A count read from the buffer sizes a loop, so a corrupt one would
  /// otherwise reserve an enormous vector before the first failed element
  /// read. No element encodes in fewer than one byte, so a count larger than
  /// what remains cannot be honest.
  uint32_t count() {
    uint32_t N = u32();
    if (Failed || N > Data.size() - Pos) {
      Failed = true;
      return 0;
    }
    return N;
  }

private:
  llvm::StringRef Data;
  size_t Pos = 0;
  bool Failed = false;
};

} // namespace ast_cache
} // namespace clang

#endif // LLVM_CLANG_ASTCACHE_BYTESTREAM_H
