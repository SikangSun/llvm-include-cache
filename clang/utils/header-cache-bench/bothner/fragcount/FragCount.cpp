// FragCount: counts, per compile, the fragments Bothner's 2003 compile server
// would handle.  A header is every file entered other than the main file and
// the predefines buffer; a header entered twice counts twice.
//
// Processed directive: a directive line ('#' first token on a line, as the
// raw lexer sees it) outside every skipped conditional block, or the
// directive that starts a skipped block ('#' at the range's begin), or a
// conditional directive inside a skipped range that the preprocessor reported
// through a conditional callback (the one that ends the skip, and #elif's it
// evaluated on the way).
//
// Option-2 fragment: maximal run of non-directive, non-skipped tokens between
// two processed directives.  Option 3: #define/#undef do not end a run.
//
// Output: JSON to $FRAGCOUNT_OUT.
#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

using namespace clang;

namespace {

enum DirKind {
  DK_include, DK_include_next, DK_import, DK_define, DK_undef, DK_if,
  DK_ifdef, DK_ifndef, DK_elif, DK_elifdef, DK_elifndef, DK_else, DK_endif,
  DK_pragma, DK_line, DK_linemarker, DK_error, DK_warning, DK_ident,
  DK_sccs, DK_null, DK_embed, DK_assert, DK_unassert, DK_other, DK_N
};
const char *DirName[DK_N] = {
  "include", "include_next", "import", "define", "undef", "if", "ifdef",
  "ifndef", "elif", "elifdef", "elifndef", "else", "endif", "pragma", "line",
  "linemarker", "error", "warning", "ident", "sccs", "null", "embed",
  "assert", "unassert", "other"};

DirKind classify(StringRef S) {
  for (int K = 0; K < DK_N; ++K)
    if (S == DirName[K] && K != DK_linemarker && K != DK_null && K != DK_other)
      return DirKind(K);
  return DK_other;
}

// One item of a raw-lexed buffer: a plain token (Kind < 0) or a whole
// directive line.
struct Item {
  unsigned Off;     // token offset, or the '#' offset of a directive
  unsigned NameOff; // directive-name token offset (== Off for null directive)
  int Kind;         // -1: plain token
  unsigned NToks;   // tokens in the directive line (incl. '#'), 1 for plain
  int TK;           // token kind of a plain token
};

struct Counts {
  uint64_t Entries = 0, Frag2 = 0, Frag3 = 0, FragTok = 0, DirTok = 0,
           SkipTok = 0, Dirs = 0, SkippedDirs = 0;
  // #else/#elif* at the top level of a skipped range that the preprocessor
  // passed without a callback (between two skipped groups); a subset of
  // SkippedDirs.  They separate no tokens, so fragments do not depend on them.
  uint64_t Boundary = 0;
  // Fragments that end inside a declaration (raw-token heuristic): bracket
  // depth > 0 at the end, or (depth 0) last token is neither ';' nor '}'.
  uint64_t OpenDepth2 = 0, OpenTail2 = 0, OpenDepth3 = 0, OpenTail3 = 0;
  std::set<unsigned> Starts2, Starts3; // fragment start offsets (headers only)
  uint64_t ByKind[DK_N] = {};
  void add(const Counts &O) {
    Entries += O.Entries; Frag2 += O.Frag2; Frag3 += O.Frag3;
    FragTok += O.FragTok; DirTok += O.DirTok; SkipTok += O.SkipTok;
    Dirs += O.Dirs; SkippedDirs += O.SkippedDirs; Boundary += O.Boundary;
    OpenDepth2 += O.OpenDepth2; OpenTail2 += O.OpenTail2;
    OpenDepth3 += O.OpenDepth3; OpenTail3 += O.OpenTail3;
    Starts2.insert(O.Starts2.begin(), O.Starts2.end());
    Starts3.insert(O.Starts3.begin(), O.Starts3.end());
    for (int K = 0; K < DK_N; ++K) ByKind[K] += O.ByKind[K];
  }
};

struct EntryRec {
  FileID FID;
  SrcMgr::CharacteristicKind Kind;
};

struct State {
  std::vector<EntryRec> Entries;
  llvm::DenseMap<FileID, std::vector<std::pair<unsigned, unsigned>>> Skips;
  llvm::DenseSet<SourceLocation::UIntTy> Cond;
  uint64_t BadSkips = 0;
  // Cross-check: directives as the preprocessor's own callbacks report them.
  std::vector<std::pair<FileID, int>> CBDirs;
};

class CB : public PPCallbacks {
  std::shared_ptr<State> SP; // the plugin action dies before the compile ends
  State &S;
  SourceManager &SM;
  void cond(SourceLocation L) { S.Cond.insert(L.getRawEncoding()); }
  void seen(SourceLocation L, int K) {
    S.CBDirs.push_back({SM.getFileID(SM.getExpansionLoc(L)), K});
  }

public:
  CB(std::shared_ptr<State> SP, SourceManager &SM) : SP(SP), S(*SP), SM(SM) {}
  void LexedFileChanged(FileID FID, LexedFileChangeReason Reason,
                        SrcMgr::CharacteristicKind FileType, FileID,
                        SourceLocation) override {
    if (Reason == LexedFileChangeReason::EnterFile)
      S.Entries.push_back({FID, FileType});
  }
  void SourceRangeSkipped(SourceRange R, SourceLocation EndifLoc) override {
    auto B = SM.getDecomposedLoc(R.getBegin());
    auto E = SM.getDecomposedLoc(R.getEnd());
    if (B.first != E.first) {
      ++S.BadSkips;
      E.second = ~0u;
    }
    S.Skips[B.first].push_back({B.second, E.second});
    cond(EndifLoc);
  }
  void If(SourceLocation L, SourceRange, ConditionValueKind) override { cond(L); seen(L, DK_if); }
  void Elif(SourceLocation L, SourceRange, ConditionValueKind,
            SourceLocation) override { cond(L); seen(L, DK_elif); }
  void Ifdef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L); seen(L, DK_ifdef); }
  void Ifndef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L); seen(L, DK_ifndef); }
  void MacroDefined(const Token &T, const MacroDirective *) override { seen(T.getLocation(), DK_define); }
  void MacroUndefined(const Token &T, const MacroDefinition &, const MacroDirective *) override { seen(T.getLocation(), DK_undef); }
  void InclusionDirective(SourceLocation HashLoc, const Token &, StringRef, bool,
                          CharSourceRange, OptionalFileEntryRef, StringRef,
                          StringRef, const Module *, bool,
                          SrcMgr::CharacteristicKind) override { seen(HashLoc, DK_include); }
  void PragmaDirective(SourceLocation L, PragmaIntroducerKind K) override {
    if (K == PIK_HashPragma) seen(L, DK_pragma);
  }
  void Elifdef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L); }
  void Elifdef(SourceLocation L, SourceRange, SourceLocation) override { cond(L); }
  void Elifndef(SourceLocation L, const Token &, const MacroDefinition &) override { cond(L); }
  void Elifndef(SourceLocation L, SourceRange, SourceLocation) override { cond(L); }
  void Else(SourceLocation L, SourceLocation) override { cond(L); seen(L, DK_else); }
  void Endif(SourceLocation L, SourceLocation) override { cond(L); seen(L, DK_endif); }
};

std::string jstr(StringRef S) {
  std::string O = "\"";
  for (char C : S) {
    if (C == '"' || C == '\\') { O += '\\'; O += C; }
    else if ((unsigned char)C < 0x20) { char B[8]; snprintf(B, 8, "\\u%04x", C); O += B; }
    else O += C;
  }
  return O + "\"";
}

void emit(llvm::raw_ostream &OS, const Counts &C) {
  OS << "{\"entries\":" << C.Entries << ",\"frag2\":" << C.Frag2
     << ",\"frag3\":" << C.Frag3 << ",\"frag_tok\":" << C.FragTok
     << ",\"dir_tok\":" << C.DirTok << ",\"skip_tok\":" << C.SkipTok
     << ",\"dirs\":" << C.Dirs << ",\"skipped_dirs\":" << C.SkippedDirs
     << ",\"boundary\":" << C.Boundary
     << ",\"open_depth2\":" << C.OpenDepth2 << ",\"open_tail2\":" << C.OpenTail2
     << ",\"open_depth3\":" << C.OpenDepth3 << ",\"open_tail3\":" << C.OpenTail3
     << ",\"by_kind\":{";
  bool First = true;
  for (int K = 0; K < DK_N; ++K) {
    if (!C.ByKind[K]) continue;
    if (!First) OS << ",";
    First = false;
    OS << "\"" << DirName[K] << "\":" << C.ByKind[K];
  }
  OS << "}}";
}

class Consumer : public ASTConsumer {
  CompilerInstance &CI;
  std::shared_ptr<State> SP;
  State &S;
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
        Item D{off(T), off(T), DK_null, 1, 0};
        L.LexFromRawLexer(T);
        if (T.is(tok::eof) || T.isAtStartOfLine()) {
          V.push_back(D);
          continue;
        }
        D.NameOff = off(T);
        if (T.is(tok::raw_identifier))
          D.Kind = classify(T.getRawIdentifier());
        else if (T.is(tok::numeric_constant))
          D.Kind = DK_linemarker;
        else
          D.Kind = DK_other;
        ++D.NToks;
        while (true) {
          L.LexFromRawLexer(T);
          if (T.is(tok::eof) || T.isAtStartOfLine())
            break;
          ++D.NToks;
        }
        V.push_back(D);
        continue;
      }
      V.push_back(Item{off(T), off(T), -1, 1, int(T.getKind())});
      L.LexFromRawLexer(T);
    }
    return V;
  }

  Counts count(FileID FID) {
    SourceManager &SM = CI.getSourceManager();
    const std::vector<Item> &V = lex(FID);
    std::vector<std::pair<unsigned, unsigned>> Sk;
    auto It = S.Skips.find(FID);
    if (It != S.Skips.end())
      Sk = It->second;
    std::sort(Sk.begin(), Sk.end());
    SourceLocation Start = SM.getLocForStartOfFile(FID);
    Counts C;
    C.Entries = 1;
    size_t SI = 0, DepthSI = ~size_t(0);
    int SkipDepth = 0; // conditional nesting inside the current skipped range
    bool Run2 = false, Run3 = false;
    int Depth = 0, Last = tok::semi;
    auto close = [&](uint64_t &OD, uint64_t &OT) {
      if (Depth > 0) ++OD;
      else if (Last != tok::semi && Last != tok::r_brace) ++OT;
    };
    for (const Item &I : V) {
      while (SI < Sk.size() && Sk[SI].second <= I.Off)
        ++SI;
      bool InSkip = SI < Sk.size() && Sk[SI].first < I.Off && I.Off < Sk[SI].second;
      if (I.Kind < 0) {
        if (InSkip) {
          ++C.SkipTok;
          continue;
        }
        ++C.FragTok;
        if (I.TK == tok::l_paren || I.TK == tok::l_square || I.TK == tok::l_brace) ++Depth;
        if (I.TK == tok::r_paren || I.TK == tok::r_square || I.TK == tok::r_brace) --Depth;
        Last = I.TK;
        if (!Run2) { ++C.Frag2; Run2 = true; C.Starts2.insert(I.Off); }
        if (!Run3) { ++C.Frag3; Run3 = true; C.Starts3.insert(I.Off); }
        continue;
      }
      bool Processed =
          !InSkip ||
          S.Cond.count(Start.getLocWithOffset(I.NameOff).getRawEncoding());
      if (InSkip && DepthSI != SI) {
        DepthSI = SI;
        SkipDepth = 0;
      }
      if (!Processed) {
        C.SkipTok += I.NToks;
        ++C.SkippedDirs;
        if (I.Kind == DK_if || I.Kind == DK_ifdef || I.Kind == DK_ifndef)
          ++SkipDepth;
        else if (I.Kind == DK_endif && SkipDepth > 0)
          --SkipDepth;
        else if (SkipDepth == 0 &&
                 (I.Kind == DK_else || I.Kind == DK_elif ||
                  I.Kind == DK_elifdef || I.Kind == DK_elifndef))
          ++C.Boundary;
        continue;
      }
      ++C.Dirs;
      ++C.ByKind[I.Kind];
      C.DirTok += I.NToks;
      if (Run2) close(C.OpenDepth2, C.OpenTail2);
      Run2 = false;
      if (I.Kind != DK_define && I.Kind != DK_undef) {
        if (Run3) close(C.OpenDepth3, C.OpenTail3);
        Run3 = false;
      }
    }
    if (Run2) close(C.OpenDepth2, C.OpenTail2);
    if (Run3) close(C.OpenDepth3, C.OpenTail3);
    return C;
  }

public:
  Consumer(CompilerInstance &CI, std::shared_ptr<State> SP) : CI(CI), SP(SP), S(*SP) {}

  void HandleTranslationUnit(ASTContext &) override {
    const char *Out = getenv("FRAGCOUNT_OUT");
    if (!Out)
      return;
    SourceManager &SM = CI.getSourceManager();
    Counts Main, Sys, Proj;
    struct HRec { std::string Path; bool Sys; Counts C; };
    std::vector<HRec> Hdrs;
    llvm::DenseMap<const FileEntry *, unsigned> Index;
    bool Dbg = getenv("FRAGCOUNT_DEBUG");
    for (const EntryRec &E : S.Entries) {
      if (Dbg) llvm::errs() << "entry " << E.FID.getHashValue() << " of " << SM.local_sloc_entry_size() << "\n";
      if (E.FID == SM.getMainFileID()) {
        Main.add(count(E.FID));
        continue;
      }
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(E.FID);
      if (!FE)
        continue; // predefines / builtin buffers
      Counts C = count(E.FID);
      bool IsSys = SrcMgr::isSystem(E.Kind);
      Counts CNoStarts = C;
      CNoStarts.Starts2.clear();
      CNoStarts.Starts3.clear();
      (IsSys ? Sys : Proj).add(CNoStarts);
      auto Ins = Index.insert({&FE->getFileEntry(), Hdrs.size()});
      if (Ins.second) {
        StringRef RP = FE->getFileEntry().tryGetRealPathName();
        Hdrs.push_back({(RP.empty() ? FE->getName() : RP).str(), IsSys, {}});
      }
      Hdrs[Ins.first->second].C.add(C);
    }
    std::error_code EC;
    llvm::raw_fd_ostream OS(Out, EC);
    if (EC) {
      llvm::errs() << "fragcount: cannot write " << Out << "\n";
      return;
    }
    uint64_t CBK[DK_N] = {};
    llvm::DenseSet<FileID> HdrFIDs;
    for (const EntryRec &E : S.Entries)
      if (E.FID != SM.getMainFileID() && SM.getFileEntryRefForID(E.FID))
        HdrFIDs.insert(E.FID);
    for (auto &P : S.CBDirs)
      if (HdrFIDs.count(P.first))
        ++CBK[P.second];
    OS << "{\"cb_by_kind\":{";
    for (int K = 0; K < DK_N; ++K)
      OS << (K ? "," : "") << "\"" << DirName[K] << "\":" << CBK[K];
    OS << "},\"bad_skips\":" << S.BadSkips << ",\"distinct\":" << Hdrs.size()
       << ",\"main\":";
    emit(OS, Main);
    OS << ",\"sys\":";
    emit(OS, Sys);
    OS << ",\"proj\":";
    emit(OS, Proj);
    OS << ",\"headers\":[";
    for (size_t I = 0; I < Hdrs.size(); ++I) {
      if (I) OS << ",";
      OS << "{\"path\":" << jstr(Hdrs[I].Path) << ",\"sys\":" << Hdrs[I].Sys
         << ",\"c\":";
      emit(OS, Hdrs[I].C);
      for (int K = 2; K <= 3; ++K) {
        OS << ",\"s" << K << "\":[";
        bool F = true;
        for (unsigned O : K == 2 ? Hdrs[I].C.Starts2 : Hdrs[I].C.Starts3) {
          if (!F) OS << ",";
          F = false;
          OS << O;
        }
        OS << "]";
      }
      OS << "}";
    }
    OS << "]}\n";
  }
};

class Action : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    auto S = std::make_shared<State>();
    Preprocessor &PP = CI.getPreprocessor();
    PP.addPPCallbacks(std::make_unique<CB>(S, CI.getSourceManager()));
    return std::make_unique<Consumer>(CI, S);
  }
  bool ParseArgs(const CompilerInstance &, const std::vector<std::string> &) override {
    return true;
  }
  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<Action> X("fragcount", "count Bothner fragments");
