//===- TokenStreamCacheObserver.h ------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CACHEOBSERVER_TOKENSTREAMCACHEOBSERVER_H
#define LLVM_CLANG_CACHEOBSERVER_TOKENSTREAMCACHEOBSERVER_H

#include "clang/Basic/SourceLocation.h"
#include "clang/CacheObserver/CacheWireFormat.h"
#include "clang/CacheObserver/MacroStateDigest.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/TransitiveClosureTracker.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <string>
#include <vector>

namespace clang {
class Preprocessor;

namespace syntax {
class TokenBuffer;
}

namespace cache_observer {

/// Sink that a populated HeaderCachePayload is handed off to once a
/// header's data collection is finalized. See CacheSocketClient.h for the
/// production (Unix-domain-socket) and in-memory (testing)
/// implementations.
class CachePayloadSink {
public:
  virtual ~CachePayloadSink() = default;
  virtual void send(const HeaderCachePayload &Payload) = 0;
};

/// A PPCallbacks observer that watches a single translation unit's
/// preprocessing and, once flush() is called, emits one HeaderCachePayload
/// per #included file to the given CachePayloadSink.
///
/// This class only ever registers itself via Preprocessor::addPPCallbacks;
/// it deliberately never touches Preprocessor::setTokenWatcher, since
/// syntax::TokenCollector uses that single-slot API internally and a
/// second caller would silently steal it. Driving a TokenCollector
/// alongside this observer (both attached to the same Preprocessor) is
/// the expected usage -- see the token-stream-cache-observer plugin.
class TokenStreamCacheObserver : public PPCallbacks {
public:
  TokenStreamCacheObserver(Preprocessor &PP, CachePayloadSink &Sink);

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                    SrcMgr::CharacteristicKind FileType,
                    FileID PrevFID) override;

  // Only the "opening" directives push/pop a conditional frame. Elif,
  // Elifdef, Elifndef, and Else deliberately have no override here -- see
  // the .cpp for why mutating the frame on those would misattribute
  // SourceRangeSkipped, which only reports one merged range per skip
  // search regardless of how many elif/else branches it passes through.
  void If(SourceLocation Loc, SourceRange ConditionRange,
          ConditionValueKind ConditionValue) override;
  void Ifdef(SourceLocation Loc, const Token &MacroNameTok,
             const MacroDefinition &MD) override;
  void Ifndef(SourceLocation Loc, const Token &MacroNameTok,
              const MacroDefinition &MD) override;
  void Endif(SourceLocation Loc, SourceLocation IfLoc) override;

  void SourceRangeSkipped(SourceRange Range, SourceLocation EndifLoc) override;

  void MacroDefined(const Token &MacroNameTok,
                     const MacroDirective *MD) override;
  void MacroUndefined(const Token &MacroNameTok, const MacroDefinition &MD,
                       const MacroDirective *Undef) override;

  /// Emits one HeaderCachePayload per file entered during this translation
  /// unit, pulling spelled tokens out of \p Buffer (which must be the
  /// TokenBuffer produced by consuming the syntax::TokenCollector that was
  /// driving the same Preprocessor). Call this exactly once, from
  /// EndSourceFileAction, before the Preprocessor (and therefore this
  /// callback object) is torn down.
  void flush(const syntax::TokenBuffer &Buffer);

private:
  struct ConditionalFrame {
    WireConditionKind Kind;
    std::string GuardMacroName; // empty if not a single-macro guard
  };

  struct InFlightRecord {
    std::string Path;
    uint64_t ContentHash = 0;
    llvm::MD5::MD5Result DigestOnEntry;
    std::vector<WireSkippedRegion> Skips;
  };

  /// Pops ConditionalStack's top frame if Endif has marked it closed.
  /// Called before a new push and after SourceRangeSkipped has had a
  /// chance to use the closed frame for attribution -- see Endif's
  /// comment for why the pop can't happen synchronously inside Endif
  /// itself.
  void popClosedTopConditional();

  Preprocessor &PP;
  CachePayloadSink &Sink;
  MacroStateDigest Digest;
  TransitiveClosureTracker Closure;
  llvm::SmallVector<ConditionalFrame, 8> ConditionalStack;
  bool TopConditionalClosed = false;
  llvm::DenseMap<FileID, InFlightRecord> Records;
  llvm::SmallVector<FileID, 8> RecordOrder; // stable emission order
};

} // namespace cache_observer
} // namespace clang

#endif // LLVM_CLANG_CACHEOBSERVER_TOKENSTREAMCACHEOBSERVER_H
