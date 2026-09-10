//===- TokenStreamCacheObserver.cpp ----------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CacheObserver/TokenStreamCacheObserver.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang;
using namespace clang::cache_observer;

namespace {

/// A cheap, non-cryptographic, process-independent combination of a
/// header's on-disk size and modification time. Deliberately avoids
/// llvm::hash_combine, which is seeded per-process (see
/// MacroStateDigest.h's rationale for the same constraint).
uint64_t combineSizeAndMTime(uint64_t Size, int64_t MTime) {
  return Size ^ (static_cast<uint64_t>(MTime) * 0x9E3779B97F4A7C15ULL);
}

} // namespace

TokenStreamCacheObserver::TokenStreamCacheObserver(Preprocessor &PP,
                                                     CachePayloadSink &Sink)
    : PP(PP), Sink(Sink), Digest(PP) {}

void TokenStreamCacheObserver::FileChanged(SourceLocation Loc,
                                            FileChangeReason Reason,
                                            SrcMgr::CharacteristicKind FileType,
                                            FileID PrevFID) {
  SourceManager &SM = PP.getSourceManager();
  // TransitiveClosureTracker::observeFileChanged filters out synthetic
  // buffers (the predefines/"<built-in>" buffer, -include'd command-line
  // buffers, etc.): they have no backing FileEntry and are never added to
  // TokenBuffer::Files by TokenCollector, so spelledTokens(FID) would later
  // assert. They also aren't meaningful cache targets -- the cache key is a
  // real header's path -- so skipping them here is exactly what's wanted.
  std::optional<FileEntryRef> FE = Closure.observeFileChanged(SM, Loc, Reason);
  if (!FE)
    return;

  InFlightRecord Rec;
  Rec.Path = FE->getName().str();
  Rec.ContentHash =
      combineSizeAndMTime(FE->getSize(), FE->getModificationTime());
  Rec.DigestOnEntry = Digest.snapshot();

  FileID FID = SM.getFileID(Loc);
  Records.try_emplace(FID, std::move(Rec));
  RecordOrder.push_back(FID);
}

void TokenStreamCacheObserver::popClosedTopConditional() {
  if (!TopConditionalClosed)
    return;
  if (!ConditionalStack.empty())
    ConditionalStack.pop_back();
  TopConditionalClosed = false;
}

void TokenStreamCacheObserver::If(SourceLocation Loc,
                                   SourceRange ConditionRange,
                                   ConditionValueKind ConditionValue) {
  popClosedTopConditional();
  ConditionalStack.push_back({WireConditionKind::If, std::string()});
}

void TokenStreamCacheObserver::Ifdef(SourceLocation Loc,
                                      const Token &MacroNameTok,
                                      const MacroDefinition &MD) {
  popClosedTopConditional();
  StringRef Name;
  if (const IdentifierInfo *II = MacroNameTok.getIdentifierInfo())
    Name = II->getName();
  ConditionalStack.push_back({WireConditionKind::Ifdef, Name.str()});
}

void TokenStreamCacheObserver::Ifndef(SourceLocation Loc,
                                       const Token &MacroNameTok,
                                       const MacroDefinition &MD) {
  popClosedTopConditional();
  StringRef Name;
  if (const IdentifierInfo *II = MacroNameTok.getIdentifierInfo())
    Name = II->getName();
  ConditionalStack.push_back({WireConditionKind::Ifndef, Name.str()});
}

// Elif/Elifdef/Elifndef/Else deliberately have no overrides here (the base
// PPCallbacks no-ops apply): SourceRangeSkipped always fires with a Range
// that starts at the *originating* #if/#ifdef/#ifndef's '#' and can merge
// an entire elif/else chain into one call (Clang gives no per-branch
// granularity -- see SkipExcludedConditionalBlock in PPDirectives.cpp).
// Mutating the top-of-stack frame on each Elif/Elifdef/Elifndef/Else would
// make attribution reflect whichever branch happened to fire *last*
// in-process, not the condition that actually caused the skip. Leaving the
// frame exactly as If/Ifdef/Ifndef pushed it, until Endif pops it, is the
// only attribution SourceRangeSkipped's own granularity can support.

void TokenStreamCacheObserver::Endif(SourceLocation Loc, SourceLocation IfLoc) {
  // Endif fires from two different call sites with different neighbors:
  // ordinary (non-skipping) completion of a conditional, where it is the
  // last callback for that conditional and nothing else follows; and from
  // inside SkipExcludedConditionalBlock when a skip search's *own* scan
  // discovers the matching #endif, where it is immediately followed by
  // the SourceRangeSkipped call describing everything just scanned over
  // (which still needs this frame for attribution). We can't tell the two
  // apart from this callback's parameters alone, so defer the actual pop:
  // resolve whatever the *previous* Endif left pending (it clearly wasn't
  // followed by a SourceRangeSkipped, or that call would have cleared it
  // already), then mark the current top closed rather than popping it now.
  popClosedTopConditional();
  if (!ConditionalStack.empty())
    TopConditionalClosed = true;
}

void TokenStreamCacheObserver::SourceRangeSkipped(SourceRange Range,
                                                    SourceLocation EndifLoc) {
  if (ConditionalStack.empty())
    return;

  SourceManager &SM = PP.getSourceManager();
  SourceLocation Begin = Range.getBegin();
  FileID FID = SM.getFileID(Begin);

  auto It = Records.find(FID);
  if (It != Records.end()) {
    const ConditionalFrame &Top = ConditionalStack.back();
    WireSkippedRegion Region;
    Region.BeginOffset = SM.getFileOffset(Begin);
    Region.EndOffset = SM.getFileOffset(Range.getEnd());
    Region.ConditionKind = Top.Kind;
    Region.GuardMacroName = Top.GuardMacroName;
    It->second.Skips.push_back(std::move(Region));
  }

  // If an Endif closed this frame just before this call (the
  // skip-search-found-#endif path), this is the point that frame's
  // attribution is no longer needed, so it's now safe to actually pop it.
  popClosedTopConditional();
}

void TokenStreamCacheObserver::MacroDefined(const Token &MacroNameTok,
                                             const MacroDirective *MD) {
  Digest.invalidate();
}

void TokenStreamCacheObserver::MacroUndefined(const Token &MacroNameTok,
                                               const MacroDefinition &MD,
                                               const MacroDirective *Undef) {
  Digest.invalidate();
}

void TokenStreamCacheObserver::flush(const syntax::TokenBuffer &Buffer) {
  const SourceManager &SM = Buffer.sourceManager();

  // TokenBuffer::spelledTokens(FID) asserts unless FID is present in
  // TokenBuffer::Files, and buildSpelledTokens() populates that map purely
  // from the expansion FileIDs of the *expanded* token stream
  // (Tooling/Syntax/Tokens.cpp). A real, FileEntry-backed header that
  // contributes no expanded tokens is therefore absent from it -- a header
  // holding only #defines, one whose entire body is skipped by an #if, or
  // assert.h re-included under NDEBUG. Those all reach FileChanged and get an
  // InFlightRecord, so filtering only synthetic buffers on the way in is not
  // enough; without this the flush aborts. Reconstruct the same set the
  // TokenBuffer used and emit only for files it actually holds. Dropping a
  // payload only costs a later cache miss, never a wrong hit.
  llvm::DenseSet<FileID> HasSpelledTokens;
  for (const syntax::Token &Tok : Buffer.expandedTokens())
    HasSpelledTokens.insert(SM.getFileID(SM.getExpansionLoc(Tok.location())));

  for (FileID FID : RecordOrder) {
    if (!HasSpelledTokens.contains(FID))
      continue;

    auto It = Records.find(FID);
    if (It == Records.end())
      continue;
    InFlightRecord &Rec = It->second;

    HeaderCachePayload Payload;
    Payload.HeaderPath = Rec.Path;
    Payload.ContentHash = Rec.ContentHash;
    Payload.MacroDigestOnEntry = Rec.DigestOnEntry;
    Payload.SkippedRegions = std::move(Rec.Skips);

    for (const syntax::Token &Tok : Buffer.spelledTokens(FID)) {
      WireToken WT;
      WT.Kind = static_cast<uint16_t>(Tok.kind());
      WT.Offset = SM.getFileOffset(Tok.location());
      WT.Spelling = Tok.text(SM).str();
      Payload.SpelledTokens.push_back(std::move(WT));
    }

    Sink.send(Payload);
  }
}
