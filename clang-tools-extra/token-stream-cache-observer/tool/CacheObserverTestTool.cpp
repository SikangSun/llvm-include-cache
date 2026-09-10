//===- CacheObserverTestTool.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A standalone test harness for TokenStreamCacheObserver: runs it over the
// given source files using an in-memory sink (no real daemon/socket
// involved) and dumps a deterministic textual rendering of every captured
// HeaderCachePayload, for lit/FileCheck-based testing of the capture,
// skip-region-attribution, and macro-digest logic in isolation.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/CacheObserver/CacheSocketClient.h"
#include "clang/CacheObserver/TokenStreamCacheObserver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"

using namespace clang;
using namespace clang::cache_observer;
using namespace llvm;

namespace {

cl::OptionCategory Cat("cache-observer-test-tool options");

class CollectingAction : public ASTFrontendAction {
public:
  explicit CollectingAction(InMemoryCachePayloadSink &Sink) : Sink(Sink) {}

protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override {
    Collector = std::make_unique<syntax::TokenCollector>(CI.getPreprocessor());
    auto Obs =
        std::make_unique<TokenStreamCacheObserver>(CI.getPreprocessor(), Sink);
    Observer = Obs.get();
    CI.getPreprocessor().addPPCallbacks(std::move(Obs));
    return std::make_unique<ASTConsumer>();
  }

  void EndSourceFileAction() override {
    if (Collector && Observer)
      Observer->flush(std::move(*Collector).consume());
  }

private:
  InMemoryCachePayloadSink &Sink;
  std::unique_ptr<syntax::TokenCollector> Collector;
  TokenStreamCacheObserver *Observer = nullptr;
};

class CollectingActionFactory : public tooling::FrontendActionFactory {
public:
  explicit CollectingActionFactory(InMemoryCachePayloadSink &Sink)
      : Sink(Sink) {}
  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<CollectingAction>(Sink);
  }

private:
  InMemoryCachePayloadSink &Sink;
};

StringRef conditionKindName(WireConditionKind K) {
  switch (K) {
  case WireConditionKind::If:
    return "If";
  case WireConditionKind::Ifdef:
    return "Ifdef";
  case WireConditionKind::Ifndef:
    return "Ifndef";
  case WireConditionKind::Elif:
    return "Elif";
  case WireConditionKind::Elifdef:
    return "Elifdef";
  case WireConditionKind::Elifndef:
    return "Elifndef";
  case WireConditionKind::Else:
    return "Else";
  }
  llvm_unreachable("unknown WireConditionKind");
}

void dumpPayload(raw_ostream &OS, const HeaderCachePayload &Payload) {
  OS << "Header: " << Payload.HeaderPath << "\n";
  OS << "MacroDigest: " << Payload.MacroDigestOnEntry.digest() << "\n";
  OS << "SpelledTokens:\n";
  for (const WireToken &Tok : Payload.SpelledTokens)
    OS << "  " << tok::getTokenName(static_cast<tok::TokenKind>(Tok.Kind))
       << " '" << Tok.Spelling << "' @" << Tok.Offset << "\n";
  OS << "SkippedRegions:\n";
  for (const WireSkippedRegion &Region : Payload.SkippedRegions)
    OS << "  [" << Region.BeginOffset << ", " << Region.EndOffset << ") "
       << conditionKindName(Region.ConditionKind) << " guard='"
       << Region.GuardMacroName << "'\n";
  OS << "---\n";
}

} // namespace

int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);
  auto OptionsParser =
      tooling::CommonOptionsParser::create(argc, argv, Cat, cl::ZeroOrMore);
  if (!OptionsParser) {
    WithColor::error() << toString(OptionsParser.takeError()) << "\n";
    return 1;
  }

  tooling::ClangTool Tool(OptionsParser->getCompilations(),
                          OptionsParser->getSourcePathList());

  InMemoryCachePayloadSink Sink;
  CollectingActionFactory Factory(Sink);
  int HadErrors = Tool.run(&Factory);
  if (HadErrors)
    return HadErrors;

  for (const HeaderCachePayload &Payload : Sink.payloads())
    dumpPayload(outs(), Payload);

  return 0;
}
