//===- ClangHeaderCachePopulate.cpp - Populate the header cache -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Builds saved copies of headers and, if a daemon is running, pushes them.
// With -dry-run it pushes nothing and only reports how many headers could be
// saved, how large the results and records are, and why the rest could not.
//
// Usage:
//   clang-header-cache-populate [options] <header>...
//   clang-header-cache-populate [options] -job <file>
//
// A header that relies on its includer -- redis's anet.h uses uint32_t and
// never includes <stdint.h> -- needs the context the real build gave it. Pass
// what to put in front of it, in order, with -context: a path, or an #include
// spelling such as '<stdint.h>'. A spelling is replayed as written, so that
// #include_next inside the file searches as it did in the real build. A
// compile that misses on a header writes the same information to a job file,
// read here with -job.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/ASTCacheProducer.h"
#include "clang/ASTCache/CacheProtocol.h"
#include "clang/ASTCache/DependencySerialization.h"
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
                                     cl::ZeroOrMore, cl::cat(Category));

/// Repeatable rather than a trailing "-- args" list: cl::ConsumeAfter takes
/// over the positional list and leaves only the first header behind.
static cl::list<std::string>
    CompilerArgs("Xcc",
                 cl::desc("Pass an argument to the compile, e.g. -Xcc -I. "
                          "-Xcc -DFOO=1. Repeatable. With -job, added after "
                          "each job's own flags."),
                 cl::value_desc("arg"), cl::cat(Category));

static cl::list<std::string>
    ContextFiles("context",
                 cl::desc("What to put in front of each header, as the real "
                          "build had it: a path, or an #include spelling "
                          "such as '<stdint.h>'. Repeatable; order is kept."),
                 cl::value_desc("file-or-spelling"), cl::cat(Category));

static cl::opt<std::string>
    JobFile("job",
            cl::desc("Read headers, flags, and context from a job file "
                     "written by a compile that missed"),
            cl::value_desc("file"), cl::cat(Category));

static cl::opt<std::string>
    SocketPath("socket", cl::desc("Daemon socket; ignored with -dry-run"),
               cl::init(DefaultSocketPath), cl::cat(Category));

static cl::opt<bool>
    DryRun("dry-run",
           cl::desc("Report what would be saved without contacting a daemon"),
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
  uint64_t NameBytes = 0;
  unsigned Conditional = 0, Symbol = 0, Inclusion = 0;
  /// Syntactic dependencies, and how many of them are NotAMacro names.
  unsigned Syntactic = 0, NotAMacro = 0;
  /// Decline reasons, counted. The distribution is the actual finding: it
  /// says which limitation costs the most coverage.
  std::map<std::string, unsigned> Reasons;
};

void report(const Totals &T, unsigned Total) {
  auto pct = [&](unsigned N) { return Total ? (100.0 * N / Total) : 0.0; };

  outs() << "\n" << Total << " headers\n"
         << "  saved       " << T.Produced
         << format("  (%.1f%%)\n", pct(T.Produced)) << "  declined    "
         << T.Declined << format("  (%.1f%%)\n", pct(T.Declined))
         << "  failed      " << T.Failed
         << format("  (%.1f%%)\n", pct(T.Failed));

  if (T.Produced) {
    outs() << "\nper saved header, mean:\n"
           << format("  saved copy      %8.1f KiB\n",
                     T.ResultBytes / 1024.0 / T.Produced)
           << format("  record          %8.1f bytes in a lookup reply, "
                     "%.1f bytes of names in the region\n",
                     double(T.RecordBytes) / T.Produced,
                     double(T.NameBytes) / T.Produced)
           << format("  dependencies    %8.1f conditional, %.1f syntactic "
                     "(%.1f not a macro), %.1f symbol, %.1f inclusion\n",
                     double(T.Conditional) / T.Produced,
                     double(T.Syntactic) / T.Produced,
                     double(T.NotAMacro) / T.Produced,
                     double(T.Symbol) / T.Produced,
                     double(T.Inclusion) / T.Produced);
  }

  if (!T.Reasons.empty()) {
    outs() << "\nwhy headers were not saved:\n";
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
      "Builds saved copies of headers and stores them in the daemon.\n\n"
      "With -dry-run, reports how many headers could be saved and why the\n"
      "rest could not, without needing a daemon.\n\n"
      "Pass the build's own flags with -Xcc, e.g. -Xcc -I. -Xcc -DHAVE_X=1,\n"
      "and the files the build had included before a header with -context.\n");

  std::vector<ProduceRequest> Requests;
  if (!JobFile.empty()) {
    if (!Headers.empty() || !ContextFiles.empty()) {
      errs() << "-job cannot be combined with headers or -context: the job "
                "file carries both\n";
      return 1;
    }
    Expected<std::vector<ProduceRequest>> Jobs = readJobFile(JobFile);
    if (!Jobs) {
      errs() << "cannot use job file " << JobFile << ": "
             << toString(Jobs.takeError()) << "\n";
      return 1;
    }
    Requests = std::move(*Jobs);
    // A compile records its jobs without the driver's flags, which only the
    // build that ran it knows; the build passes them here.
    for (ProduceRequest &R : Requests)
      R.Args.insert(R.Args.end(), CompilerArgs.begin(), CompilerArgs.end());
  } else {
    for (const std::string &H : Headers) {
      ProduceRequest R;
      R.Header.Path = H;
      R.Args.assign(CompilerArgs.begin(), CompilerArgs.end());
      for (const std::string &C : ContextFiles) {
        IncludeRef I;
        // Nothing here resolves a spelling, so the file it opens is not
        // checked; a job file from a real compile carries both.
        if (StringRef(C).starts_with("<") || StringRef(C).starts_with("\""))
          I.Spelled = C;
        else
          I.Path = C;
        R.Context.push_back(std::move(I));
      }
      Requests.push_back(std::move(R));
    }
  }
  if (Requests.empty()) {
    errs() << "nothing to do: name headers, or pass -job <file>\n";
    return 1;
  }

  std::unique_ptr<raw_socket_stream> Conn;
  if (!DryRun) {
    if (Error E = checkSocketPath(SocketPath)) {
      errs() << toString(std::move(E)) << "\n";
      return 1;
    }
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
  for (const ProduceRequest &Req : Requests) {
    ProduceResult R = produceHeader(Req);

    switch (R.Stat) {
    case ProduceResult::Status::Produced: {
      ++T.Produced;
      T.ResultBytes += R.Push.Data.size();
      T.Conditional += R.Push.Deps.Conditional.size();
      T.Syntactic += R.Push.Deps.Syntactic.size();
      for (const SyntacticDependency &D : R.Push.Deps.Syntactic)
        T.NotAMacro += D.Required == SyntacticDependency::State::NotAMacro;
      T.Symbol += R.Push.Deps.Symbol.size();
      T.Inclusion += R.Push.Deps.Inclusion.size();

      // A LOOKUP reply carries one record per candidate, less its NotAMacro
      // names, which the daemon keeps in the region; both sizes are worth
      // reporting separately from the saved copy's.
      HeaderDependencySet InReply = R.Push.Deps;
      T.NameBytes += extractNotAMacroNames(InReply).size();
      SmallVector<char, 0> Encoded;
      encodeDependencySet(InReply, Encoded);
      T.RecordBytes += Encoded.size();

      SmallVector<char, 0> Msg;
      encodePushRequest(R.Push, Msg);

      if (Conn) {
        if (!sendMessage(*Conn, Msg)) {
          errs() << "lost the daemon connection while pushing "
                 << Req.Header.Path << "\n";
          return 1;
        }
        if (!receiveMessage(*Conn)) {
          errs() << "no acknowledgment from the daemon for "
                 << Req.Header.Path << "\n";
          return 1;
        }
      }
      break;
    }
    case ProduceResult::Status::Declined:
      ++T.Declined;
      ++T.Reasons[R.Reason];
      if (ListDeclines)
        outs() << "declined  " << Req.Header.Path << "  -- " << R.Reason
               << "\n";
      break;
    case ProduceResult::Status::Failed:
      ++T.Failed;
      ++T.Reasons["FAILED: " + R.Reason];
      errs() << "failed    " << Req.Header.Path << "  -- " << R.Reason
             << "\n";
      break;
    }
  }

  report(T, Requests.size());
  return 0;
}
