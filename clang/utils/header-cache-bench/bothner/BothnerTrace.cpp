// BothnerTrace: front-end time per header fragment, for the model of Bothner's
// 2003 compile server (clang/lib/ASTCache/COMPILE_SERVER_PLAN.md, 3.2).
//
// Fragments are option 2: maximal runs of tokens in a header between two
// directives the preprocessor processed, found as fragcount/FragCount.cpp
// finds them (raw lexing after the compile, with the skipped ranges and the
// conditional callbacks it reported). A header entered twice has two sets of
// fragment instances.
//
// The time-stamp counter is read at every directive callback, at every file
// entry and exit, at every top-level declaration handed to the consumer, and
// at the first token delivered to the parser after each of those. The tokens
// after it until the next such event are in the same fragment (or the main
// file): only a directive or a change of file ends a fragment. So reading the
// counter at each of them would change no attribution, and cost about 30 ns a
// token. Each interval between two events goes to one owner:
//   token -> token        (not recorded; it would go to the token's owner)
//   any -> declaration    the declaration's fragment. The parser hands a
//                         declaration over only after lexing the token after
//                         it, which may lie past the next directive; this
//                         moves that tail back to where it belongs;
//   token -> directive    split: the directive's calibrated cost goes to the
//                         directives of its file, the rest to the token's
//                         owner;
//   directive -> directive the directives of the later event's file (mostly
//                         lexing and handling the later directive);
//   directive -> token    the directives of the earlier event's file (the
//                         end of that directive, and lexing the token).
// What comes before the first event goes to the predefines.
// A directive's calibrated cost is the median, for its kind, of the intervals
// in the same compile that start at a directive event and end at one of that
// kind. "split_ns" in the output is the total of the token -> directive
// intervals, "calibrated_ns" what the calibration took out of them for the
// directives: the only part of the attribution that is estimated rather than
// measured.
//
// Output: JSON to $BOTHNER_TRACE_OUT. Without it the events are still
// recorded, so that the recording's overhead can be measured, but nothing is
// computed or written. $BOTHNER_TRACE_EVENTS also writes one line per event:
// kind, file and offset, the owner charged, and the ticks it got.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/DeclGroup.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <x86intrin.h>

using namespace clang;

namespace {

enum EvKind : uint8_t { E_TOK, E_DECL, E_DIR };
enum Dir : uint8_t {
  D_define, D_undef, D_if, D_ifdef, D_ifndef, D_elif, D_elifdef, D_elifndef,
  D_else, D_endif, D_include, D_pragma, D_ident, D_skip, D_enter, D_exit, D_N
};
const char *DirName[D_N] = {"define", "undef",   "if",      "ifdef",
                            "ifndef", "elif",    "elifdef", "elifndef",
                            "else",   "endif",   "include", "pragma",
                            "ident",  "skipped", "enter",   "exit"};

struct Ev {
  uint64_t T;
  SourceLocation::UIntTy Loc;
  EvKind Kind;
  Dir D;
};

struct State {
  std::vector<Ev> Events;
  uint64_t T0 = 0, T1 = 0;
  std::chrono::steady_clock::time_point C0, C1;
  // As FragCount records them: header entries, skipped ranges, and the
  // conditional directives the preprocessor reported.
  std::vector<std::pair<FileID, SrcMgr::CharacteristicKind>> Entries;
  llvm::DenseMap<FileID, std::vector<std::pair<unsigned, unsigned>>> Skips;
  llvm::DenseSet<SourceLocation::UIntTy> Cond;
  uint64_t Tokens = 0;
  bool InRun = false; // the last event recorded was a token
  State() { Events.reserve(1 << 16); }
  void add(EvKind K, SourceLocation L, Dir D = D_N) {
    Events.push_back({__rdtsc(), L.getRawEncoding(), K, D});
    InRun = K == E_TOK;
  }
  void token(SourceLocation L) {
    ++Tokens;
    if (!InRun)
      add(E_TOK, L);
  }
};

class CB : public PPCallbacks {
  std::shared_ptr<State> SP; // the plugin action dies before the compile ends
  State &S;
  SourceManager &SM;
  void dir(SourceLocation L, Dir D) { S.add(E_DIR, L, D); }
  void cond(SourceLocation L, Dir D) {
    S.add(E_DIR, L, D);
    S.Cond.insert(L.getRawEncoding());
  }

public:
  CB(std::shared_ptr<State> SP, SourceManager &SM) : SP(SP), S(*SP), SM(SM) {}
  void LexedFileChanged(FileID FID, LexedFileChangeReason Reason,
                        SrcMgr::CharacteristicKind FileType, FileID PrevFID,
                        SourceLocation) override {
    if (Reason == LexedFileChangeReason::EnterFile) {
      dir(SM.getLocForStartOfFile(FID), D_enter);
      S.Entries.push_back({FID, FileType});
    } else if (PrevFID.isValid()) {
      dir(SM.getLocForStartOfFile(PrevFID), D_exit);
    }
  }
  void SourceRangeSkipped(SourceRange R, SourceLocation EndifLoc) override {
    dir(R.getBegin(), D_skip);
    auto B = SM.getDecomposedLoc(R.getBegin());
    auto E = SM.getDecomposedLoc(R.getEnd());
    if (B.first != E.first)
      E.second = ~0u;
    S.Skips[B.first].push_back({B.second, E.second});
    S.Cond.insert(EndifLoc.getRawEncoding());
  }
  void If(SourceLocation L, SourceRange, ConditionValueKind) override { cond(L, D_if); }
  void Elif(SourceLocation L, SourceRange, ConditionValueKind, SourceLocation) override { cond(L, D_elif); }
  void Ifdef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L, D_ifdef); }
  void Ifndef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L, D_ifndef); }
  void Elifdef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L, D_elifdef); }
  void Elifdef(SourceLocation L, SourceRange, SourceLocation) override { cond(L, D_elifdef); }
  void Elifndef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L, D_elifndef); }
  void Elifndef(SourceLocation L, SourceRange, SourceLocation) override { cond(L, D_elifndef); }
  void Else(SourceLocation L, SourceLocation) override { cond(L, D_else); }
  void Endif(SourceLocation L, SourceLocation) override { cond(L, D_endif); }
  void MacroDefined(const Token &T, const MacroDirective *) override { dir(T.getLocation(), D_define); }
  void MacroUndefined(const Token &T, const MacroDefinition &, const MacroDirective *) override {
    dir(T.getLocation(), D_undef);
  }
  void InclusionDirective(SourceLocation HashLoc, const Token &, StringRef, bool,
                          CharSourceRange, OptionalFileEntryRef, StringRef,
                          StringRef, const Module *, bool,
                          SrcMgr::CharacteristicKind) override {
    dir(HashLoc, D_include);
  }
  void PragmaDirective(SourceLocation L, PragmaIntroducerKind K) override {
    if (K == PIK_HashPragma)
      dir(L, D_pragma);
  }
  void Ident(SourceLocation L, StringRef) override { dir(L, D_ident); }
};

// One option-2 fragment instance.
struct Frag {
  unsigned Entry, Start, End, NTok, Start3;
  uint64_t Ticks = 0, Edge = 0;
};

// Owners of time other than fragments.
enum Owner : int {
  O_MAIN = -1,     // tokens of the main file
  O_MAIN_DIR = -2, // directives of the main file
  O_HDR_DIR = -3,  // directives of headers, file entry and exit
  O_PRE = -4,      // before the main file: predefines, command line
  O_END = -5,      // after the last token: the end of the translation unit
  O_LOOSE = -6,    // a header token in no fragment (should not happen)
  O_N = 6
};
const char *OwnerName[O_N] = {"main", "main_dir", "hdr_dir", "pre", "end", "loose"};

std::string jstr(StringRef S) {
  std::string O = "\"";
  for (char C : S) {
    if (C == '"' || C == '\\') {
      O += '\\';
      O += C;
    } else if ((unsigned char)C < 0x20) {
      char B[8];
      snprintf(B, 8, "\\u%04x", C);
      O += B;
    } else {
      O += C;
    }
  }
  return O + "\"";
}

class Consumer : public ASTConsumer {
  CompilerInstance &CI;
  std::shared_ptr<State> SP;
  State &S;

  // A raw-lexed buffer item, as in FragCount: a plain token or a whole
  // directive line.
  struct Item {
    unsigned Off, NameOff;
    bool Directive, DefineOrUndef, Opens;
  };
  std::map<const char *, std::vector<Item>> Lexed; // by buffer start

  const std::vector<Item> &lex(FileID FID) {
    SourceManager &SM = CI.getSourceManager();
    StringRef Buf = SM.getBufferData(FID);
    auto It = Lexed.find(Buf.data());
    if (It != Lexed.end())
      return It->second;
    std::vector<Item> &V = Lexed[Buf.data()];
    SourceLocation Start = SM.getLocForStartOfFile(FID);
    Lexer L(Start, CI.getLangOpts(), Buf.begin(), Buf.begin(), Buf.end());
    auto off = [&](const Token &T) {
      return unsigned(T.getLocation().getRawEncoding() - Start.getRawEncoding());
    };
    Token T;
    L.LexFromRawLexer(T);
    while (T.isNot(tok::eof)) {
      if (T.is(tok::hash) && T.isAtStartOfLine()) {
        Item D{off(T), off(T), true, false, false};
        L.LexFromRawLexer(T);
        if (T.is(tok::eof) || T.isAtStartOfLine()) {
          V.push_back(D);
          continue;
        }
        D.NameOff = off(T);
        if (T.is(tok::raw_identifier)) {
          StringRef N = T.getRawIdentifier();
          D.DefineOrUndef = N == "define" || N == "undef";
          D.Opens = N == "if" || N == "ifdef" || N == "ifndef";
        }
        do
          L.LexFromRawLexer(T);
        while (T.isNot(tok::eof) && !T.isAtStartOfLine());
        V.push_back(D);
        continue;
      }
      V.push_back(Item{off(T), off(T), false, false, false});
      L.LexFromRawLexer(T);
    }
    return V;
  }

  // The option-2 fragments of one header entry, in order.
  void fragments(FileID FID, unsigned Entry, std::vector<Frag> &Out) {
    SourceManager &SM = CI.getSourceManager();
    const std::vector<Item> &V = lex(FID);
    std::vector<std::pair<unsigned, unsigned>> Sk;
    auto It = S.Skips.find(FID);
    if (It != S.Skips.end())
      Sk = It->second;
    std::sort(Sk.begin(), Sk.end());
    SourceLocation Start = SM.getLocForStartOfFile(FID);
    size_t SI = 0;
    bool Run2 = false, Run3 = false;
    unsigned Start3 = 0;
    for (const Item &I : V) {
      while (SI < Sk.size() && Sk[SI].second <= I.Off)
        ++SI;
      bool InSkip = SI < Sk.size() && Sk[SI].first < I.Off && I.Off < Sk[SI].second;
      if (!I.Directive) {
        if (InSkip)
          continue;
        if (!Run3) {
          Run3 = true;
          Start3 = I.Off;
        }
        if (!Run2) {
          Run2 = true;
          Out.push_back({Entry, I.Off, I.Off, 0, Start3});
        }
        Out.back().End = I.Off;
        ++Out.back().NTok;
        continue;
      }
      bool Processed =
          !InSkip ||
          S.Cond.count(Start.getLocWithOffset(I.NameOff).getRawEncoding());
      if (!Processed)
        continue;
      Run2 = false;
      if (!I.DefineOrUndef)
        Run3 = false;
    }
  }

public:
  Consumer(CompilerInstance &CI, std::shared_ptr<State> SP)
      : CI(CI), SP(SP), S(*SP) {}

  bool HandleTopLevelDecl(DeclGroupRef G) override {
    SourceLocation L;
    if (!G.isNull())
      L = (*G.begin())->getBeginLoc();
    S.add(E_DECL, L);
    return true;
  }

  void HandleTranslationUnit(ASTContext &) override {
    S.T1 = __rdtsc();
    S.C1 = std::chrono::steady_clock::now();
    const char *Out = getenv("BOTHNER_TRACE_OUT");
    if (!Out)
      return;
    SourceManager &SM = CI.getSourceManager();
    FileID Main = SM.getMainFileID();

    // Fragment instances, and per header FileID the first of its fragments.
    std::vector<Frag> Frags;
    llvm::DenseMap<FileID, std::pair<unsigned, unsigned>> Range; // [first, end)
    struct EntryOut {
      std::string Name, Real;
      bool Sys;
    };
    std::vector<EntryOut> Entries;
    for (auto &[FID, Kind] : S.Entries) {
      if (FID == Main)
        continue;
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID);
      if (!FE)
        continue; // predefines and builtin buffers
      unsigned First = Frags.size();
      fragments(FID, Entries.size(), Frags);
      Range[FID] = {First, unsigned(Frags.size())};
      StringRef RP = FE->getFileEntry().tryGetRealPathName();
      Entries.push_back({FE->getName().str(), RP.str(), SrcMgr::isSystem(Kind)});
    }

    auto fileOf = [&](SourceLocation::UIntTy Raw) {
      SourceLocation L = SourceLocation::getFromRawEncoding(Raw);
      if (L.isInvalid())
        return FileID();
      return SM.getFileID(SM.getExpansionLoc(L));
    };
    // The owner of a token or declaration at Raw.
    unsigned Loose = 0;
    auto ownerOf = [&](SourceLocation::UIntTy Raw, int Default) -> int {
      SourceLocation L = SourceLocation::getFromRawEncoding(Raw);
      if (L.isInvalid())
        return Default;
      auto [FID, Off] = SM.getDecomposedLoc(SM.getExpansionLoc(L));
      if (FID == Main)
        return O_MAIN;
      auto It = Range.find(FID);
      if (It == Range.end())
        return Default; // predefines, builtins, a buffer the parser made
      auto [B, E] = It->second;
      auto P = std::upper_bound(Frags.begin() + B, Frags.begin() + E, Off,
                                [](unsigned O, const Frag &F) { return O < F.Start; });
      if (P == Frags.begin() + B || Off > (P - 1)->End) {
        ++Loose;
        return O_LOOSE;
      }
      return int(P - 1 - Frags.begin());
    };
    // The directive owner of an event's file.
    auto dirOf = [&](SourceLocation::UIntTy Raw) -> int {
      FileID F = fileOf(Raw);
      if (F == Main)
        return O_MAIN_DIR;
      if (F.isValid() && SM.getFileEntryRefForID(F))
        return O_HDR_DIR;
      return O_PRE;
    };

    // Calibration: intervals from a directive event to the next directive
    // event, by the later one's kind.
    std::vector<std::vector<uint64_t>> Samples(D_N);
    std::vector<uint64_t> All;
    for (size_t I = 1; I < S.Events.size(); ++I)
      if (S.Events[I].Kind == E_DIR && S.Events[I - 1].Kind == E_DIR) {
        uint64_t D = S.Events[I].T - S.Events[I - 1].T;
        Samples[S.Events[I].D].push_back(D);
        All.push_back(D);
      }
    auto median = [](std::vector<uint64_t> &V) -> uint64_t {
      if (V.empty())
        return 0;
      std::nth_element(V.begin(), V.begin() + V.size() / 2, V.end());
      return V[V.size() / 2];
    };
    uint64_t Fallback = median(All);
    uint64_t Calib[D_N];
    for (int K = 0; K < D_N; ++K)
      Calib[K] = Samples[K].empty() ? Fallback : median(Samples[K]);

    // Attribution.
    std::unique_ptr<llvm::raw_fd_ostream> Dump;
    if (const char *P = getenv("BOTHNER_TRACE_EVENTS")) {
      std::error_code EC;
      Dump = std::make_unique<llvm::raw_fd_ostream>(P, EC);
      if (EC)
        Dump.reset();
    }
    uint64_t Own[O_N] = {}, Split = 0, Calibrated = 0;
    auto add = [&](int O, uint64_t T) {
      if (O >= 0)
        Frags[O].Ticks += T;
      else
        Own[-O - 1] += T;
      if (Dump)
        *Dump << "  -> " << (O >= 0 ? "frag " + std::to_string(O)
                                     : std::string(OwnerName[-O - 1]))
              << " " << T << "\n";
    };
    uint64_t Prev = S.T0;
    bool Parser = false; // the previous event was a token or a declaration
    int LastTok = O_PRE, LastDir = O_PRE;
    uint64_t Counts[3] = {}, DirCounts[D_N] = {};
    if (!S.Events.empty()) {
      add(O_PRE, S.Events[0].T - S.T0);
      Prev = S.Events[0].T;
    }
    for (const Ev &E : S.Events) {
      uint64_t T = E.T - Prev;
      Prev = E.T;
      ++Counts[E.Kind];
      if (Dump) {
        SourceLocation L = SourceLocation::getFromRawEncoding(E.Loc);
        *Dump << (E.Kind == E_TOK ? "tok" : E.Kind == E_DECL ? "decl" : DirName[E.D]);
        if (L.isValid()) {
          PresumedLoc P = SM.getPresumedLoc(SM.getExpansionLoc(L));
          if (P.isValid())
            *Dump << " " << llvm::sys::path::filename(P.getFilename()) << ":"
                  << P.getLine() << ":" << P.getColumn();
        }
        *Dump << "\n";
      }
      switch (E.Kind) {
      case E_TOK:
        add(Parser ? LastTok : LastDir, T);
        LastTok = ownerOf(E.Loc, LastTok);
        Parser = true;
        break;
      case E_DECL:
        add(ownerOf(E.Loc, LastTok), T);
        Parser = true;
        break;
      case E_DIR: {
        ++DirCounts[E.D];
        int O = dirOf(E.Loc);
        if (Parser) {
          uint64_t ToDir = std::min(T, Calib[E.D]);
          add(O, ToDir);
          add(LastTok, T - ToDir);
          if (LastTok >= 0)
            Frags[LastTok].Edge += T - ToDir;
          Split += T;
          Calibrated += ToDir;
        } else {
          add(O, T);
        }
        LastDir = O;
        Parser = false;
        break;
      }
      }
    }
    add(Parser ? O_END : LastDir, S.T1 - Prev);

    double Ns = 1e9 * std::chrono::duration<double>(S.C1 - S.C0).count() /
                double(S.T1 - S.T0); // nanoseconds per tick
    auto ns = [&](uint64_t T) { return (unsigned long long)(T * Ns + 0.5); };
    uint64_t FragTicks = 0;
    for (const Frag &F : Frags)
      FragTicks += F.Ticks;

    std::error_code EC;
    llvm::raw_fd_ostream OS(Out, EC);
    if (EC) {
      llvm::errs() << "bothner-trace: cannot write " << Out << "\n";
      return;
    }
    OS << "{\"ghz\":" << 1.0 / Ns << ",\"span_ns\":" << ns(S.T1 - S.T0)
       << ",\"frag_ns\":" << ns(FragTicks) << ",\"own_ns\":{";
    for (int K = 0; K < O_N; ++K)
      OS << (K ? "," : "") << "\"" << OwnerName[K] << "\":" << ns(Own[K]);
    OS << "},\"split_ns\":" << ns(Split)
       << ",\"calibrated_ns\":" << ns(Calibrated)
       << ",\"loose_tokens\":" << Loose << ",\"tokens\":" << S.Tokens
       << ",\"token_events\":" << Counts[E_TOK] << ",\"decls\":" << Counts[E_DECL]
       << ",\"dir_events\":{";
    for (int K = 0; K < D_N; ++K)
      OS << (K ? "," : "") << "\"" << DirName[K] << "\":" << DirCounts[K];
    OS << "},\"calib_ns\":{";
    for (int K = 0; K < D_N; ++K)
      OS << (K ? "," : "") << "\"" << DirName[K] << "\":" << ns(Calib[K]);
    OS << "},\"entries\":[";
    for (size_t I = 0; I < Entries.size(); ++I)
      OS << (I ? "," : "") << "[" << jstr(Entries[I].Name) << ","
         << jstr(Entries[I].Real) << "," << Entries[I].Sys << "]";
    // [entry, start, end, tokens, option-3 start, ns, edge ns]
    OS << "],\"frags\":[";
    for (size_t I = 0; I < Frags.size(); ++I) {
      const Frag &F = Frags[I];
      OS << (I ? "," : "") << "[" << F.Entry << "," << F.Start << "," << F.End
         << "," << F.NTok << "," << F.Start3 << "," << ns(F.Ticks) << ","
         << ns(F.Edge) << "]";
    }
    OS << "]}\n";
  }
};

class Action : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    auto S = std::make_shared<State>();
    S->C0 = std::chrono::steady_clock::now();
    S->T0 = __rdtsc();
    Preprocessor &PP = CI.getPreprocessor();
    // For measuring the plugin's own cost: BOTHNER_TRACE_OFF=callbacks,
    // tokens or both leaves those out.
    StringRef Off = getenv("BOTHNER_TRACE_OFF") ? getenv("BOTHNER_TRACE_OFF") : "";
    if (!Off.contains("callbacks"))
      PP.addPPCallbacks(std::make_unique<CB>(S, CI.getSourceManager()));
    State *Raw = S.get();
    if (!Off.contains("tokens"))
      PP.setTokenWatcher([Raw](const Token &T) { Raw->token(T.getLocation()); });
    return std::make_unique<Consumer>(CI, S);
  }
  bool ParseArgs(const CompilerInstance &,
                 const std::vector<std::string> &) override {
    return true;
  }
  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<Action> X("bothner-trace",
                                             "time per Bothner fragment");
