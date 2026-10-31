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
//
// With $BOTHNER_TRACE_RECORD set, the run records what the model's decisions
// need instead of times: every token, so that each fragment gets a hash of the
// tokens the parser received, and at each directive event whether the parser
// was between declarations. Its times mean nothing. Every run also writes the
// compile's configuration, as the header cache tells configurations apart.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/DeclBoundaryObserver.h"
#include "clang/Lex/DirectoryLookup.h"
#include "clang/Lex/ExpansionCandidateObserver.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SetVector.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <x86intrin.h>

using namespace clang;

// In clang/lib/ASTCache (PragmaStateCheck.h), which the clang binary exports.
namespace clang::ast_cache {
uint64_t warningFlagsHash(const DiagnosticOptions &Opts);
}

namespace {

// E_MAC (record mode only): an identifier reached the macro-expansion
// decision.
enum EvKind : uint8_t { E_TOK, E_DECL, E_DIR, E_MAC };
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
  bool Boundary; // record mode: the parser was between declarations
};

const uint64_t FNVBasis = 1469598103934665603ull, FNVPrime = 1099511628211ull;

// What the parser received: the token's kind and, for an identifier or a
// literal, its text. Equal for two compiles exactly when they hand the parser
// the same token, so a fragment's hash covers every macro's effect on it.
uint64_t tokenHash(const Token &T, Preprocessor &PP) {
  uint64_t H = FNVBasis;
  auto Mix = [&](const char *P, size_t N) {
    for (size_t I = 0; I < N; ++I)
      H = (H ^ (unsigned char)P[I]) * FNVPrime;
  };
  uint16_t K = T.getKind();
  Mix(reinterpret_cast<const char *>(&K), sizeof(K));
  if (T.isAnnotation())
    return H;
  if (IdentifierInfo *II = T.getIdentifierInfo()) {
    Mix(II->getNameStart(), II->getLength());
  } else if (T.isLiteral()) {
    if (const char *P = T.getLiteralData()) {
      Mix(P, T.getLength());
    } else {
      std::string S = PP.getSpelling(T);
      Mix(S.data(), S.size());
    }
  }
  return H;
}

uint64_t fnv(StringRef S, uint64_t H = FNVBasis) {
  for (char C : S)
    H = (H ^ (unsigned char)C) * FNVPrime;
  return H;
}
uint64_t mix(uint64_t H, uint64_t V) { return (H ^ V) * FNVPrime ^ (V >> 29); }

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
  uint64_t Config = 0;

  // Record mode (BOTHNER_TRACE_RECORD). Every token is an event, every
  // identifier that reaches the macro-expansion decision too (E_MAC), and
  // each event notes the parser's nesting. Aux, beside Events:
  //   E_TOK  {token hash, 0}
  //   E_MAC  {name id, macro state: 0 for no macro, else its definition's hash}
  //   E_DIR  {name id for #define and #undef, else 0; pragma state}
  // Its times mean nothing.
  bool Record = false;
  std::vector<std::pair<uint64_t, uint64_t>> Aux;
  CompilerInstance *CI = nullptr;
  Preprocessor *PP = nullptr;
  llvm::DenseMap<const IdentifierInfo *, unsigned> NameId;
  std::vector<StringRef> Names;
  llvm::DenseMap<const MacroInfo *, uint64_t> MacroHash;
  std::vector<DeclGroupRef> Groups; // top-level declarations, for the uses
  std::unique_ptr<ExpansionCandidateObserver> Candidates;

  State() { Events.reserve(1 << 16); }
  void add(EvKind K, SourceLocation L, Dir D = D_N) {
    bool B = true;
    if (Record) {
      if (DeclBoundaryObserver *O = PP->getDeclBoundaryObserver())
        B = O->isAtDeclarationBoundary();
      Aux.push_back({0, K == E_DIR ? pragmaState() : 0});
    }
    Events.push_back({__rdtsc(), L.getRawEncoding(), K, D, B});
    InRun = K == E_TOK;
  }
  void token(const Token &T) {
    ++Tokens;
    if (Record) {
      add(E_TOK, T.getLocation());
      Aux.back().first = tokenHash(T, *PP);
    } else if (!InRun) {
      add(E_TOK, T.getLocation());
    }
  }
  unsigned name(const IdentifierInfo *II) {
    auto [It, New] = NameId.insert({II, Names.size()});
    if (New)
      Names.push_back(II->getName());
    return It->second;
  }
  // The state a fragment's macro checks compare (the paper's "obvious
  // solution", p.30, with our expands-to): not a macro, or a macro with this
  // definition.
  uint64_t macroState(const IdentifierInfo *II) {
    const MacroInfo *MI = PP->getMacroDefinition(II).getMacroInfo();
    if (!MI)
      return 0;
    auto [It, New] = MacroHash.insert({MI, 0});
    if (New) {
      std::optional<std::string> Body = clang::ast_cache::serializeMacroTokens(
          *MI, PP->getSourceManager(), PP->getLangOpts());
      uint64_t H = fnv(Body ? StringRef(*Body) : StringRef("\x01unreadable"));
      H = mix(H, MI->isFunctionLike() ? 1 + MI->getNumParams() : 0);
      It->second = H | 1;
    }
    return It->second;
  }
  void macroCandidate(const Token &T) {
    const IdentifierInfo *II = T.getIdentifierInfo();
    if (!II)
      return;
    add(E_MAC, T.getLocation());
    Aux.back() = {name(II), macroState(II)};
  }
  // #pragma pack and the floating-point pragmas, which change what a
  // declaration means without changing its tokens.
  uint64_t pragmaState() {
    if (!CI->hasSema())
      return 0;
    Sema &S = CI->getSema();
    return mix(mix(FNVBasis, Sema::AlignPackInfo::getRawEncoding(S.AlignPackStack.CurrentValue)),
               S.CurFPFeatures.getAsOpaqueInt());
  }
};

class MacroCandidates : public ExpansionCandidateObserver {
  State &S;

public:
  MacroCandidates(State &S) : S(S) {}
  void expansionCandidate(const Token &T) override { S.macroCandidate(T); }
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
  void MacroDefined(const Token &T, const MacroDirective *) override {
    dir(T.getLocation(), D_define);
    if (S.Record)
      S.Aux.back().first = S.name(T.getIdentifierInfo());
  }
  void MacroUndefined(const Token &T, const MacroDefinition &, const MacroDirective *) override {
    dir(T.getLocation(), D_undef);
    if (S.Record)
      S.Aux.back().first = S.name(T.getIdentifierInfo());
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

// What record mode finds about a fragment instance (see HandleTranslationUnit).
struct Rec {
  uint64_t Hash = FNVBasis, Pragma = 0, Oracle = 0;
  int8_t Before = -1, After = -1;
  size_t Order = SIZE_MAX;
  std::map<unsigned, uint64_t> Ids; // name id -> macro state, first sighting
  llvm::SmallSetVector<int, 8> Uses, Deps;
  bool UsesMain = false, DepsMain = false, Param = false;
};

// Every name a declaration's parse resolved to a declaration: references,
// types written by name, and the earlier declaration a redeclaration merges
// with. Member and designator names are left out, as Bothner leaves them out.
class UseVisitor : public RecursiveASTVisitor<UseVisitor> {
public:
  std::function<void(SourceLocation, const Decl *)> Ref;
  bool VisitDeclRefExpr(DeclRefExpr *E) {
    Ref(E->getLocation(), E->getDecl());
    return true;
  }
  bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl());
    return true;
  }
  bool VisitRecordTypeLoc(RecordTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl());
    return true;
  }
  bool VisitEnumTypeLoc(EnumTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl());
    return true;
  }
  bool VisitDecl(Decl *D) {
    if (Decl *P = D->getPreviousDecl())
      Ref(D->getLocation(), P);
    return true;
  }
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
    if (S.Record)
      S.Groups.push_back(G);
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
    std::vector<FileID> EntryFID;
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
      EntryFID.push_back(FID);
    }

    auto fileOf = [&](SourceLocation::UIntTy Raw) {
      SourceLocation L = SourceLocation::getFromRawEncoding(Raw);
      if (L.isInvalid())
        return FileID();
      return SM.getFileID(SM.getExpansionLoc(L));
    };
    // The owner of a token or declaration at Raw. Count: a header location in
    // no fragment is a token that should have been in one.
    unsigned Loose = 0;
    auto ownerOf = [&](SourceLocation::UIntTy Raw, int Default, bool Count = true) -> int {
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
        Loose += Count;
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
      if (E.Kind == E_MAC)
        continue;
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
      case E_MAC: // skipped above
        break;
      }
    }
    add(Parser ? O_END : LastDir, S.T1 - Prev);

    // Record mode, per fragment instance:
    //   Hash    the tokens the parser received;
    //   Before, After  whether the parser was between declarations at the
    //           directive event before its first token and the one after its
    //           last (Bothner's currently_nested, p.33);
    //   Order   the event number of its first token: the order fragments
    //           were processed in;
    //   Pragma  #pragma pack and floating-point state at its start;
    //   Ids     each identifier that reached the macro-expansion decision in
    //           it, with its macro state (the macro checks, p.30);
    //   Uses    the fragments whose declarations its names resolved to, as
    //           GCC resolved them: to an entity's first declaration, which
    //           its later declarations were merged into (p.27-28, p.31);
    //   Deps    the fragments holding any earlier declaration of those
    //           entities: what the oracle hashes;
    //   flags   it used a main-file declaration (never reusable, p.27); the
    //           oracle depends on one; it declares a tag in a parameter list
    //           (p.28-29);
    //   Oracle  everything its parse depends on, hashed: tokens, pragma
    //           state, and the oracles of its Deps. Two instances with the
    //           same oracle parse to the same declarations.
    std::vector<Rec> R(Frags.size());
    std::vector<std::array<uint64_t, 3>> Defs; // event number, name id, defined
    if (S.Record) {
      bool Last = true;
      uint64_t LastPragma = 0;
      int Cur = -1;
      for (size_t I = 0; I < S.Events.size(); ++I) {
        const Ev &E = S.Events[I];
        if (E.Kind == E_DIR) {
          Last = E.Boundary;
          LastPragma = S.Aux[I].second;
          if (Cur >= 0 && R[Cur].After < 0)
            R[Cur].After = E.Boundary;
          Cur = -1;
          if (E.D == D_define || E.D == D_undef)
            Defs.push_back({I, S.Aux[I].first, E.D == D_define});
        } else if (E.Kind == E_TOK) {
          int O = ownerOf(E.Loc, O_LOOSE);
          if (O < 0)
            continue;
          Rec &F = R[O];
          if (F.Order == SIZE_MAX) {
            F.Order = I;
            F.Before = Last;
            F.Pragma = LastPragma;
          }
          F.Hash = (F.Hash ^ S.Aux[I].first) * FNVPrime;
          Cur = O;
        } else if (E.Kind == E_MAC) {
          int O = ownerOf(E.Loc, O_LOOSE, /*Count=*/false);
          if (O >= 0)
            R[O].Ids.insert({unsigned(S.Aux[I].first), S.Aux[I].second});
        }
      }

      // Uses and Deps, from the declarations each fragment made.
      auto declOwner = [&](const Decl *D) {
        return ownerOf(SM.getExpansionLoc(D->getLocation()).getRawEncoding(), O_PRE,
                       /*Count=*/false);
      };
      auto startOf = [&](int F) {
        return SM.getComposedLoc(EntryFID[Frags[F].Entry], Frags[F].Start);
      };
      UseVisitor V;
      V.Ref = [&](SourceLocation At, const Decl *D) {
        int U = ownerOf(SM.getExpansionLoc(At).getRawEncoding(), O_PRE, false);
        if (U < 0 || !D)
          return;
        const Decl *First = D->getCanonicalDecl();
        if (First->isImplicit())
          return; // a builtin: there from the start, as GCC's were (p.23)
        int O = declOwner(First);
        if (O >= 0 && O != U && R[O].Order < R[U].Order)
          R[U].Uses.insert(O);
        else if (O == O_MAIN && SM.isBeforeInTranslationUnit(First->getLocation(), startOf(U)))
          R[U].UsesMain = true;
        for (const Decl *Q : D->redecls()) {
          if (Q->isImplicit())
            continue;
          int OQ = declOwner(Q);
          if (OQ >= 0 && OQ != U && R[OQ].Order < R[U].Order)
            R[U].Deps.insert(OQ);
          else if (OQ == O_MAIN && SM.isBeforeInTranslationUnit(Q->getLocation(), startOf(U)))
            R[U].DepsMain = true;
        }
        // p.28-29: a tag first declared in a parameter list, which C moves
        // into the function (Sema::ActOnFunctionDeclarator).
        if (auto *TD = dyn_cast<TagDecl>(D))
          if (auto *FD = dyn_cast<FunctionDecl>(TD->getDeclContext()))
            if (!FD->doesThisDeclarationHaveABody() ||
                SM.isBeforeInTranslationUnit(TD->getLocation(), FD->getBody()->getBeginLoc()))
              R[U].Param = true;
      };
      for (DeclGroupRef G : S.Groups)
        for (Decl *D : G)
          V.TraverseDecl(D);

      // Oracles, in processing order: each one's Deps come before it.
      std::vector<unsigned> ByOrder(Frags.size());
      std::iota(ByOrder.begin(), ByOrder.end(), 0);
      std::sort(ByOrder.begin(), ByOrder.end(),
                [&](unsigned A, unsigned B) { return R[A].Order < R[B].Order; });
      uint64_t MainHash = fnv(SM.getBufferData(Main));
      for (unsigned I : ByOrder) {
        Rec &F = R[I];
        uint64_t H = mix(mix(FNVBasis, F.Hash), F.Pragma);
        std::vector<uint64_t> D;
        for (int Q : F.Deps)
          D.push_back(R[Q].Oracle);
        std::sort(D.begin(), D.end());
        for (uint64_t X : D)
          H = mix(H, X);
        if (F.DepsMain)
          H = mix(H, MainHash);
        F.Oracle = H | 1;
      }
    }

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
    OS << "{\"config\":\"" << llvm::format_hex_no_prefix(S.Config, 16)
       << "\",\"record\":" << S.Record << ",\"ghz\":" << 1.0 / Ns
       << ",\"span_ns\":" << ns(S.T1 - S.T0)
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
    // [entry, start, end, tokens, option-3 start, ns, edge ns]; in record
    // mode also [token hash, nesting before, nesting after (1: between
    // declarations, 0: inside one, -1: no event), order, pragma state, oracle,
    // flags (1: uses the main file, 2: its oracle does, 4: a tag in a
    // parameter list), uses, [[name id, macro state (low 32 bits; 0: not a
    // macro)], ...], deps]. The simulator recomputes the oracle from the
    // token hash, the pragma state and the deps, leaving out deps at what its
    // rule counts as the fragment's own location.
    auto hex = [](uint64_t V) { return llvm::format_hex_no_prefix(V, 16); };
    OS << "],\"frags\":[";
    for (size_t I = 0; I < Frags.size(); ++I) {
      const Frag &F = Frags[I];
      OS << (I ? "," : "") << "[" << F.Entry << "," << F.Start << "," << F.End
         << "," << F.NTok << "," << F.Start3 << "," << ns(F.Ticks) << ","
         << ns(F.Edge);
      if (S.Record) {
        const Rec &X = R[I];
        OS << ",\"" << hex(X.Hash) << "\"," << int(X.Before) << "," << int(X.After)
           << "," << X.Order << ",\"" << hex(X.Pragma) << "\",\"" << hex(X.Oracle)
           << "\"," << (X.UsesMain | X.DepsMain << 1 | X.Param << 2) << ",[";
        for (size_t K = 0; K < X.Uses.size(); ++K)
          OS << (K ? "," : "") << X.Uses[K];
        OS << "],[";
        bool First = true;
        for (auto [Id, St] : X.Ids) {
          OS << (First ? "" : ",") << "[" << Id << "," << uint32_t(St) << "]";
          First = false;
        }
        OS << "],[";
        for (size_t K = 0; K < X.Deps.size(); ++K)
          OS << (K ? "," : "") << X.Deps[K];
        OS << "]";
      }
      OS << "]";
    }
    if (S.Record) {
      OS << "],\"names\":[";
      for (size_t I = 0; I < S.Names.size(); ++I)
        OS << (I ? "," : "") << jstr(S.Names[I]);
      // Every #define and #undef: [event number, name id, 1 if #define].
      OS << "],\"defs\":[";
      for (size_t I = 0; I < Defs.size(); ++I)
        OS << (I ? "," : "") << "[" << Defs[I][0] << "," << Defs[I][1] << ","
           << Defs[I][2] << "]";
    }
    OS << "]}\n";
  }
};

// The compile's configuration, as the header cache tells configurations apart
// (the user's "flag checking device", instead of assuming one flag set per
// server): Clang's module hash (language, target, -D/-U, code generation,
// header-search options), the compile-wide part of the cache's searchContext
// (HeaderCacheServer.cpp: working directory, sysroot, overlays, every search
// directory and its kind), and the cache's warningFlagsHash.
uint64_t configuration(CompilerInstance &CI) {
  std::string K = CI.getInvocation().getModuleHash();
  K += '\0';
  auto Num = [&](uint64_t V) {
    K.append(reinterpret_cast<const char *>(&V), sizeof(V));
  };
  auto Str = [&](StringRef V) {
    K += V;
    K += '\0';
  };
  Str(CI.getFileManager().getFileSystemOpts().WorkingDir);
  if (llvm::ErrorOr<std::string> CWD =
          CI.getFileManager().getVirtualFileSystem().getCurrentWorkingDirectory())
    Str(*CWD);
  HeaderSearch &HS = CI.getPreprocessor().getHeaderSearchInfo();
  const HeaderSearchOptions &O = HS.getHeaderSearchOpts();
  Str(O.Sysroot);
  for (const std::string &F : O.VFSOverlayFiles)
    Str(F);
  Num(CI.getLangOpts().MSVCCompat);
  unsigned Index = 0, Angled = 0, System = 0;
  for (const DirectoryLookup &D : HS.search_dir_range()) {
    if (HS.angled_dir_begin() != HS.search_dir_end() &&
        &D == &*HS.angled_dir_begin())
      Angled = Index;
    if (HS.system_dir_begin() != HS.search_dir_end() &&
        &D == &*HS.system_dir_begin())
      System = Index;
    Num(D.getLookupType());
    Num(D.getDirCharacteristic());
    Str(D.getName());
    ++Index;
  }
  Num(Angled);
  Num(System);
  Num(Index);
  Num(clang::ast_cache::warningFlagsHash(CI.getDiagnosticOpts()));
  return llvm::xxh3_64bits(K);
}

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
    S->PP = &PP;
    S->Record = getenv("BOTHNER_TRACE_RECORD") != nullptr;
    S->Config = configuration(CI);
    S->CI = &CI;
    if (S->Record) {
      // The slot the header cache uses; empty in a compile without it.
      S->Candidates = std::make_unique<MacroCandidates>(*S);
      PP.setExpansionCandidateObserver(*S->Candidates);
    }
    if (!Off.contains("tokens"))
      PP.setTokenWatcher([Raw](const Token &T) { Raw->token(T); });
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
