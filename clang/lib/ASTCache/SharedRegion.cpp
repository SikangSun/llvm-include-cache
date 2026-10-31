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
#include "llvm/Support/xxhash.h"
#include <atomic>
#include <cstring>
#include <random>

using namespace clang;
using namespace clang::ast_cache;

namespace {

constexpr uint64_t RegionMagic = 0x4348444352474E34ull; // "CHDCRGN4"

/// Sits at offset 0 of the region. Only Used and AskList change after
/// creation, and only the daemon changes them.
struct RegionHeader {
  uint64_t Magic;
  uint64_t InstanceId;
  std::atomic<uint64_t> Used;
  /// Offset of the current ask-list: an 8-byte little-endian length, then the
  /// list. Zero before the first list is published.
  std::atomic<uint64_t> AskList;
  /// The index of copies: IndexSlots slots from IndexStart, a power of two;
  /// entries begin at DataStart, after it.
  uint64_t IndexSlots;
  uint64_t DataStart;
};

/// One header in the index of copies, found by the hash of its name
/// (Key, never 0; 0 marks a free slot): its name, an 8-byte length and
/// the bytes, and the record of its newest copy. The daemon sets Name and
/// Newest before Key, so a slot whose Key a reader sees is whole.
struct IndexSlot {
  std::atomic<uint64_t> Key;
  std::atomic<uint64_t> Name;
  std::atomic<uint64_t> Newest;
};

constexpr uint64_t IndexStart = 4096;
static_assert(sizeof(RegionHeader) <= IndexStart, "header must fit");
static_assert(sizeof(IndexedCopy) == 80, "a record is ten words");

uint64_t alignUp(uint64_t V, uint64_t A) { return (V + A - 1) & ~(A - 1); }

/// 2^18 slots, 6 MiB, for a region of 1 GiB or more; fewer for a small
/// region, at least 256. A build stores a few thousand headers.
uint64_t slotsFor(uint64_t Size) {
  uint64_t Slots = 256;
  while (Slots < (1u << 18) && Slots < Size / 4096)
    Slots *= 2;
  return Slots;
}

uint64_t keyOf(llvm::StringRef Name) {
  uint64_t K = llvm::xxh3_64bits(Name);
  return K ? K : 1;
}

} // namespace

llvm::Expected<SharedRegion> SharedRegion::create(llvm::StringRef Path,
                                                  uint64_t Size) {
  uint64_t Slots = slotsFor(Size);
  // Entries start after the index, aligned so a stored parsed header begins
  // on a boundary the AST reader is happy to read from directly.
  uint64_t DataStart = alignUp(IndexStart + Slots * sizeof(IndexSlot), 4096);
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
  H->IndexSlots = Slots;
  H->DataStart = DataStart;
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
  if (Offset < H->DataStart || Offset > Used || Length > Used - Offset)
    return {};
  return llvm::StringRef(Mapping.const_data() + Offset, Length);
}

std::optional<uint64_t> SharedRegion::indexCopy(llvm::StringRef Name,
                                                const IndexedCopy &Copy) {
  if (!valid())
    return std::nullopt;
  auto *H = reinterpret_cast<RegionHeader *>(Mapping.data());
  auto *Slots = reinterpret_cast<IndexSlot *>(Mapping.data() + IndexStart);
  uint64_t Key = keyOf(Name), Mask = H->IndexSlots - 1;
  // The record, written whole before a slot names it: a reader that
  // finds it through the slot sees every word.
  auto Append = [&](uint64_t Next) -> std::optional<uint64_t> {
    uint64_t Words[10] = {Next,        Copy.MetadataOffset, Copy.MetadataLength,
                          Copy.Offset, Copy.Length,         Copy.MetadataHash,
                          0,           0,                   0,
                          0};
    return append(llvm::StringRef(reinterpret_cast<const char *>(Words),
                                  sizeof(Words)));
  };
  for (uint64_t I = 0; I <= Mask; ++I) {
    IndexSlot &S = Slots[(Key + I) & Mask];
    uint64_t K = S.Key.load(std::memory_order_relaxed);
    if (K == 0) {
      std::string Blob(8, '\0');
      for (unsigned B = 0; B != 8; ++B)
        Blob[B] = static_cast<char>((Name.size() >> (8 * B)) & 0xff);
      Blob += Name;
      std::optional<uint64_t> NameAt = append(Blob);
      std::optional<uint64_t> Record = NameAt ? Append(0) : std::nullopt;
      if (!Record)
        return std::nullopt;
      S.Name.store(*NameAt, std::memory_order_relaxed);
      S.Newest.store(*Record, std::memory_order_relaxed);
      // Release: a reader that sees the key sees the name and the record.
      S.Key.store(Key, std::memory_order_release);
      return Record;
    }
    if (K != Key)
      continue;
    llvm::StringRef Len = read(S.Name.load(std::memory_order_relaxed), 8);
    uint64_t N = 0;
    for (unsigned B = 0; B != Len.size(); ++B)
      N |= static_cast<uint64_t>(static_cast<uint8_t>(Len[B])) << (8 * B);
    if (Len.size() != 8 ||
        read(S.Name.load(std::memory_order_relaxed) + 8, N) != Name)
      continue;
    std::optional<uint64_t> Record =
        Append(S.Newest.load(std::memory_order_relaxed));
    if (!Record)
      return std::nullopt;
    S.Newest.store(*Record, std::memory_order_release);
    return Record;
  }
  return std::nullopt;
}

void SharedRegion::stampCopy(uint64_t Record, uint64_t Stamp) {
  if (IndexedCopy *C = const_cast<IndexedCopy *>(copyAt(Record)))
    C->Stamp.store(Stamp, std::memory_order_relaxed);
}

void SharedRegion::countServe(uint64_t Record) {
  if (IndexedCopy *C = const_cast<IndexedCopy *>(copyAt(Record)))
    C->Serves.fetch_add(1, std::memory_order_relaxed);
}

static_assert(sizeof(MergeRecord) == 40, "a merge record is five words");

const MergeRecord *SharedRegion::mergeAt(uint64_t At) const {
  if (!At || At % 8 || read(At, sizeof(MergeRecord)).empty())
    return nullptr;
  return reinterpret_cast<const MergeRecord *>(Mapping.const_data() + At);
}

const MergeRecord *SharedRegion::comboOf(const IndexedCopy &Copy,
                                         uint64_t Combo) const {
  for (const MergeRecord *M =
           mergeAt(Copy.Merge.load(std::memory_order_acquire));
       M; M = mergeAt(M->Next))
    if (M->Combo == Combo)
      return M;
  return nullptr;
}

std::optional<uint64_t> SharedRegion::countCombo(uint64_t Record,
                                                 uint64_t Combo,
                                                 uint64_t Serves,
                                                 uint64_t Instructions) {
  IndexedCopy *C = const_cast<IndexedCopy *>(copyAt(Record));
  if (!C)
    return std::nullopt;
  if (const MergeRecord *M = comboOf(*C, Combo)) {
    auto *W = const_cast<MergeRecord *>(M);
    W->Serves.fetch_add(Serves, std::memory_order_relaxed);
    W->Instructions.fetch_add(Instructions, std::memory_order_relaxed);
    return offsetOf(M);
  }
  // Written whole before the copy names it.
  uint64_t Words[5] = {C->Merge.load(std::memory_order_relaxed), Combo, Serves,
                       0, Instructions};
  std::optional<uint64_t> At = append(
      llvm::StringRef(reinterpret_cast<const char *>(Words), sizeof(Words)));
  if (At)
    C->Merge.store(*At, std::memory_order_release);
  return At;
}

std::optional<uint64_t> SharedRegion::claimMerge(uint64_t Record,
                                                 uint64_t Combo) {
  std::optional<uint64_t> At = countCombo(Record, Combo, /*Serves=*/0);
  if (!At)
    return std::nullopt;
  auto *M = const_cast<MergeRecord *>(mergeAt(*At));
  uint64_t None = 0;
  if (!M->Merged.compare_exchange_strong(None, 1, std::memory_order_relaxed))
    return std::nullopt;
  return At;
}

void SharedRegion::setMerged(uint64_t At, uint64_t Merged) {
  if (auto *M = const_cast<MergeRecord *>(mergeAt(At)))
    // Release: a reader that sees the record sees the merged copy's.
    M->Merged.store(Merged, std::memory_order_release);
}

uint64_t SharedRegion::offsetOf(const void *Record) const {
  return static_cast<const char *>(Record) - Mapping.const_data();
}

bool SharedRegion::publishSearches(uint64_t Record, uint64_t Build,
                                   llvm::ArrayRef<uint64_t> Pairs) {
  IndexedCopy *C = const_cast<IndexedCopy *>(copyAt(Record));
  if (!C)
    return false;
  std::vector<uint64_t> Words;
  Words.reserve(Pairs.size() + 2);
  Words.push_back(Build);
  Words.push_back(Pairs.size() / 2);
  Words.insert(Words.end(), Pairs.begin(), Pairs.end());
  std::optional<uint64_t> At =
      append(llvm::StringRef(reinterpret_cast<const char *>(Words.data()),
                             Words.size() * 8));
  if (!At)
    return false;
  // Release: a reader that sees the offset sees the outcomes behind it.
  C->Searches.store(*At, std::memory_order_release);
  return true;
}

uint64_t SharedRegion::newestCopy(llvm::StringRef Name) const {
  if (!valid())
    return 0;
  const auto *H = reinterpret_cast<const RegionHeader *>(Mapping.const_data());
  const auto *Slots =
      reinterpret_cast<const IndexSlot *>(Mapping.const_data() + IndexStart);
  uint64_t Key = keyOf(Name), Mask = H->IndexSlots - 1;
  for (uint64_t I = 0; I <= Mask; ++I) {
    const IndexSlot &S = Slots[(Key + I) & Mask];
    uint64_t K = S.Key.load(std::memory_order_acquire);
    if (K == 0)
      return 0;
    if (K != Key)
      continue;
    uint64_t NameAt = S.Name.load(std::memory_order_relaxed);
    llvm::StringRef Len = read(NameAt, 8);
    if (Len.size() != 8)
      return 0;
    uint64_t N = 0;
    for (unsigned B = 0; B != 8; ++B)
      N |= static_cast<uint64_t>(static_cast<uint8_t>(Len[B])) << (8 * B);
    if (read(NameAt + 8, N) == Name)
      return S.Newest.load(std::memory_order_acquire);
  }
  return 0;
}

const IndexedCopy *SharedRegion::copyAt(uint64_t Record) const {
  if (Record % 8 || read(Record, sizeof(IndexedCopy)).empty())
    return nullptr;
  return reinterpret_cast<const IndexedCopy *>(Mapping.const_data() + Record);
}

std::optional<StoredSearches> SharedRegion::searchesAt(uint64_t Offset) const {
  llvm::StringRef Head = read(Offset, 16);
  if (Offset % 8 || Head.size() != 16)
    return std::nullopt;
  const auto *W = reinterpret_cast<const uint64_t *>(Head.data());
  if (W[1] > (1u << 24) || read(Offset + 16, W[1] * 16).size() != W[1] * 16)
    return std::nullopt;
  return StoredSearches{W[0], llvm::ArrayRef<uint64_t>(W + 2, W[1] * 2)};
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
