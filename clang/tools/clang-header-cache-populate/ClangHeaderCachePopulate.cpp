//===- ClangHeaderCachePopulate.cpp - Populate the header cache -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compiles each named header on its own and, if a daemon is running, pushes
// the result to it. With -dry-run it pushes nothing and only reports, which
// answers the question worth asking before any of the rest of the machinery
// matters: what fraction of real headers can be cached at all, and why the
// others cannot.
//
// Usage:
//   clang-header-cache-populate [options] <header>... [-- <compiler args>]
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/SharedRegion.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_socket_stream.h"
#include <map>

using namespace llvm;
using namespace clang::ast_cache;

static cl::OptionCategory Category("clang-header-cache-populate options");

static cl::list<std::string> Headers(cl::Positional, cl::desc("<header>..."),
                                     cl::OneOrMore, cl::cat(Category));

/// Repeatable rather than a trailing "-- args" list: cl::ConsumeAfter takes
/// over the positional list and leaves only the first header behind.
static cl::list<std::string>
    CompilerArgs("Xcc",
                 cl::desc("Pass an argument to the compile, e.g. -Xcc -I. "
                          "-Xcc -DFOO=1. Repeatable."),
                 cl::value_desc("arg"), cl::cat(Category));

static cl::opt<std::string>
    SocketPath("socket", cl::desc("Daemon socket; ignored with -dry-run"),
               cl::init(DefaultSocketPath), cl::cat(Category));

static cl::opt<bool>
    DryRun("dry-run",
           cl::desc("Report what would be cached without contacting a daemon"),
           cl::init(false), cl::cat(Category));

static cl::opt<bool> ListDeclines("list-declines",
                                  cl::desc("Print every declined header"),
                                  cl::init(false), cl::cat(Category));

namespace {

struct Totals {
  unsigned Produced = 0;
  unsigned Declined = 0;
  unsigned Failed = 0;
  uint64_t ResultBytes = 0;
  uint64_t RecordBytes = 0;
  unsigned Conditional = 0, Value = 0, Symbol = 0, Inclusion = 0;
  /// Decline reasons, counted. The distribution is the actual finding here:
  /// it says which limitation is costing the most coverage.
  std::map<std::string, unsigned> Reasons;
};

void report(const Totals &T, unsigned Total) {
  auto pct = [&](unsigned N) { return Total ? (100.0 * N / Total) : 0.0; };

  outs() << "\n" << Total << " headers\n"
         << "  cacheable   " << T.Produced << format("  (%.1f%%)\n", pct(T.Produced))
         << "  declined    " << T.Declined << format("  (%.1f%%)\n", pct(T.Declined))
         << "  failed      " << T.Failed << format("  (%.1f%%)\n", pct(T.Failed));

  if (T.Produced) {
    outs() << "\nper cacheable header, mean:\n"
           << format("  stored result   %8.1f KiB\n",
                     T.ResultBytes / 1024.0 / T.Produced)
           << format("  record          %8.1f bytes\n",
                     double(T.RecordBytes) / T.Produced)
           << format("  dependencies    %8.1f conditional, %.1f value, "
                     "%.1f symbol, %.1f inclusion\n",
                     double(T.Conditional) / T.Produced,
                     double(T.Value) / T.Produced,
                     double(T.Symbol) / T.Produced,
                     double(T.Inclusion) / T.Produced);
  }

  if (!T.Reasons.empty()) {
    outs() << "\nwhy headers were declined:\n";
    for (const auto &KV : T.Reasons)
      outs() << format("  %5u  ", KV.second) << KV.first << "\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions(Category);
  cl::ParseCommandLineOptions(
      argc, argv,
      "Compiles each header on its own and stores the result.\n\n"
      "With -dry-run, reports how many headers are cacheable and why the\n"
      "rest are not, without needing a daemon.\n\n"
      "Pass the build's own flags with -Xcc, e.g. -Xcc -I. -Xcc -DHAVE_X=1\n");

  std::unique_ptr<raw_socket_stream> Conn;
  if (!DryRun) {
    Expected<std::unique_ptr<raw_socket_stream>> S =
        raw_socket_stream::createConnectedUnix(SocketPath);
    if (!S) {
      errs() << "cannot reach a daemon at " << SocketPath << ": "
             << toString(S.takeError())
             << "\n(start clang-header-cache-daemon, or pass -dry-run)\n";
      return 1;
    }
    Conn = std::move(*S);
  }

  Totals T;
  for (const std::string &H : Headers) {
    ProduceResult R = produceHeader(H, CompilerArgs);

    switch (R.Stat) {
    case ProduceResult::Status::Produced: {
      ++T.Produced;
      T.ResultBytes += R.Push.Data.size();
      T.Conditional += R.Push.Deps.Conditional.size();
      T.Value += R.Push.Deps.Value.size();
      T.Symbol += R.Push.Deps.Symbol.size();
      T.Inclusion += R.Push.Deps.Inclusion.size();

      SmallVector<char, 0> Msg;
      encodePushRequest(R.Push, Msg);
      // The record's size is what a LOOKUP reply carries per candidate, so
      // it is worth reporting separately from the stored result.
      T.RecordBytes += Msg.size() - R.Push.Data.size();

      if (Conn) {
        if (!sendMessage(*Conn, Msg)) {
          errs() << "lost the daemon connection while pushing " << H << "\n";
          return 1;
        }
        if (!receiveMessage(*Conn)) {
          errs() << "no acknowledgment from the daemon for " << H << "\n";
          return 1;
        }
      }
      break;
    }
    case ProduceResult::Status::Declined:
      ++T.Declined;
      ++T.Reasons[R.Reason];
      if (ListDeclines)
        outs() << "declined  " << H << "  -- " << R.Reason << "\n";
      break;
    case ProduceResult::Status::Failed:
      ++T.Failed;
      ++T.Reasons["FAILED: " + R.Reason];
      errs() << "failed    " << H << "  -- " << R.Reason << "\n";
      break;
    }
  }

  report(T, Headers.size());
  return 0;
}
