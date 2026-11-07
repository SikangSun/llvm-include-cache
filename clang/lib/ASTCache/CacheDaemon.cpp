//===- CacheDaemon.cpp ------------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/CacheDaemon.h"
#include "clang/ASTCache/DependencySerialization.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_socket_stream.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <functional>
#include <sys/wait.h>
#include <thread>

using namespace clang;
using namespace clang::ast_cache;

namespace clang {
namespace ast_cache {
struct OfflineState {
  std::mutex Mutex;
  /// The run in flight, if Running: its number, process and start.
  std::atomic<bool> Running{false};
  uint64_t Run = 0;
  llvm::sys::ProcessInfo Process;
  std::chrono::steady_clock::time_point Start;
  /// Stats's offline counters.
  uint64_t Started = 0, Finished = 0, Failed = 0, Stopped = 0, Micros = 0;

  /// Stops the run in flight. Mutex held.
  void stopLocked(llvm::StringRef Why) {
    if (!Running.load(std::memory_order_acquire))
      return;
    Running.store(false, std::memory_order_release);
    ++Stopped;
    // Not reaped yet (the waiting thread reaps it with Mutex held): the
    // process id is still its own.
    ::kill(Process.Pid, SIGTERM);
    llvm::outs() << "offline: run " << Run << " stopped by " << Why << "\n";
    llvm::outs().flush();
  }
};
} // namespace ast_cache
} // namespace clang

// A run in flight when the daemon exits goes on to write its decisions.
CacheDaemon::CacheDaemon() : Offline(std::make_shared<OfflineState>()) {}

CacheDaemon::~CacheDaemon() = default;

llvm::Expected<std::unique_ptr<CacheDaemon>>
CacheDaemon::create(llvm::StringRef RegionPath, uint64_t RegionSize,
                    Options Opts) {
  llvm::SmallString<256> Absolute(RegionPath);
  if (std::error_code EC = llvm::sys::fs::make_absolute(Absolute))
    return llvm::createStringError(EC, "resolving the region path");
  llvm::Expected<SharedRegion> R = SharedRegion::create(Absolute, RegionSize);
  if (!R)
    return R.takeError();

  std::unique_ptr<CacheDaemon> D(new CacheDaemon());
  D->Region = std::move(*R);
  D->RegionPath = std::string(Absolute);
  D->Opts = Opts;
  if (!D->Opts.OfflineRecord.empty())
    if (std::error_code EC =
            llvm::sys::fs::create_directories(D->Opts.OfflineRecord))
      return llvm::createStringError(EC, "creating the record directory");
  if (D->Opts.FixedAskList) {
    for (const std::string &P : *D->Opts.FixedAskList)
      D->listLocked(P);
    D->publishLocked();
  }
  // The picks of an earlier daemon are not this build's.
  if (D->Opts.MergePicks) {
    if (D->Opts.PicksPath.empty())
      D->Opts.PicksPath = D->RegionPath + ".picks";
    llvm::sys::fs::remove(D->Opts.PicksPath);
    llvm::sys::fs::remove(D->Opts.PicksPath + ".log");
  }
  return D;
}

std::vector<std::string> CacheDaemon::askList() const {
  std::lock_guard<std::mutex> Lock(Mutex);
  return Listed;
}

bool CacheDaemon::listLocked(llvm::StringRef Path) {
  if (Dropped.contains(Path) || !ListedSet.insert(Path).second)
    return false;
  Listed.push_back(Path.str());
  Counters.HeadersListed = Listed.size();
  return true;
}

bool CacheDaemon::dropIfCostlyLocked(llvm::StringRef Path) {
  if (!Opts.DropCostly || !ListedSet.contains(Path))
    return false;
  auto Parse = FirstTiming.find(Path);
  auto Hits = HitCosts.find(Path);
  if (Parse == FirstTiming.end() || Hits == HitCosts.end())
    return false;
  // A hit replaces the parse, and costs its query, its replay and what the
  // loaded PCM adds to every later name lookup in the compile.
  uint64_t HitCost = Hits->second.second / Hits->second.first;
  if (Parse->second >= HitCost + Opts.LaterCostMicros)
    return false;
  ListedSet.erase(Path);
  llvm::erase(Listed, Path);
  Dropped.insert(Path);
  Counters.HeadersListed = Listed.size();
  ++Counters.HeadersDropped;
  return true;
}

bool CacheDaemon::unlistLocked(llvm::StringRef Path) {
  if (!ListedSet.erase(Path))
    return false;
  llvm::erase(Listed, Path);
  Counters.HeadersListed = Listed.size();
  ++Counters.HeadersUnlisted;
  return true;
}

bool CacheDaemon::cutoffLocked(const TimingReport &Report) {
  for (const HeaderTiming &T : Report.Requests) {
    HeaderCosts &C = CostsOf[T.Path];
    ++C.Requests;
    C.RequestMicros += T.Micros;
  }
  for (const HeaderTiming &T : Report.Loads) {
    HeaderCosts &C = CostsOf[T.Path];
    ++C.Loads;
    C.LoadMicros += T.Micros;
  }
  for (const HeaderTiming &H : Report.Headers) {
    if (H.Path.empty())
      continue;
    auto [It, New] = HeldParse.try_emplace(H.Path, 0);
    It->second = H.OwnMicros + H.FoldedMicros;
    if (New)
      HeldOrder.push_back(H.Path);
  }
  bool Changed = false;
  for (const std::string &Path : HeldOrder) {
    // Every hit is a request too, so a hit saving less than a request and
    // a load never pays, however many hits there are. A cost not measured
    // for the header yet is the default, -ask-cutoff-us for its load and
    // nothing for its requests: a header no compile has asked for or loaded
    // is asked for while its parse time is at least 226 us.
    HeaderCosts C = CostsOf.lookup(Path);
    uint64_t Ask = C.Requests ? C.RequestMicros / C.Requests : 0;
    uint64_t Load = C.Loads ? C.LoadMicros / C.Loads : Opts.AskCutoffMicros;
    bool Pays = HeldParse.lookup(Path) >= Ask + Load;
    Changed |= Pays ? listLocked(Path) : unlistLocked(Path);
  }
  return Changed;
}

bool CacheDaemon::foldLocked(const TimingReport &Report) {
  const double C = Opts.FoldAskMicros, L = Opts.FoldLoadMicros,
               W = Opts.FoldWriteMicros;
  bool Changed = false;
  llvm::StringSet<> Touched;
  for (const HeaderTiming &H : Report.Headers) {
    if (H.Path.empty())
      continue;
    // The first report of a header is its first read: it was not listed, so
    // nothing was asked.
    if (!FirstTiming.count(H.Path)) {
      FoldState &F = Folds[H.Path];
      F.Own = H.OwnMicros;
      F.Children = H.Children;
      if (Opts.FixedAskList)
        continue;
      bool Passes =
          Opts.FoldListBy == Options::FoldListing::Inclusive
              ? H.Micros >= Opts.AskCutoffMicros
          : Opts.FoldListBy == Options::FoldListing::Own
              ? F.Own >= C + L
              : F.Own + F.Children * C >= C + L;
      if (Opts.FoldFloor && !Passes) {
        F.BelowFloor = true;
        ++Counters.HeadersBelowFloor;
        continue;
      }
      Changed |= listLocked(H.Path);
      continue;
    }
    if (!H.Asked)
      continue;
    auto It = Folds.find(H.Path);
    if (It == Folds.end())
      continue;
    It->second.Gain -= C + W;
    ++It->second.Misses;
    Touched.insert(H.Path);
  }
  for (const HeaderTiming &H : Report.Hits) {
    auto It = Folds.find(H.Path);
    if (It == Folds.end())
      continue;
    FoldState &F = It->second;
    F.Gain += F.Own + F.Children * C - C - L;
    ++F.Hits;
    Touched.insert(H.Path);
  }
  for (const std::string &P : Report.Links) {
    auto It = Folds.find(P);
    if (It == Folds.end())
      continue;
    It->second.Gain -= Opts.FoldLinkMicros;
    ++It->second.Links;
    Touched.insert(P);
  }
  for (const auto &T : Touched) {
    FoldState &F = Folds.find(T.getKey())->second;
    if (F.Folded || F.Gain >= -Opts.FoldBudget * W ||
        !ListedSet.contains(T.getKey()))
      continue;
    F.Folded = true;
    ListedSet.erase(T.getKey());
    llvm::erase(Listed, T.getKey());
    Dropped.insert(T.getKey());
    Counters.HeadersListed = Listed.size();
    ++Counters.HeadersFolded;
    Changed = true;
  }
  return Changed;
}

void CacheDaemon::dumpFolds(llvm::raw_ostream &OS) const {
  std::lock_guard<std::mutex> Lock(Mutex);
  OS << "fold\tstate\town_us\tchildren\thits\tmisses\tlinks\tgain_us\theader\n";
  for (const auto &KV : Folds) {
    const FoldState &F = KV.second;
    OS << "fold\t"
       << (F.BelowFloor ? "floor"
           : F.Folded   ? "folded"
           : ListedSet.contains(KV.getKey()) ? "listed"
                                             : "other")
       << "\t" << F.Own << "\t" << F.Children << "\t" << F.Hits << "\t"
       << F.Misses << "\t" << F.Links << "\t" << (int64_t)F.Gain << "\t"
       << KV.getKey() << "\n";
  }
}

void CacheDaemon::publishLocked() {
  // The whole list each time. It is at most a few hundred paths -- 778 for
  // qemu, 35 KB -- and changes only while a build's first compiles report,
  // after which membership is settled.
  std::string List;
  for (const std::string &P : Listed) {
    List += P;
    List += '\0';
  }
  // A full region leaves the previous list in place, which costs asks, never
  // correctness: the list only decides whether to ask.
  Region.publishAskList(List);
}

void CacheDaemon::learnLocked(uint64_t Build,
                              const std::vector<LearnedSearch> &Learned,
                              const std::vector<uint64_t> &Used) {
  for (uint64_t U : Used) {
    LastUsed[U] = ++UseClock;
    if (uint64_t R = RecordOf.lookup(U)) {
      Region.stampCopy(R, UseClock);
      Region.countServe(R);
    }
    Usage &X = Usages[U];
    uint64_t C = Counters.TimingReports;
    ++X.Uses;
    X.First = std::min(X.First, C);
    X.Last = std::max(X.Last, C);
    if (X.LastCompile != C) {
      ++X.Compiles;
      X.LastCompile = C;
    }
  }
  if (!Build)
    return;
  // The copies whose outcomes changed, published once each.
  llvm::SmallVector<StoredEntry *, 16> Changed;
  for (const LearnedSearch &L : Learned) {
    auto It = Table.find(L.Filename);
    if (It == Table.end())
      continue;
    for (StoredEntry &E : It->second) {
      if (E.MetadataOffset != L.Entry)
        continue;
      if (E.SearchBuild != Build) {
        E.SearchBuild = Build;
        E.Searches.clear();
      }
      if (llvm::none_of(E.Searches, [&](const SearchOutcome &O) {
            return O.Key == L.Found.Key;
          })) {
        E.Searches.push_back(L.Found);
        if (!llvm::is_contained(Changed, &E))
          Changed.push_back(&E);
      }
    }
  }
  for (StoredEntry *E : Changed) {
    std::vector<uint64_t> Pairs;
    for (const SearchOutcome &O : E->Searches) {
      Pairs.push_back(O.Key);
      Pairs.push_back(O.Outcome);
    }
    Region.publishSearches(E->Record, E->SearchBuild, Pairs);
  }
}

void CacheDaemon::dumpEntries(llvm::raw_ostream &OS) const {
  std::lock_guard<std::mutex> Lock(Mutex);
  for (const auto &[Path, Count] : LookupsOf)
    OS << "header\t" << Path << "\t" << Count << "\t"
       << (Table.count(Path) ? Table.find(Path)->second.size() : 0) << "\n";
  for (const auto &[Path, Entries] : Table)
    for (const StoredEntry &E : Entries) {
      Usage U = Usages.lookup(E.MetadataOffset);
      std::optional<MetadataView> M =
          MetadataView::open(Region.read(E.MetadataOffset, E.MetadataLength));
      OS << "entry\t" << Path << "\t" << (M ? M->moduleName() : "") << "\t"
         << E.Length << "\t" << E.MetadataLength << "\t" << U.PushedAt
         << "\t" << U.Uses << "\t" << U.Compiles << "\t"
         << (U.Uses ? U.First : 0) << "\t" << U.Last << "\t";
      if (M)
        OS << M->conditional().size() << "," << M->expandsTo().size() << ","
           << M->notAMacro().size() << "," << M->symbol().size() << ","
           << M->inclusion().size() << "," << M->files().size() << ","
           << M->includes().size() << "," << M->declarations().size() << ","
           << M->macroEffects().size() << "," << M->links().size();
      OS << "\t";
      if (M) {
        bool First = true;
        for (const ConditionalView &C : M->conditional()) {
          OS << (First ? "" : ",") << C.MacroName << (C.WasDefined ? "+" : "-");
          First = false;
        }
      }
      OS << "\n";
    }
}

llvm::SmallVector<char, 0>
CacheDaemon::handleTiming(const TimingReport &Report) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    learnLocked(Report.Build, Report.Learned, Report.Used);
    for (const TimingReport::ComboHit &C : Report.Combos)
      if (uint64_t R = RecordOf.lookup(C.Copy))
        Region.countCombo(R, C.Combo, /*Serves=*/1, C.Instructions);
    ++Counters.TimingReports;
    if (!Report.Compile.empty())
      NotListedBy[Report.Compile] = Report.NotListed;
    bool Changed = Opts.Fold && foldLocked(Report);
    if (Opts.MeasuredCutoff && !Opts.FixedAskList && !Opts.Fold)
      Changed |= cutoffLocked(Report);
    for (const HeaderTiming &H : Report.Headers) {
      if (H.Path.empty() || !FirstTiming.try_emplace(H.Path, H.Micros).second)
        continue;
      if (H.Micros >= Opts.AskCutoffMicros && !Opts.FixedAskList &&
          !Opts.Fold && !Opts.MeasuredCutoff)
        Changed |= listLocked(H.Path);
    }
    // All of a compile's hits count before any header is judged: the first
    // hit on a header loads its PCM, a later one in the same compile reuses
    // it.
    llvm::StringSet<> HitHeaders;
    for (const HeaderTiming &H : Report.Hits) {
      if (H.Path.empty())
        continue;
      auto &[Count, Total] = HitCosts[H.Path];
      ++Count;
      Total += H.Micros;
      ++Counters.HitsTimed;
      HitHeaders.insert(H.Path);
    }
    for (const auto &H : HitHeaders)
      Changed |= dropIfCostlyLocked(H.getKey());
    Counters.HeadersTimed = FirstTiming.size();
    if (Changed)
      publishLocked();
    if (Opts.MergePicks &&
        (!Report.Build || !PickBuild || Report.Build == PickBuild) &&
        pickLocked(Report))
      writePicksLocked();
  }
  llvm::SmallVector<char, 0> Ack;
  encodeTimingAck(Ack);
  return Ack;
}

/// The median of \p V, the mean of the two middle values for an even count;
/// \p Empty for none.
static double median(std::vector<double> &V, double Empty) {
  if (V.empty())
    return Empty;
  size_t M = V.size() / 2;
  std::nth_element(V.begin(), V.begin() + M, V.end());
  double Hi = V[M];
  if (V.size() % 2)
    return Hi;
  return (*std::max_element(V.begin(), V.begin() + M) + Hi) / 2;
}

bool CacheDaemon::pickLocked(const TimingReport &Report) {
  ++PickReports;
  llvm::StringSet<> Seen;
  std::string First;
  for (const TimingReport::Ask &A : Report.Asks) {
    PickCounts &P = PickOf[A.Path];
    ++P.Asks;
    if (Seen.insert(A.Path).second)
      ++P.Compiles;
    if (A.FromMain && First.empty())
      First = A.Path;
    if (!A.Hit)
      continue;
    ++P.Hits;
    P.Links += A.Links;
    P.Bytes += A.Bytes;
    P.LoadMicros += A.LoadMicros;
    PickLinks += A.Links;
    PickLoadMicros += A.LoadMicros;
  }
  if (!First.empty())
    ++PickOf[First].First;

  // The five counters of each header asked for so far: load-time share at
  // hits, copies and bytes per hit over the median header's, share of
  // compiles asking, share of its compiles where the .c file asked first.
  std::vector<double> Trees, Bytes;
  for (const auto &E : PickOf)
    if (const PickCounts &P = E.second; P.Hits) {
      Trees.push_back(double(P.Hits + P.Links) / double(P.Hits));
      Bytes.push_back(double(P.Bytes) / double(P.Hits));
    }
  double MedTree = median(Trees, 1), MedBytes = median(Bytes, 1);
  std::vector<std::pair<llvm::StringRef, std::array<double, 5>>> Rows;
  for (const auto &E : PickOf) {
    const PickCounts &P = E.second;
    std::array<double, 5> V;
    V[0] = PickLoadMicros ? double(P.LoadMicros) / double(PickLoadMicros) : 0;
    V[1] = P.Hits && MedTree
               ? double(P.Hits + P.Links) / double(P.Hits) / MedTree
               : 0;
    V[2] = P.Hits && MedBytes ? double(P.Bytes) / double(P.Hits) / MedBytes : 0;
    V[3] = double(P.Compiles) / double(PickReports);
    V[4] = P.Compiles ? double(P.First) / double(P.Compiles) : 0;
    Rows.push_back({E.getKey(), V});
  }
  // Each counter's top value: the k-th highest, k the top PickTopPercent of
  // the headers, at least one.
  std::array<double, 5> Top{};
  if (Rows.empty())
    return false;
  size_t K = std::max<size_t>(
      1, size_t(double(Rows.size()) * Opts.PickTopPercent / 100));
  K = std::min(K, Rows.size());
  for (unsigned C = 0; C != 5; ++C) {
    std::vector<double> Col;
    Col.reserve(Rows.size());
    for (const auto &R : Rows)
      Col.push_back(R.second[C]);
    std::nth_element(Col.begin(), Col.begin() + (K - 1), Col.end(),
                     std::greater<double>());
    Top[C] = Col[K - 1];
  }
  PickTops.push_back({PickReports, Top});
  // Steady: every top value within PickDrift of its value when the build
  // had 10% fewer compiles (PickTops is in the order of its counts).
  bool Steady = PickReports >= Opts.PickMinCompiles;
  if (Steady) {
    auto After = llvm::partition_point(PickTops, [&](const auto &E) {
      return double(E.first) <= 0.9 * double(PickReports);
    });
    Steady = After != PickTops.begin();
    for (unsigned C = 0; Steady && C != 5; ++C)
      Steady = std::abs(Top[C] - std::prev(After)->second[C]) <=
               Opts.PickDrift * std::max(std::abs(Top[C]), 1e-12);
  }
  Counters.PickSteadyReports += Steady;
  // While steady, the headers in the top on all five pass. By path, so that
  // the list comes in one order in every run.
  std::vector<std::pair<llvm::StringRef, std::array<double, 5>>> Now;
  for (const auto &[Path, V] : Rows) {
    bool All = Steady;
    for (unsigned C = 0; All && C != 5; ++C)
      All = V[C] >= Top[C] && V[C] > 0;
    if (All)
      Now.push_back({Path, V});
  }
  llvm::sort(Now,
             [](const auto &A, const auto &B) { return A.first < B.first; });
  std::vector<std::string> List;
  for (const auto &[Path, V] : Now)
    List.push_back(Path.str());
  if (List == Passing)
    return false;
  // For measuring: when each header came on the list and why, and when it
  // left.
  std::error_code EC;
  llvm::raw_fd_ostream Log(Opts.PicksPath + ".log", EC,
                           llvm::sys::fs::OF_Append);
  for (const auto &[Path, V] : Now) {
    if (llvm::is_contained(Passing, Path))
      continue;
    EverPassed.insert(Path);
    if (!EC) {
      Log << "on\t" << PickReports << "\t" << Path;
      for (unsigned C = 0; C != 5; ++C)
        Log << "\t" << V[C] << "/" << Top[C];
      Log << "\n";
    }
  }
  for (const std::string &Path : Passing)
    if (!EC && !llvm::is_contained(List, Path))
      Log << "off\t" << PickReports << "\t" << Path << "\n";
  Passing = std::move(List);
  Counters.Picks = EverPassed.size();
  return true;
}

void CacheDaemon::writePicksLocked() {
  // Whole, under a new name, then renamed: a compile reads either the old
  // list or the new one.
  std::string Tmp = Opts.PicksPath + ".tmp";
  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(Tmp, EC);
    if (EC)
      return;
    for (const std::string &P : Passing)
      if (P.find_first_of("\t\n") == std::string::npos)
        OS << "merge-root\t" << P << "\n";
  }
  llvm::sys::fs::rename(Tmp, Opts.PicksPath);
}

llvm::SmallVector<char, 0>
CacheDaemon::handleHello(uint64_t Conn, const HelloRequest &Hello) {
  const bool Offline = Hello.Offline;
  HelloReply Reply{RegionPath, Region.instanceId()};
  // A compile the offline step runs records nothing, and takes the
  // decisions it is given.
  if (Offline) {
    std::lock_guard<std::mutex> Lock(Mutex);
    OfflineConns.insert(Conn);
  }
  // A compile built before skips the headers it skipped then, so that it
  // asks for the same headers as then, and finds the copies it wrote.
  if (!Hello.Compile.empty()) {
    std::lock_guard<std::mutex> Lock(Mutex);
    auto It = NotListedBy.find(Hello.Compile);
    if (It != NotListedBy.end())
      Reply.NotListedBefore = It->second;
  }
  // Before the offline step wrote its decisions, compiles record what it
  // reads; after, they take the decisions.
  if (!Opts.OfflineDecisions.empty() && !Offline) {
    if (llvm::sys::fs::exists(Opts.OfflineDecisions))
      Reply.DecisionsPath = Opts.OfflineDecisions;
    else
      Reply.RecordDir = Opts.OfflineRecord;
  }
  // The five-counter merge pick: the compile lists its requests, and takes
  // the headers that pass now, unless it has the offline step's decisions.
  // Only in the first build a compile names (user 2026-11-06: the merge
  // heuristic runs in the first build only); a compile that names none is
  // taken to be of that build.
  if (Opts.MergePicks && !Offline) {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!PickBuild)
      PickBuild = Hello.Build;
    if (!Hello.Build || Hello.Build == PickBuild) {
      Reply.WantAsks = true;
      if (Reply.DecisionsPath.empty() && !Passing.empty())
        Reply.DecisionsPath = Opts.PicksPath;
    }
  }
  llvm::SmallVector<char, 0> Out;
  encodeHelloReply(Reply, Out);
  return Out;
}

llvm::SmallVector<char, 0> CacheDaemon::handleBuildHint(const BuildHint &Hint) {
  llvm::SmallVector<char, 0> Ack;
  encodeBuildHintAck(Ack);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    ++Counters.BuildHints;
  }
  if (Opts.OfflineCommand.empty() || Opts.OfflineRecord.empty() ||
      Opts.OfflineDecisions.empty() ||
      llvm::sys::fs::exists(Opts.OfflineDecisions))
    return Ack;
  std::shared_ptr<OfflineState> S = Offline;
  std::lock_guard<std::mutex> Lock(S->Mutex);
  // The newest hint wins: a run in flight read a record the build was still
  // adding to.
  S->stopLocked("a later hint");
  std::string Log = Opts.OfflineDecisions + ".log";
  // The run's output starts the log afresh; the run's line follows it.
  llvm::sys::fs::remove(Log);
  llvm::SmallVector<llvm::StringRef, 8> Args(Opts.OfflineCommand.begin(),
                                             Opts.OfflineCommand.end());
  Args.push_back(Opts.OfflineRecord);
  Args.push_back(Opts.OfflineDecisions);
  std::optional<llvm::StringRef> Redirects[] = {llvm::StringRef(),
                                                llvm::StringRef(Log),
                                                llvm::StringRef(Log)};
  std::string Err;
  bool Failed = false;
  llvm::sys::ProcessInfo P =
      llvm::sys::ExecuteNoWait(Args[0], Args, std::nullopt, Redirects,
                               /*MemoryLimit=*/0, &Err, &Failed);
  if (Failed || !P.Pid) {
    llvm::errs() << "clang-header-cache-daemon: offline step not started: "
                 << Err << "\n";
    ++S->Failed;
    return Ack;
  }
  uint64_t Run = ++S->Run;
  ++S->Started;
  S->Process = P;
  S->Start = std::chrono::steady_clock::now();
  S->Running.store(true, std::memory_order_release);
  llvm::outs() << "offline: run " << Run << " started after the link of "
               << Hint.Output << "\n";
  llvm::outs().flush();
  // The thread holds the state, not the daemon: the daemon can exit first.
  std::thread([S, P, Run, Log, Decisions = Opts.OfflineDecisions] {
    // Waits without reaping, so that until Mutex is taken a stop's signal
    // reaches this process and no other given its id.
    siginfo_t Info{};
    while (::waitid(P_PID, P.Pid, &Info, WEXITED | WNOWAIT) == -1 &&
           errno == EINTR)
      ;
    std::lock_guard<std::mutex> Lock(S->Mutex);
    ::waitpid(P.Pid, nullptr, 0);
    int Code = Info.si_code == CLD_EXITED ? Info.si_status : -1;
    // Stopped: counted where it was stopped.
    if (S->Run != Run || !S->Running.load(std::memory_order_acquire))
      return;
    S->Running.store(false, std::memory_order_release);
    uint64_t Micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - S->Start)
                          .count();
    bool Ok = Code == 0 && llvm::sys::fs::exists(Decisions);
    ++(Ok ? S->Finished : S->Failed);
    if (Ok)
      S->Micros = Micros;
    std::error_code EC;
    llvm::raw_fd_ostream OS(Log, EC, llvm::sys::fs::OF_Append);
    if (!EC)
      OS << "offline run " << Run << ": " << Micros << " us wall, exit "
         << Code << "\n";
    llvm::outs() << "offline: run " << Run << " " << (Ok ? "finished" : "failed")
                 << " in " << Micros / 1000 << " ms (exit " << Code << ")\n";
    llvm::outs().flush();
  }).detach();
  return Ack;
}

void CacheDaemon::stopOffline(llvm::StringRef Why) {
  std::lock_guard<std::mutex> Lock(Offline->Mutex);
  Offline->stopLocked(Why);
}

void CacheDaemon::endWritingLocked(uint64_t Conn, llvm::StringRef Filename) {
  auto End = [&](Writers &W) {
    size_t Before = W.List.size();
    llvm::erase_if(W.List, [&](const Writer &X) { return X.Conn == Conn; });
    if (W.List.size() != Before)
      W.Arrived.notify_all();
  };
  if (!Filename.empty()) {
    if (auto It = WritersOf.find(Filename); It != WritersOf.end())
      End(*It->second);
    return;
  }
  for (auto &KV : WritersOf)
    End(*KV.second);
}

void CacheDaemon::settleWaitLocked(uint64_t Conn,
                                   const std::vector<uint64_t> &Used) {
  auto It = WaitedFor.find(Conn);
  if (It == WaitedFor.end())
    return;
  WaitHistory &H = WaitsOf[It->second.first];
  bool Fit = llvm::any_of(It->second.second, [&](uint64_t Offset) {
    return llvm::is_contained(Used, Offset);
  });
  ++(Fit ? H.Fit : H.Missed);
  WaitedFor.erase(It);
}

bool CacheDaemon::waitForWriterLocked(std::unique_lock<std::mutex> &Lock,
                                      const LookupRequest &Req,
                                      uint64_t Conn) {
  auto W = WritersOf.find(Req.Filename);
  if (W == WritersOf.end())
    return false;
  Writers *Ws = W->second.get();
  auto Other = [&] {
    return llvm::any_of(Ws->List, [&](const Writer &X) {
      return X.ClientId != Req.ClientId;
    });
  };
  if (!Other())
    return false;
  // The gate: once a few waits for this header ended in a copy the waiting
  // compile could not use, its copies differ from compile to compile, and
  // parsing beats waiting.
  if (auto H = WaitsOf.find(Req.Filename); H != WaitsOf.end()) {
    unsigned Checked = H->second.Fit + H->second.Missed;
    if (Checked >= 3 && 2 * H->second.Fit < Checked) {
      ++Counters.WaitsGated;
      return false;
    }
  }
  // A writer's copy arrives once it has parsed the header and written the
  // copy; it began before this compile asked, so waiting for it ends sooner
  // than parsing and writing here would. Twice the header's parse time,
  // from the earliest writer's start, covers both, unless it stalled.
  auto Parse = FirstTiming.find(Req.Filename);
  if (Parse == FirstTiming.end() || !Parse->second)
    return false;
  auto Stored = [&] {
    auto T = Table.find(Req.Filename);
    return T == Table.end() ? size_t(0) : T->second.size();
  };
  size_t Before = Stored();
  auto Start = std::chrono::steady_clock::now();
  auto Earliest = Start;
  for (const Writer &X : Ws->List)
    if (X.ClientId != Req.ClientId)
      Earliest = std::min(Earliest, X.Since);
  auto Deadline =
      std::max(Start + std::chrono::milliseconds(1),
               Earliest + 2 * std::chrono::microseconds(Parse->second));
  ++Counters.Waits;
  while (Other() && Stored() == Before)
    if (Ws->Arrived.wait_until(Lock, Deadline) == std::cv_status::timeout) {
      ++Counters.WaitsTimedOut;
      break;
    }
  if (size_t After = Stored(); After != Before) {
    ++Counters.WaitsServed;
    std::vector<uint64_t> New;
    const std::vector<StoredEntry> &Entries = Table.find(Req.Filename)->second;
    for (size_t I = Before; I != After; ++I)
      New.push_back(Entries[I].MetadataOffset);
    WaitedFor[Conn] = {Req.Filename, std::move(New)};
  }
  Counters.WaitMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - Start)
                             .count();
  return true;
}

llvm::SmallVector<char, 0>
CacheDaemon::handleLookup(const LookupRequest &Req, uint64_t Conn) {
  LookupResponse Resp;
  {
    std::unique_lock<std::mutex> Lock(Mutex);
    settleWaitLocked(Conn, Req.Used);
    learnLocked(Req.Build, Req.Learned, Req.Used);
    ++Counters.Lookups;
    ++LookupsOf[Req.Filename];
    // CLANG_HEADER_CACHE_WAIT: a copy another compile is writing may serve
    // this one; the reply then holds it.
    if (Req.Wait)
      Resp.Waited = waitForWriterLocked(Lock, Req, Conn);
    // Measurement (CLANG_HEADER_CACHE_PROBE_REUSE): "*<suffix>" asks for
    // the entries of every name ending in the suffix.
    std::vector<StoredEntry> Matching;
    llvm::StringRef Suffix;
    if (Req.Filename.size() > 1 && Req.Filename[0] == '*') {
      Suffix = llvm::StringRef(Req.Filename).drop_front();
      for (const auto &KV : Table)
        if (KV.getKey().ends_with(Suffix))
          llvm::append_range(Matching, KV.getValue());
    }
    auto It = Table.find(Req.Filename);
    if (!Suffix.empty()) {
      for (const StoredEntry &E : Matching)
        Resp.Entries.push_back({E.MetadataOffset, E.MetadataLength, E.Offset,
                                E.Length, E.Hash, std::vector<SearchOutcome>()});
    } else if (It != Table.end()) {
      ++Counters.Hits;
      // Where every stored entry for the name is, with no comparison of its
      // metadata here. The daemon has no way to evaluate a dependency --
      // that needs the client's live macro table and its Sema -- and sending
      // the client's whole state here instead would be far larger than the
      // handful of names a header actually depends on.
      // The most recently used first: a compile checks them in order and
      // takes the first that fits, which is most often the one that fit
      // the compile before it (REPORT.md I15).
      auto Recent = [&](const std::vector<StoredEntry> &Stored,
                        std::vector<CacheEntryRef> &Out) {
        std::vector<const StoredEntry *> Order;
        Order.reserve(Stored.size());
        for (const StoredEntry &E : Stored)
          Order.push_back(&E);
        std::stable_sort(Order.begin(), Order.end(),
                         [&](const StoredEntry *A, const StoredEntry *B) {
                           return LastUsed.lookup(A->MetadataOffset) >
                                  LastUsed.lookup(B->MetadataOffset);
                         });
        Out.reserve(Order.size());
        for (const StoredEntry *E : Order)
          Out.push_back({E->MetadataOffset, E->MetadataLength, E->Offset,
                         E->Length, E->Hash,
                         Req.Build && E->SearchBuild == Req.Build
                             ? E->Searches
                             : std::vector<SearchOutcome>()});
      };
      Recent(It->second, Resp.Entries);
      // CLANG_HEADER_CACHE_PIECES: the versions of its copies' sections, in
      // the same reply.
      if (auto V = VersionsOf.find(Req.Filename); V != VersionsOf.end())
        for (const std::string &Name : V->second) {
          auto Stored = Table.find(Name);
          if (Stored == Table.end())
            continue;
          LookupResponse::SectionVersions &Out = Resp.Versions.emplace_back();
          Out.Section = Name.substr(Req.Filename.size() + 9);
          Recent(Stored->second, Out.Entries);
        }
    }
  }

  llvm::SmallVector<char, 0> Out;
  encodeLookupResponse(Resp, Out);
  return Out;
}

llvm::SmallVector<char, 0> CacheDaemon::handlePush(const ReceivedPush &Req,
                                                   uint64_t Conn) {
  uint64_t Hash = llvm::xxh3_64bits(Req.Metadata);
  std::optional<MetadataView> V = MetadataView::open(Req.Metadata);
  std::string Name = V ? V->moduleName().str() : std::string();
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    // Whether stored or a duplicate, a copy of this header now exists: the
    // pushing compile's writing ends, and a claim on the copy is settled.
    auto Done = [&] {
      if (!Name.empty()) {
        PushedNames.insert(Name);
        Claimed.erase(Name);
      }
      endWritingLocked(Conn, Req.Filename);
    };
    ++Counters.Pushes;
    auto [Slot, NewName] = Table.try_emplace(Req.Filename);
    std::vector<StoredEntry> &Entries = Slot->second;
    // CLANG_HEADER_CACHE_PIECES: a version of a section, kept with the
    // header whose copy holds the section.
    if (NewName) {
      size_t At = Req.Filename.find("#section:");
      if (At != std::string::npos)
        VersionsOf[Req.Filename.take_front(At)].push_back(
            Req.Filename.str());
    }

    for (const StoredEntry &E : Entries) {
      if (E.Hash == Hash &&
          Region.read(E.MetadataOffset, E.MetadataLength) == Req.Metadata) {
        // Another client stored this exact result while this one was still
        // compiling. Discard it and acknowledge: the client that lost the
        // race already holds its own result and has no use for the winner.
        ++Counters.Duplicates;
        if (auto M = MergeOf.find(Conn); M != MergeOf.end() && E.Record &&
                                          M->second.second == Req.Filename) {
          Region.setMerged(M->second.first, E.Record);
          MergeOf.erase(M);
          ++Counters.MergesStored;
        }
        Done();
        llvm::SmallVector<char, 0> Ack;
        encodePushAck(Ack);
        return Ack;
      }
    }

    // An entry is recorded only with its metadata, which every compile reads
    // in place in the region, as it reads the copy.
    std::optional<uint64_t> MetadataAt = Region.append(Req.Metadata);
    std::optional<uint64_t> Offset =
        MetadataAt ? Region.append(Req.Data) : std::nullopt;
    // A copy is stored only with its record in the index, which compiles
    // read in place of asking.
    std::optional<uint64_t> Record;
    if (Offset) {
      IndexedCopy C{};
      C.MetadataOffset = *MetadataAt;
      C.MetadataLength = Req.Metadata.size();
      C.Offset = *Offset;
      C.Length = Req.Data.size();
      C.MetadataHash = Hash;
      Record = Region.indexCopy(Req.Filename, C);
    }
    // A merged copy is checked just before the copy whose tree it holds.
    if (Record)
      if (auto M = MergeOf.find(Conn);
          M != MergeOf.end() && M->second.second == Req.Filename) {
        Region.setMerged(M->second.first, *Record);
        MergeOf.erase(M);
        ++Counters.MergesStored;
      }
    if (Record) {
      StoredEntry E;
      E.Hash = Hash;
      E.MetadataOffset = *MetadataAt;
      E.MetadataLength = Req.Metadata.size();
      E.Offset = *Offset;
      E.Length = Req.Data.size();
      E.Record = *Record;
      RecordOf[*MetadataAt] = *Record;
      Usages[*MetadataAt].PushedAt = Counters.TimingReports;
      Entries.push_back(std::move(E));
      Counters.BytesStored += Req.Data.size() + Req.Metadata.size();
      Counters.PartBytes += Req.Data.size();
      Counters.MetadataBytes += Req.Metadata.size();
      ++Counters.Entries;
      Counters.Headers = Table.size();
      if (Opts.ListStored && listLocked(Req.Filename))
        publishLocked();
    } else {
      ++Counters.Dropped;
    }
    Done();
    // A full region is not an error the client can act on -- it already has
    // its own result -- so a failed append is acknowledged like any other
    // push and the entry is simply not recorded.
  }

  llvm::SmallVector<char, 0> Ack;
  encodePushAck(Ack);
  return Ack;
}

llvm::SmallVector<char, 0>
CacheDaemon::handleNotice(const WritingNotice &Notice, uint64_t Conn) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Notice.Ended) {
      endWritingLocked(Conn, Notice.Filename);
    } else {
      std::unique_ptr<Writers> &W = WritersOf[Notice.Filename];
      if (!W)
        W = std::make_unique<Writers>();
      W->List.push_back(
          {Conn, Notice.ClientId, std::chrono::steady_clock::now()});
    }
  }
  llvm::SmallVector<char, 0> Ack;
  encodeNoticeAck(Ack);
  return Ack;
}

llvm::SmallVector<char, 0> CacheDaemon::handleClaim(const ClaimRequest &Req,
                                                    uint64_t Conn) {
  bool Granted = true;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    // CLANG_HEADER_CACHE_MERGE: "merge:<record>:<combination>", once per
    // combination of a copy's links. Given back, by a compile whose merged
    // copy was declined, it is not merged again.
    if (llvm::StringRef(Req.ModuleName).starts_with("merge:")) {
      auto [R, C] = llvm::StringRef(Req.ModuleName).drop_front(6).split(':');
      uint64_t Record = 0, Combo = 0;
      std::optional<uint64_t> At;
      if (Req.Release) {
        if (auto M = MergeOf.find(Conn);
            M != MergeOf.end() && M->second.second == Req.Filename) {
          MergeOf.erase(M);
          // A declined merge leaves the header free to merge another
          // combination.
          MergedHeaders.erase(Req.Filename);
        }
      } else if (Opts.PickOneMerge && MergedHeaders.contains(Req.Filename)) {
        ++Counters.MergesRefusedOnce;
      } else if (!R.getAsInteger(16, Record) && !C.getAsInteger(16, Combo)) {
        At = Region.claimMerge(Record, Combo);
      }
      Granted = At.has_value();
      if (Granted) {
        MergeOf[Conn] = {*At, Req.Filename};
        ++Counters.MergesClaimed;
        if (Opts.PickOneMerge)
          MergedHeaders.insert(Req.Filename);
      }
      llvm::SmallVector<char, 0> Out;
      encodeClaimReply(Granted, Out);
      return Out;
    }
    if (Req.Release) {
      if (auto It = Claimed.find(Req.ModuleName);
          It != Claimed.end() && It->second == Conn)
        Claimed.erase(It);
    } else if (PushedNames.contains(Req.ModuleName) ||
               Claimed.contains(Req.ModuleName)) {
      Granted = false;
      ++Counters.ClaimsRefused;
    } else {
      Claimed[Req.ModuleName] = Conn;
      ++Counters.ClaimsGranted;
    }
  }
  llvm::SmallVector<char, 0> Out;
  encodeClaimReply(Granted, Out);
  return Out;
}

void CacheDaemon::connectionClosed(uint64_t Conn) {
  std::lock_guard<std::mutex> Lock(Mutex);
  OfflineConns.erase(Conn);
  endWritingLocked(Conn, /*Filename=*/{});
  // A claim still held when its compile ends is for a copy it wrote: one
  // declined is given back. Claims by the whole dependency list
  // (CLANG_HEADER_CACHE_CLAIM=deps) are not module names, which pushes mark.
  for (auto It = Claimed.begin(); It != Claimed.end();) {
    auto Next = std::next(It);
    if (It->second == Conn) {
      PushedNames.insert(It->getKey());
      Claimed.erase(It);
    }
    It = Next;
  }
  WaitedFor.erase(Conn);
  MergeOf.erase(Conn);
}

std::optional<llvm::SmallVector<char, 0>>
CacheDaemon::handleMessage(llvm::StringRef Body, uint64_t Conn) {
  std::optional<MessageKind> Kind = peekMessageKind(Body);
  if (!Kind)
    return std::nullopt;

  switch (*Kind) {
  case MessageKind::LookupRequest:
    // A lookup: the build goes on, and the offline step's record with it.
    // The offline step's own compiles look up too.
    if (Offline->Running.load(std::memory_order_relaxed)) {
      bool Own;
      {
        std::lock_guard<std::mutex> Lock(Mutex);
        Own = OfflineConns.contains(Conn);
      }
      if (!Own)
        stopOffline("a lookup");
    }
    if (std::optional<LookupRequest> Req = decodeLookupRequest(Body))
      return handleLookup(*Req, Conn);
    return std::nullopt;
  case MessageKind::WritingNotice:
    if (std::optional<WritingNotice> Notice = decodeWritingNotice(Body))
      return handleNotice(*Notice, Conn);
    return std::nullopt;
  case MessageKind::ClaimRequest:
    if (std::optional<ClaimRequest> Req = decodeClaimRequest(Body))
      return handleClaim(*Req, Conn);
    return std::nullopt;
  case MessageKind::PushRequest:
    // The metadata is checked whole once, here, outside the lock; every
    // compile after reads it as stored. Metadata that is not well formed
    // must not be offered: a candidate missing constraints matches compiles
    // it must not.
    if (std::optional<ReceivedPush> Req = decodePushRequest(Body))
      if (std::optional<MetadataView> V = MetadataView::open(Req->Metadata))
        if (V->validate())
          return handlePush(*Req, Conn);
    return std::nullopt;
  case MessageKind::TimingReport:
    if (std::optional<TimingReport> Report = decodeTimingReport(Body)) {
      {
        std::lock_guard<std::mutex> Lock(Mutex);
        settleWaitLocked(Conn, Report->Used);
      }
      return handleTiming(*Report);
    }
    return std::nullopt;
  case MessageKind::Hello:
    if (std::optional<HelloRequest> Hello = decodeHello(Body))
      return handleHello(Conn, *Hello);
    return std::nullopt;
  case MessageKind::BuildHint:
    if (std::optional<BuildHint> Hint = decodeBuildHint(Body))
      return handleBuildHint(*Hint);
    return std::nullopt;
  case MessageKind::SeenRequest:
    if (std::optional<SeenRequest> Req = decodeSeenRequest(Body)) {
      bool Seen;
      {
        std::lock_guard<std::mutex> Lock(Mutex);
        Seen = !SeenOnce.insert(Req->Filename + '\0' + Req->ModuleName).second;
        ++(Seen ? Counters.SeenAgain : Counters.SeenFirst);
      }
      llvm::SmallVector<char, 0> Out;
      encodeSeenReply(Seen, Out);
      return Out;
    }
    return std::nullopt;
  case MessageKind::SeenReply:
  case MessageKind::NoticeAck:
  case MessageKind::ClaimReply:
  case MessageKind::LookupResponse:
  case MessageKind::PushAck:
  case MessageKind::TimingAck:
  case MessageKind::HelloReply:
  case MessageKind::BuildHintAck:
    // Reply kinds; a client sending one is talking nonsense.
    return std::nullopt;
  }
  return std::nullopt;
}

CacheDaemon::Stats CacheDaemon::stats() const {
  Stats S;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    S = Counters;
  }
  std::lock_guard<std::mutex> Lock(Offline->Mutex);
  S.OfflineStarted = Offline->Started;
  S.OfflineFinished = Offline->Finished;
  S.OfflineFailed = Offline->Failed;
  S.OfflineStopped = Offline->Stopped;
  S.OfflineMicros = Offline->Micros;
  return S;
}

void CacheDaemon::stop() {
  Stopping.store(true, std::memory_order_release);
  if (auto *L = static_cast<llvm::ListeningSocket *>(Listener))
    L->shutdown();
}

llvm::Error CacheDaemon::run(llvm::StringRef SocketPath) {
  if (llvm::Error E = checkSocketPath(SocketPath))
    return E;
  // A socket file left behind by a previous run would make bind fail.
  llvm::sys::fs::remove(SocketPath);

  llvm::Expected<llvm::ListeningSocket> L =
      llvm::ListeningSocket::createUnix(SocketPath);
  if (!L)
    return L.takeError();
  Listener = &*L;

  uint64_t Connections = 0;
  while (!Stopping.load(std::memory_order_acquire)) {
    llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> Conn = L->accept();
    if (!Conn) {
      llvm::consumeError(Conn.takeError());
      // A failed accept during shutdown is expected; otherwise keep serving.
      continue;
    }

    // A connection's start is the daemon accepting a socket and its end is
    // the socket closing -- there are no connect or disconnect messages.
    std::thread([this, S = std::move(*Conn), Id = ++Connections]() mutable {
      while (std::optional<std::string> Body = receiveMessage(*S)) {
        std::optional<llvm::SmallVector<char, 0>> Reply =
            handleMessage(*Body, Id);
        if (!Reply)
          break;
        if (!sendMessage(*S, *Reply))
          break;
      }
      connectionClosed(Id);
    }).detach();
  }

  Listener = nullptr;
  L->shutdown();
  return llvm::Error::success();
}
