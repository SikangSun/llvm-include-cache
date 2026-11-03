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
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
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

static cl::opt<bool> MeasuredCutoff(
    "measured-cutoff",
    cl::desc("In place of -ask-cutoff-us as the cutoff: list a header while "
             "the parse time of the lines its copy holds is at least one "
             "request for it and one load of its copy, each the mean of its "
             "own so far; before its first load the load is -ask-cutoff-us, "
             "before its first request the request is 0"),
    cl::init(false), cl::cat(Category));

static cl::opt<bool> Fold(
    "fold",
    cl::desc("Judge each header as its own copy against folding it into the "
             "copy around it, in place of -ask-cutoff-us: listed at its first "
             "read if its own time plus an ask per header it includes is at "
             "least an ask and a load, taken off once its losses pass "
             "-fold-budget writes"),
    cl::init(false), cl::cat(Category));

static cl::opt<bool>
    FoldFloor("fold-floor",
              cl::desc("With -fold: list only headers at or above the floor "
                       "(on by default); off, list every header"),
              cl::init(true), cl::cat(Category));

static cl::opt<CacheDaemon::Options::FoldListing> FoldList(
    "fold-list", cl::desc("With -fold: what the floor reads"),
    cl::values(clEnumValN(CacheDaemon::Options::FoldListing::OwnAndChildren,
                          "own-children",
                          "the own time plus an ask per header included "
                          "(default)"),
               clEnumValN(CacheDaemon::Options::FoldListing::Own, "own",
                          "the own time alone"),
               clEnumValN(CacheDaemon::Options::FoldListing::Inclusive,
                          "inclusive",
                          "the time with the headers inside it, against "
                          "-ask-cutoff-us")),
    cl::init(CacheDaemon::Options::FoldListing::OwnAndChildren),
    cl::cat(Category));

static cl::opt<double>
    FoldBudget("fold-budget",
               cl::desc("With -fold: the losses, in writes, after which a "
                        "header comes off the ask-list"),
               cl::init(8), cl::cat(Category));

static cl::opt<unsigned long long>
    FoldAsk("fold-ask-us", cl::desc("With -fold: an ask, in microseconds"),
            cl::init(92), cl::cat(Category));
static cl::opt<unsigned long long>
    FoldLoad("fold-load-us",
             cl::desc("With -fold: loading one copy, in microseconds"),
             cl::init(50), cl::cat(Category));
static cl::opt<unsigned long long> FoldLink(
    "fold-link-us",
    cl::desc("With -fold: what each load of a header's copy as another "
             "header's link costs it, in microseconds (0: nothing)"),
    cl::init(50), cl::cat(Category));
static cl::opt<unsigned long long>
    FoldWrite("fold-write-us",
              cl::desc("With -fold: writing one copy, in microseconds"),
              cl::init(519), cl::cat(Category));

static cl::opt<std::string> Offline(
    "offline",
    cl::desc("The offline step between builds: a command, its words split "
             "at spaces, run after a link a compile reports with the record "
             "directory and the decisions file as its last two arguments "
             "(clang/utils/header-cache/offline/offline.py)"),
    cl::init(""), cl::cat(Category));
static cl::opt<std::string> OfflineRecord(
    "offline-record",
    cl::desc("Where compiles record what the offline step reads, until the "
             "decisions file exists"),
    cl::init(""), cl::cat(Category));
static cl::opt<std::string> OfflineDecisions(
    "offline-decisions",
    cl::desc("The decisions file the offline step writes; once it exists, "
             "compiles take their decisions from it"),
    cl::init(""), cl::cat(Category));

static cl::opt<std::string> AskListLoad(
    "ask-list-load",
    cl::desc("Measurement: the ask-list is exactly the headers in this file, "
             "one path per line, and timings do not change it"),
    cl::init(""), cl::cat(Category));

static cl::opt<std::string>
    AskListSave("ask-list-save",
                cl::desc("Measurement: write the ask-list to this file on "
                         "exit, one path per line"),
                cl::init(""), cl::cat(Category));

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
  Opts.Fold = Fold;
  Opts.FoldFloor = FoldFloor;
  Opts.FoldListBy = FoldList;
  Opts.FoldBudget = FoldBudget;
  Opts.FoldAskMicros = FoldAsk;
  Opts.FoldLoadMicros = FoldLoad;
  Opts.FoldWriteMicros = FoldWrite;
  Opts.FoldLinkMicros = FoldLink;
  Opts.MeasuredCutoff = MeasuredCutoff;
  if (!Offline.empty()) {
    SmallVector<StringRef, 4> Words;
    StringRef(Offline).split(Words, ' ', -1, /*KeepEmpty=*/false);
    for (StringRef W : Words)
      Opts.OfflineCommand.push_back(W.str());
    // The program by its path: a process is started from a path, not looked
    // up the way a shell would.
    if (!StringRef(Opts.OfflineCommand[0]).contains('/')) {
      ErrorOr<std::string> P = sys::findProgramByName(Opts.OfflineCommand[0]);
      if (!P) {
        errs() << "clang-header-cache-daemon: cannot find "
               << Opts.OfflineCommand[0] << "\n";
        return 1;
      }
      Opts.OfflineCommand[0] = *P;
    }
  }
  Opts.OfflineRecord = OfflineRecord;
  Opts.OfflineDecisions = OfflineDecisions;
  if (!AskListLoad.empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> List =
        MemoryBuffer::getFile(AskListLoad);
    if (!List) {
      errs() << "clang-header-cache-daemon: cannot read " << AskListLoad
             << "\n";
      return 1;
    }
    SmallVector<StringRef, 0> Lines;
    (*List)->getBuffer().split(Lines, '\n', -1, /*KeepEmpty=*/false);
    Opts.FixedAskList.emplace();
    for (StringRef L : Lines)
      Opts.FixedAskList->push_back(L.str());
  }
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

  if (!AskListSave.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(AskListSave, EC);
    if (!EC)
      for (const std::string &P : (*D)->askList())
        OS << P << "\n";
  }

  // For measuring: every copy stored and how it was used.
  if (const char *Dump = std::getenv("CLANG_HEADER_CACHE_DAEMON_DUMP")) {
    std::error_code EC;
    raw_fd_ostream OS(Dump, EC);
    if (!EC)
      (*D)->dumpEntries(OS);
  }
  if (Verbose) {
    CacheDaemon::Stats S = (*D)->stats();
    outs() << "lookups " << S.Lookups << " (" << S.Hits << " with entries)\n"
           << "pushes  " << S.Pushes << " (" << S.Duplicates << " duplicate, "
           << S.Dropped << " dropped, the region full)\n"
           << "stored  " << S.Entries << " entries for " << S.Headers
           << " headers, " << (S.BytesStored / 1024)
           << " KiB: " << (S.PartBytes / 1024) << " KiB of parts, "
           << (S.MetadataBytes / 1024) << " KiB of metadata\n"
           << "timed   " << S.HeadersTimed << " headers from "
           << S.TimingReports << " compiles; " << S.HeadersListed
           << " on the ask-list; " << S.HitsTimed << " hits timed, "
           << S.HeadersDropped
           << " headers dropped as costing more than "
              "parsing; "
           << S.HeadersBelowFloor << " below the fold floor, "
           << S.HeadersFolded << " folded by their losses; "
           << S.HeadersUnlisted
           << " taken off the list by the measured cutoff\n"
           << "waits   " << S.Waits << " (" << S.WaitsServed
           << " ended by a new copy, " << S.WaitsTimedOut
           << " by the time limit, " << (S.WaitMicros / 1000) << " ms), "
           << S.WaitsGated << " lookups the gate did not hold; claims "
           << S.ClaimsGranted << " granted, " << S.ClaimsRefused
           << " refused; merges " << S.MergesClaimed << " started, "
           << S.MergesStored << " stored\n"
           << "offline " << S.BuildHints << " link hints; runs "
           << S.OfflineStarted << " started, " << S.OfflineFinished
           << " finished, " << S.OfflineFailed << " failed, "
           << S.OfflineStopped << " stopped; last finished in "
           << (S.OfflineMicros / 1000) << " ms\n";
    if (Fold)
      (*D)->dumpFolds(outs());
  }
  return 0;
}
