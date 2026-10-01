//===- SharedRegion.cpp -----------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/SharedRegion.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include <atomic>
#include <cstring>
#include <random>

using namespace clang;
using namespace clang::ast_cache;

namespace {

constexpr uint64_t RegionMagic = 0x4348444352474E31ull; // "CHDCRGN1"

/// Sits at offset 0 of the region. Only Used and AskList change after
/// creation, and only the daemon changes them.
struct RegionHeader {
  uint64_t Magic;
  uint64_t InstanceId;
  std::atomic<uint64_t> Used;
  /// Offset of the current ask-list: an 8-byte little-endian length, then the
  /// list. Zero before the first list is published.
  std::atomic<uint64_t> AskList;
};

/// Entries start after the header, aligned so a stored parsed header begins
/// on a boundary the AST reader is happy to read from directly.
constexpr uint64_t DataStart = 4096;
static_assert(sizeof(RegionHeader) <= DataStart, "header must fit");

uint64_t alignUp(uint64_t V, uint64_t A) { return (V + A - 1) & ~(A - 1); }

} // namespace

llvm::Expected<SharedRegion> SharedRegion::create(llvm::StringRef Path,
                                                  uint64_t Size) {
  if (Size <= DataStart)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "region size too small");

  // Unlink first. A fresh instance must not append over data that a client
  // surviving from a previous instance still has mapped; unlinking gives this
  // instance a genuinely new file and leaves that client's mapping orphaned
  // but intact.
  llvm::sys::fs::remove(Path);

  int FD = -1;
  if (std::error_code EC = llvm::sys::fs::openFileForReadWrite(
          Path, FD, llvm::sys::fs::CD_CreateAlways, llvm::sys::fs::OF_None))
    return llvm::createStringError(EC, "creating shared region '%s'",
                                   Path.str().c_str());

  if (std::error_code EC = llvm::sys::fs::resize_file(FD, Size)) {
    llvm::sys::fs::closeFile(FD);
    return llvm::createStringError(EC, "sizing shared region");
  }

  std::error_code EC;
  SharedRegion R;
  R.Mapping = llvm::sys::fs::mapped_file_region(
      llvm::sys::fs::convertFDToNativeFile(FD),
      llvm::sys::fs::mapped_file_region::readwrite, Size, 0, EC);
  llvm::sys::fs::closeFile(FD);
  if (EC)
    return llvm::createStringError(EC, "mapping shared region");

  R.Path = Path.str();

  auto *H = reinterpret_cast<RegionHeader *>(R.Mapping.data());
  H->Magic = RegionMagic;
  std::random_device RD;
  H->InstanceId = (static_cast<uint64_t>(RD()) << 32) | RD();
  H->AskList.store(0, std::memory_order_relaxed);
  H->Used.store(DataStart, std::memory_order_release);
  return R;
}

llvm::Expected<SharedRegion> SharedRegion::attach(llvm::StringRef Path) {
  int FD = -1;
  if (std::error_code EC = llvm::sys::fs::openFileForReadWrite(
          Path, FD, llvm::sys::fs::CD_OpenExisting, llvm::sys::fs::OF_None))
    return llvm::createStringError(EC, "attaching to shared region '%s'",
                                   Path.str().c_str());

  uint64_t Size = 0;
  if (std::error_code EC = llvm::sys::fs::file_size(Path, Size)) {
    llvm::sys::fs::closeFile(FD);
    return llvm::createStringError(EC, "sizing shared region");
  }

  std::error_code EC;
  SharedRegion R;
  // readwrite, not readonly: LLVM maps readonly as MAP_PRIVATE, which would
  // not reliably show appends made after this client attached. Nothing here
  // writes.
  R.Mapping = llvm::sys::fs::mapped_file_region(
      llvm::sys::fs::convertFDToNativeFile(FD),
      llvm::sys::fs::mapped_file_region::readwrite, Size, 0, EC);
  llvm::sys::fs::closeFile(FD);
  if (EC)
    return llvm::createStringError(EC, "mapping shared region");

  const auto *H = reinterpret_cast<const RegionHeader *>(R.Mapping.const_data());
  if (H->Magic != RegionMagic)
    return llvm::createStringError(llvm::errc::illegal_byte_sequence,
                                   "'%s' is not a header cache region",
                                   Path.str().c_str());

  R.Path = Path.str();
  return R;
}

std::optional<uint64_t> SharedRegion::append(llvm::StringRef Data) {
  if (!valid())
    return std::nullopt;
  auto *H = reinterpret_cast<RegionHeader *>(Mapping.data());
  uint64_t Offset = alignUp(H->Used.load(std::memory_order_relaxed), 8);
  if (Offset > Mapping.size() || Data.size() > Mapping.size() - Offset)
    return std::nullopt;

  std::memcpy(Mapping.data() + Offset, Data.data(), Data.size());
  // Release: the bytes must be visible to anyone who later observes the new
  // cursor. In practice the socket reply carrying the offset is a stronger
  // barrier still, but the ordering should not depend on that.
  H->Used.store(Offset + Data.size(), std::memory_order_release);
  return Offset;
}

llvm::StringRef SharedRegion::read(uint64_t Offset, uint64_t Length) const {
  if (!valid())
    return {};
  const auto *H = reinterpret_cast<const RegionHeader *>(Mapping.const_data());
  uint64_t Used = H->Used.load(std::memory_order_acquire);
  if (Offset < DataStart || Offset > Used || Length > Used - Offset)
    return {};
  return llvm::StringRef(Mapping.const_data() + Offset, Length);
}

uint64_t SharedRegion::instanceId() const {
  if (!valid())
    return 0;
  return reinterpret_cast<const RegionHeader *>(Mapping.const_data())
      ->InstanceId;
}

uint64_t SharedRegion::bytesUsed() const {
  if (!valid())
    return 0;
  return reinterpret_cast<const RegionHeader *>(Mapping.const_data())
      ->Used.load(std::memory_order_acquire);
}

bool SharedRegion::publishAskList(llvm::StringRef List) {
  if (!valid())
    return false;
  std::string Blob(8, '\0');
  uint64_t Len = List.size();
  for (unsigned I = 0; I != 8; ++I)
    Blob[I] = static_cast<char>((Len >> (8 * I)) & 0xff);
  Blob += List;
  std::optional<uint64_t> Offset = append(Blob);
  if (!Offset)
    return false;
  // Release: a reader that sees the new offset also sees the list behind it.
  reinterpret_cast<RegionHeader *>(Mapping.data())
      ->AskList.store(*Offset, std::memory_order_release);
  return true;
}

llvm::StringRef SharedRegion::askList(uint64_t *Version) const {
  if (Version)
    *Version = 0;
  if (!valid())
    return {};
  uint64_t Offset = reinterpret_cast<const RegionHeader *>(Mapping.const_data())
                        ->AskList.load(std::memory_order_acquire);
  if (Version)
    *Version = Offset;
  if (Offset == 0)
    return {};
  llvm::StringRef LenBytes = read(Offset, 8);
  if (LenBytes.size() != 8)
    return {};
  uint64_t Len = 0;
  for (unsigned I = 0; I != 8; ++I)
    Len |= static_cast<uint64_t>(static_cast<uint8_t>(LenBytes[I])) << (8 * I);
  return read(Offset + 8, Len);
}
