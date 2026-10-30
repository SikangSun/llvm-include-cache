//===- CacheClientDirectory.cpp - Images in a directory -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The pre-parsed-header baseline (PPH_PLAN.md) keeps its images as files, as
// GCC's PPH did, rather than in a daemon: an image is a build product, made
// before the compiles that use it.
//
// Layout: <dir>/<16 hex digits of xxh3(header name)>/ holds one file per
// image of that header, named for its metadata's hash, and a file "header"
// with the header's name for people reading the directory. An image file is
//
//   "CLPPH002"  u64 metadata length  u64 after length  u64 data length
//   metadata  after  zero bytes up to a multiple of 8  data
//
// The metadata and data are the two parts the daemon stores for a copy
// (CacheDaemon::handlePush), the PCM 8-byte aligned as it is in the region.
// "after" is PPH's end state of the names the metadata lists
// (CheckPolicy::After): u32 count, then per macro u32 length and name, u32
// length and value, the value a byte saying whether the name is an include
// guard ('g') or not ('n') followed by the tokens. The "safe" policy
// (pphSafe) takes the guards alone. A lookup hands out offsets into the
// images it read as if they were the region's, so everything that checks and
// loads a copy reads an image unchanged.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheClient.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_socket_stream.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>

using namespace clang;
using namespace clang::ast_cache;

namespace {

constexpr llvm::StringLiteral Magic = "CLPPH002";
constexpr uint64_t HeaderSize = 32;

uint64_t alignTo8(uint64_t N) { return (N + 7) & ~uint64_t(7); }

void appendU32(std::string &Out, uint32_t V) {
  char B[4];
  llvm::support::endian::write32le(B, V);
  Out.append(B, 4);
}

std::string encodeAfter(const llvm::StringMap<std::string> &After) {
  // Sorted, so that one header's image is the same bytes every time.
  std::vector<llvm::StringRef> Names;
  for (const auto &E : After)
    Names.push_back(E.getKey());
  llvm::sort(Names);
  std::string Out;
  appendU32(Out, Names.size());
  for (llvm::StringRef N : Names) {
    const std::string &Tokens = After.find(N)->second;
    appendU32(Out, N.size());
    Out += N;
    appendU32(Out, Tokens.size());
    Out += Tokens;
  }
  return Out;
}

std::optional<llvm::StringMap<std::string>> decodeAfter(llvm::StringRef In,
                                                        bool GuardsOnly) {
  llvm::StringMap<std::string> After;
  auto U32 = [&](uint32_t &V) {
    if (In.size() < 4)
      return false;
    V = llvm::support::endian::read32le(In.data());
    In = In.drop_front(4);
    return true;
  };
  auto Str = [&](llvm::StringRef &S) {
    uint32_t N;
    if (!U32(N) || N > In.size())
      return false;
    S = In.take_front(N);
    In = In.drop_front(N);
    return true;
  };
  uint32_t Count;
  if (!U32(Count))
    return std::nullopt;
  for (uint32_t I = 0; I != Count; ++I) {
    llvm::StringRef Name, Value;
    if (!Str(Name) || !Str(Value) || Value.empty() ||
        (Value[0] != 'g' && Value[0] != 'n'))
      return std::nullopt;
    if (!GuardsOnly || Value[0] == 'g')
      After[Name] = Value.drop_front().str();
  }
  if (!In.empty())
    return std::nullopt;
  return After;
}

std::string hex16(uint64_t H) {
  std::string S = llvm::utohexstr(H, /*LowerCase=*/true);
  return std::string(16 - S.size(), '0') + S;
}

std::string headerDir(llvm::StringRef Dir, llvm::StringRef Filename) {
  llvm::SmallString<256> P(Dir);
  llvm::sys::path::append(P, hex16(llvm::xxh3_64bits(Filename)));
  return std::string(P);
}

/// Writes \p Bytes to \p Path through a temporary file in the same directory,
/// renamed into place: a reader sees the old file, no file, or the whole new
/// one. Two compiles writing the same image write the same bytes.
bool writeAtomically(llvm::StringRef Path, llvm::ArrayRef<llvm::StringRef> Bytes) {
  llvm::SmallString<256> Temp(llvm::sys::path::parent_path(Path));
  llvm::sys::path::append(Temp, ".tmp-%%%%%%%%%%%%");
  int FD;
  llvm::SmallString<256> TempPath;
  if (llvm::sys::fs::createUniqueFile(Temp, FD, TempPath))
    return false;
  {
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    for (llvm::StringRef B : Bytes)
      OS << B;
    OS.close();
    if (OS.has_error()) {
      OS.clear_error();
      llvm::sys::fs::remove(TempPath);
      return false;
    }
  }
  if (llvm::sys::fs::rename(TempPath, Path)) {
    llvm::sys::fs::remove(TempPath);
    return false;
  }
  return true;
}

} // namespace

std::unique_ptr<CacheClient> CacheClient::openDirectory(llvm::StringRef Dir) {
  std::unique_ptr<CacheClient> C(new CacheClient());
  llvm::SmallString<256> Abs(Dir);
  llvm::sys::fs::make_absolute(Abs);
  C->ImageDir = std::string(Abs);
  return C;
}

llvm::StringRef CacheClient::bytesAt(uint64_t Offset, uint64_t Length) const {
  if (!isDirectory())
    return Region.read(Offset, Length);
  // The image whose range holds Offset: the last one starting at or before
  // it. An offset outside every image reads as nothing, as the region's do.
  auto It = llvm::upper_bound(ImageStarts, Offset);
  if (It == ImageStarts.begin())
    return {};
  size_t I = It - ImageStarts.begin() - 1;
  llvm::StringRef Buf = Images[I]->getBuffer();
  uint64_t At = Offset - ImageStarts[I];
  if (At > Buf.size() || Length > Buf.size() - At)
    return {};
  return Buf.substr(At, Length);
}

bool CacheClient::hasImages(llvm::StringRef Filename) {
  auto [It, New] = HasImages.try_emplace(Filename, false);
  if (New)
    It->second = llvm::sys::fs::is_directory(headerDir(ImageDir, Filename));
  return It->second;
}

std::vector<CacheEntryRef> CacheClient::readImages(llvm::StringRef Filename) {
  // A generating compile writes this header's image; it is never served one.
  if (Filename == GenTarget)
    return {};
  auto [Slot, New] = ImagesOf.try_emplace(Filename);
  if (!New)
    return Slot->second;
  std::string Dir = headerDir(ImageDir, Filename);
  std::vector<std::string> Files;
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC))
    if (llvm::sys::path::extension(It->path()) == ".pph")
      Files.push_back(It->path());
  // Images in a fixed order, so that a compile chooses among them the same
  // way every time.
  llvm::sort(Files);

  std::vector<CacheEntryRef> &Entries = Slot->second;
  for (const std::string &F : Files) {
    // Never rewritten in place (writeAtomically), so mapping it is safe.
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
        llvm::MemoryBuffer::getFile(F, /*IsText=*/false,
                                    /*RequiresNullTerminator=*/false);
    if (!Buf)
      continue;
    llvm::StringRef B = (*Buf)->getBuffer();
    if (B.size() < HeaderSize || !B.starts_with(Magic))
      continue;
    uint64_t MetaLen = llvm::support::endian::read64le(B.data() + 8);
    uint64_t AfterLen = llvm::support::endian::read64le(B.data() + 16);
    uint64_t DataLen = llvm::support::endian::read64le(B.data() + 24);
    if (MetaLen > B.size() || AfterLen > B.size() - MetaLen)
      continue;
    uint64_t DataAt = alignTo8(HeaderSize + MetaLen + AfterLen);
    if (DataAt > B.size() || DataLen != B.size() - DataAt)
      continue;
    // The daemon checks a copy's metadata whole when it is pushed; nothing
    // has checked an image's, so it is checked here, once per compile.
    std::optional<MetadataView> V =
        MetadataView::open(B.substr(HeaderSize, MetaLen));
    if (!V || !V->validate())
      continue;
    std::optional<llvm::StringMap<std::string>> After =
        decodeAfter(B.substr(HeaderSize + MetaLen, AfterLen), PPHSafe);
    if (!After)
      continue;
    uint64_t Start = ImagesEnd;
    ImageStarts.push_back(Start);
    ImagesEnd = alignTo8(Start + B.size());
    Images.push_back(std::move(*Buf));
    CacheEntryRef E;
    E.MetadataOffset = Start + HeaderSize;
    E.MetadataLength = MetaLen;
    E.Offset = Start + DataAt;
    E.Length = DataLen;
    // A link names a copy by its metadata's xxh3, as the daemon's index
    // does (CacheEntryRef::MetadataHash).
    E.MetadataHash = llvm::xxh3_64bits(B.substr(HeaderSize, MetaLen));
    AfterOf[E.MetadataOffset] = std::move(*After);
    Entries.push_back(std::move(E));
  }
  return Entries;
}

void CacheClient::refuseImage(llvm::StringRef Why) {
  if (!Refused.empty())
    return;
  Refused = Why.str();
  llvm::errs() << "pph: " << GenTarget << ": no image: " << Why << "\n";
}

bool CacheClient::writeImage(const PushRequest &Req) {
  if (!Refused.empty())
    return false;
  llvm::SmallVector<char, 0> Meta;
  encodeMetadata(Req.Deps, Req.ModuleName, Meta);
  llvm::StringRef M(Meta.data(), Meta.size());
  std::string After =
      encodeAfter(EndState ? EndState(Req.Deps) : llvm::StringMap<std::string>());
  std::string Dir = headerDir(ImageDir, Req.Filename);
  if (llvm::sys::fs::create_directories(Dir))
    return false;

  llvm::SmallString<256> Name(Dir);
  llvm::sys::path::append(Name, "header");
  if (!llvm::sys::fs::exists(Name) &&
      !writeAtomically(Name, {Req.Filename, "\n"}))
    return false;

  char Header[HeaderSize];
  memcpy(Header, Magic.data(), 8);
  llvm::support::endian::write64le(Header + 8, M.size());
  llvm::support::endian::write64le(Header + 16, After.size());
  llvm::support::endian::write64le(Header + 24, Req.Data.size());
  uint64_t Used = HeaderSize + M.size() + After.size();
  std::string Pad(alignTo8(Used) - Used, '\0');
  llvm::SmallString<256> Path(Dir);
  llvm::sys::path::append(Path, hex16(llvm::xxh3_64bits(M)) + ".pph");
  return writeAtomically(Path, {llvm::StringRef(Header, HeaderSize), M, After,
                                Pad, Req.Data});
}
