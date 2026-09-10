//===- Plugin.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Populates a header token-stream cache daemon by observing ordinary
// compiles. Register with:
//
//   clang -Xclang -load -Xclang <path>/TokenStreamCacheObserverPlugin.so \
//         -Xclang -add-plugin -Xclang token-stream-cache \
//         -Xclang -plugin-arg-token-stream-cache -Xclang -socket=/path/to.sock
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/CacheObserver/CacheSocketClient.h"
#include "clang/CacheObserver/TokenStreamCacheObserver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/Support/Error.h"

using namespace clang;
using namespace clang::cache_observer;

namespace {

/// All the interesting work happens via the PPCallbacks
/// TokenStreamCacheObserver, driven purely off the Preprocessor -- this
/// plugin never needs to look at the AST itself. The consumer exists only
/// to satisfy CreateASTConsumer's contract and drive the compilation to
/// completion.
class NullConsumer : public ASTConsumer {};

class TokenStreamCacheAction : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override {
    Collector = std::make_unique<syntax::TokenCollector>(CI.getPreprocessor());
    auto Obs = std::make_unique<TokenStreamCacheObserver>(
        CI.getPreprocessor(), *Sink);
    Observer = Obs.get();
    // Only ever adds a PPCallbacks registration -- never touches
    // Preprocessor::setTokenWatcher, which the TokenCollector constructed
    // above already claimed for itself.
    CI.getPreprocessor().addPPCallbacks(std::move(Obs));
    return std::make_unique<NullConsumer>();
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &Args) override {
    DiagnosticsEngine &Diags = CI.getDiagnostics();
    std::string SocketPath;
    for (StringRef Arg : Args) {
      if (Arg.consume_front("-socket=")) {
        SocketPath = Arg.str();
        continue;
      }
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "unknown token-stream-cache plugin argument '%0'"))
          << Arg;
      return false;
    }
    if (SocketPath.empty()) {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "token-stream-cache plugin requires -socket=<path>"));
      return false;
    }

    llvm::Expected<std::unique_ptr<SocketCachePayloadSink>> Connected =
        SocketCachePayloadSink::connect(SocketPath);
    if (!Connected) {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "token-stream-cache: failed to connect to caching daemon at "
          "'%0': %1"))
          << SocketPath << llvm::toString(Connected.takeError());
      return false;
    }
    Sink = std::move(*Connected);
    return true;
  }

  ActionType getActionType() override { return AddAfterMainAction; }

  void EndSourceFileAction() override {
    // Runs before CI.setPreprocessor(nullptr) tears down the PPCallbacks
    // chain (and therefore Observer), so this is the last safe point to
    // consume the TokenCollector and hand the result to Observer.
    if (Collector && Observer)
      Observer->flush(std::move(*Collector).consume());
  }

private:
  std::unique_ptr<SocketCachePayloadSink> Sink;
  std::unique_ptr<syntax::TokenCollector> Collector;
  TokenStreamCacheObserver *Observer = nullptr;
};

} // namespace

static FrontendPluginRegistry::Add<TokenStreamCacheAction>
    X("token-stream-cache", "populate the header token-stream cache daemon");
