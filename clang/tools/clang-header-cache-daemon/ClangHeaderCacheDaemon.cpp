//===- ClangHeaderCacheDaemon.cpp - Header cache daemon -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Serves cached parsed headers to compiler processes over a Unix socket. See
// clang/include/clang/ASTCache/CacheDaemon.h for the protocol and the
// threading model.
//
// Holds nothing across restarts: it starts with an empty table and a fresh
// shared region, and loads nothing from disk.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/SharedRegion.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"
#include <csignal>

using namespace llvm;
using namespace clang::ast_cache;

static cl::OptionCategory Category("clang-header-cache-daemon options");

static cl::opt<std::string>
    SocketPath("socket", cl::desc("Unix socket to listen on"),
               cl::init(DefaultSocketPath), cl::cat(Category));

static cl::opt<std::string>
    RegionPath("region", cl::desc("Backing file for the shared region"),
               cl::init(DefaultRegionPath), cl::cat(Category));

static cl::opt<unsigned long long>
    RegionSize("region-size",
               cl::desc("Shared region size in bytes; reserved up front and "
                        "sparse until written"),
               cl::init(DefaultRegionSize), cl::cat(Category));

static cl::opt<bool> Verbose("v", cl::desc("Report stats on exit"),
                             cl::init(false), cl::cat(Category));

static cl::opt<unsigned long long> AskCutoff(
    "ask-cutoff-us",
    cl::desc("A header some compile took at least this many microseconds to "
             "read goes on the ask-list, the headers compiles ask about"),
    cl::init(DefaultAskCutoffMicros), cl::cat(Category));

static cl::opt<bool> ListStored(
    "list-stored",
    cl::desc("Also put every header with a stored copy on the ask-list, timed "
             "or not (for clang-header-cache-populate and tests)"),
    cl::init(false), cl::cat(Category));

static cl::opt<bool> DropCostly(
    "drop-costly",
    cl::desc("Drop a header from the ask-list once its hits cost more than "
             "parsing it: its first parse time is below its average hit cost "
             "plus -later-cost-us (DECISIONS.md 2.20; off by default)"),
    cl::init(false), cl::cat(Category));

static cl::opt<unsigned long long>
    LaterCost("later-cost-us",
              cl::desc("What one loaded PCM adds to the rest of a compile, in "
                       "microseconds"),
              cl::init(DefaultLaterCostMicros), cl::cat(Category));

static CacheDaemon *TheDaemon = nullptr;

/// Signal handling exists for one reason: the socket file has to come off
/// disk, or the next run cannot bind. Nothing else needs unwinding -- no
/// connection draining, no in-flight completion, no persistence.
static void onSignal(int) {
  if (TheDaemon)
    TheDaemon->stop();
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions(Category);
  cl::ParseCommandLineOptions(
      argc, argv,
      "Serves cached parsed headers to clang processes.\n\n"
      "Start before building; stop with Ctrl-C. Nothing is persisted.\n");

  CacheDaemon::Options Opts;
  Opts.AskCutoffMicros = AskCutoff;
  Opts.ListStored = ListStored;
  Opts.DropCostly = DropCostly;
  Opts.LaterCostMicros = LaterCost;
  Expected<std::unique_ptr<CacheDaemon>> D =
      CacheDaemon::create(RegionPath, RegionSize, Opts);
  if (!D) {
    errs() << "clang-header-cache-daemon: " << toString(D.takeError()) << "\n";
    return 1;
  }
  TheDaemon = D->get();

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  outs() << "listening on " << SocketPath << "\n"
         << "region " << RegionPath << " (" << (RegionSize / (1024 * 1024))
         << " MiB reserved)\n";
  outs().flush();

  if (Error E = (*D)->run(SocketPath)) {
    errs() << "clang-header-cache-daemon: " << toString(std::move(E)) << "\n";
    return 1;
  }

  if (Verbose) {
    CacheDaemon::Stats S = (*D)->stats();
    outs() << "lookups " << S.Lookups << " (" << S.Hits << " with entries)\n"
           << "pushes  " << S.Pushes << " (" << S.Duplicates << " duplicate)\n"
           << "stored  " << S.Entries << " entries for " << S.Headers
           << " headers, " << (S.BytesStored / 1024)
           << " KiB: " << (S.PartBytes / 1024) << " KiB of parts, "
           << (S.NameBytes / 1024) << " KiB of not-a-macro names, "
           << (S.EffectBytes / 1024) << " KiB of macro effects; "
           << (S.MetadataBytes / 1024)
           << " KiB of metadata in the daemon's memory\n"
           << "timed   " << S.HeadersTimed << " headers from "
           << S.TimingReports << " compiles; " << S.HeadersListed
           << " on the ask-list; " << S.HitsTimed << " hits timed, "
           << S.HeadersDropped
           << " headers dropped as costing more than "
              "parsing\n";
  }
  return 0;
}
