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
//
// Record mode also records what the compile server's checks need
// (COMPILE_SERVER_DESIGN.md 2.6 and 3): nesting under the fixed observer
// (2.9), the full pragma state, what each fragment declares and references at
// file scope, never-reusable builtins, pending parser pragmas, directives
// inside macro arguments, and what separates consecutive fragments of a file
// (for fragment groups, 2.7).
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
#include "clang/Sema/Scope.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SetVector.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
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
#include <set>
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

// Pragma state that refuses a reuse whatever the stored state: a #pragma weak
// or redefine_extname waiting for a declaration, and an active
// #pragma clang attribute.
enum : uint32_t { PF_WEAK = 1, PF_ATTRIBUTE = 2 };
// In a directive event's Info: the parser was inside a function prototype's
// scope (inPrototype).
const uint32_t INFO_PROTO = 1u << 31;
// The server's view at a directive event (see HandleTranslationUnit).
enum : uint8_t { VIEW_STRICT = 1, VIEW_PENDING = 2, VIEW_ARGS = 4 };
// Per fragment instance: why the server never reuses it here (SV_BUILTIN),
// or declines at its start.
enum : uint32_t {
  SV_BUILTIN = 1,   // a never-reusable builtin or _Pragma (2.4)
  SV_ARGS = 2,      // it starts or ends inside a function-like macro's arguments
  SV_PENDING = 4,   // it starts while a parser pragma is pending
  SV_WEAK = 8,      // PF_WEAK at its start
  SV_ATTRIBUTE = 16, // PF_ATTRIBUTE at its start
  SV_UNDEFINED = 32, // it declares an internal or inline function that it
                     // does not define, never reused (2.8: Used bits leak)
  SV_UNDEFINED_END = 64 // and that function is still undefined at the end of
                        // the compile
};
// What separates a fragment from the previous one of the same header entry:
// the processed directives between them, by kind, or SEP_FIRST.
enum : uint8_t {
  SEP_COND = 1, SEP_DEFINE = 2, SEP_INCLUDE = 4, SEP_PRAGMA = 8, SEP_OTHER = 16, SEP_FIRST = 32
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
  uint64_t Config = 0;
  uint64_t FullConfig = 0; // the server's configuration key (2.1)

  // Record mode (BOTHNER_TRACE_RECORD). Every token is an event, every
  // identifier that reaches the macro-expansion decision too (E_MAC), and
  // each event notes the parser's nesting. Aux, beside Events:
  //   E_TOK  {token hash, for an identifier its name id + 1, else 0}
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
  // Also beside Events, for the server's checks:
  //   Prag  the full pragma state at each directive and token (else 0);
  //   Info  E_TOK: the token's kind; E_DIR: the kind of Sema's current
  //         context, and INFO_PROTO; and, where Prag is set, PF_* flags
  //         << 16.
  std::vector<uint64_t> Prag;
  std::vector<uint32_t> Info;
  // Never-reusable builtins by location: 1 always (__COUNTER__, __DATE__,
  // __TIME__, __TIMESTAMP__, __BASE_FILE__, __INCLUDE_LEVEL__, _Pragma); 2
  // after a #line in the same file (__FILE__, __FILE_NAME__, __LINE__).
  std::vector<std::pair<SourceLocation, uint8_t>> Builtins;
  llvm::DenseMap<FileID, unsigned> LineDirective; // offset of a file's first #line
  // Function-like macro invocations written in a file: [name, ')'].
  std::vector<std::pair<SourceLocation::UIntTy, SourceLocation::UIntTy>> Invocations;

  State() { Events.reserve(1 << 16); }
  void add(EvKind K, SourceLocation L, Dir D = D_N) {
    bool B = true;
    if (Record) {
      if (DeclBoundaryObserver *O = PP->getDeclBoundaryObserver())
        B = O->isAtDeclarationBoundary();
      Aux.push_back({0, K == E_DIR ? pragmaState() : 0});
      Prag.push_back(K == E_DIR ? fullPragmaState() : 0);
      Info.push_back(K == E_DIR ? contextKind() | pragmaFlags() << 16 |
                                      (inPrototype() ? INFO_PROTO : 0)
                                : 0);
    }
    Events.push_back({__rdtsc(), L.getRawEncoding(), K, D, B});
    InRun = K == E_TOK;
  }
  void token(const Token &T) {
    ++Tokens;
    if (Record) {
      add(E_TOK, T.getLocation());
      Aux.back().first = tokenHash(T, *PP);
      if (T.is(tok::identifier))
        Aux.back().second = name(T.getIdentifierInfo()) + 1;
      // At every token: a fragment's first token may follow a pragma line's
      // annotation token, which no fragment owns.
      Info.back() = T.getKind() | pragmaFlags() << 16;
      Prag.back() = fullPragmaState();
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
  // Every pragma state the server compares (2.6): pack and align, floating
  // point, ms_struct, visibility, #pragma clang section and optimize, the
  // Microsoft segment pragmas, #pragma clang attribute's depth, and
  // assume_nonnull.
  uint64_t fullPragmaState() {
    if (!CI->hasSema())
      return 0;
    Sema &S = CI->getSema();
    uint64_t H = mix(pragmaState(), S.FpPragmaStack.CurrentValue.getAsOpaqueInt());
    H = mix(H, S.MSStructPragmaOn);
    // Sema's VisContext is a VisStack (SemaAttr.cpp).
    if (auto *V = static_cast<std::vector<std::pair<unsigned, SourceLocation>> *>(S.VisContext)) {
      H = mix(H, V->size() + 1);
      if (!V->empty())
        H = mix(H, V->back().first);
    }
    for (const Sema::PragmaClangSection *P :
         {&S.PragmaClangBSSSection, &S.PragmaClangDataSection, &S.PragmaClangRodataSection,
          &S.PragmaClangRelroSection, &S.PragmaClangTextSection})
      H = mix(H, P->Valid ? fnv(P->SectionName) : 0);
    for (const StringLiteral *L : {S.DataSegStack.CurrentValue, S.BSSSegStack.CurrentValue,
                                   S.ConstSegStack.CurrentValue, S.CodeSegStack.CurrentValue})
      H = mix(H, L ? fnv(L->getBytes()) : 0);
    H = mix(H, S.StrictGuardStackCheckStack.CurrentValue);
    H = mix(H, S.OptimizeOffPragmaLocation.isValid());
    H = mix(H, S.PragmaAttributeStack.size());
    H = mix(H, PP->getPragmaAssumeNonNullLoc().isValid());
    return H | 1;
  }
  uint32_t pragmaFlags() {
    if (!CI->hasSema())
      return 0;
    Sema &S = CI->getSema();
    return (S.WeakUndeclaredIdentifiers.empty() && S.ExtnameUndeclaredIdentifiers.empty() ? 0 : PF_WEAK) |
           (S.PragmaAttributeStack.empty() ? 0 : PF_ATTRIBUTE);
  }
  uint32_t contextKind() {
    if (!CI->hasSema() || !CI->getSema().CurContext)
      return 0xFFFF;
    return CI->getSema().CurContext->getDeclKind();
  }
  // Inside a function prototype's scope: in a declarator's parameter list,
  // or between a K&R definition's parameter declarations, where a ';' at
  // file scope does not end the declaration.
  bool inPrototype() {
    if (!CI->hasSema())
      return false;
    for (Scope *Sc = CI->getSema().getCurScope(); Sc; Sc = Sc->getParent())
      if (Sc->isFunctionPrototypeScope())
        return true;
    return false;
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
    else if (S.Record)
      S.Builtins.push_back({L, 1}); // _Pragma or __pragma
  }
  void Ident(SourceLocation L, StringRef) override { dir(L, D_ident); }
  void FileChanged(SourceLocation L, FileChangeReason Reason, SrcMgr::CharacteristicKind,
                   FileID) override {
    if (S.Record && Reason == RenameFile) { // #line or a line marker
      auto [FID, Off] = SM.getDecomposedExpansionLoc(L);
      if (SM.getFileEntryRefForID(FID)) // not the predefines' markers
        S.LineDirective.try_emplace(FID, Off);
    }
  }
  void MacroExpands(const Token &Name, const MacroDefinition &MD, SourceRange Range,
                    const MacroArgs *) override {
    const MacroInfo *MI = MD.getMacroInfo();
    if (!S.Record || !MI)
      return;
    if (MI->isBuiltinMacro()) {
      StringRef N = Name.getIdentifierInfo()->getName();
      if (N == "__COUNTER__" || N == "__DATE__" || N == "__TIME__" || N == "__TIMESTAMP__" ||
          N == "__BASE_FILE__" || N == "__INCLUDE_LEVEL__")
        S.Builtins.push_back({Name.getLocation(), 1});
      else if (N == "__FILE__" || N == "__FILE_NAME__" || N == "__LINE__")
        S.Builtins.push_back({Name.getLocation(), 2});
    } else if (MI->isFunctionLike() && Range.getEnd().isFileID()) {
      // From where the invocation is written: its name may come from
      // another macro's expansion (#define CALL FOO, then CALL(...)).
      S.Invocations.push_back({SM.getExpansionLoc(Range.getBegin()).getRawEncoding(),
                               Range.getEnd().getRawEncoding()});
    }
  }
};

// One option-2 fragment instance. Key: the offset just after the directive
// line before it (0 at the start of the file), where the server's hook sees
// it start (2.4); Sep: what separates it from the previous one (SEP_*).
struct Frag {
  unsigned Entry, Start, End, NTok, Start3, Key;
  uint8_t Sep;
  uint64_t Ticks = 0, Edge = 0;
};

// One option-3 fragment instance (p.26: #define and #undef inside fragments;
// every other directive still ends one): the option-2 fragments it holds,
// and its #define and #undef lines, whose processing a reuse also skips.
struct Span {
  unsigned Entry, Start, End; // End: the ending directive's '#', or the file's end
  unsigned Key;               // as Frag's
  uint8_t Sep;                // as Frag's, between spans
  std::vector<unsigned> Members;
  unsigned Defines = 0;
  uint64_t DefTicks = 0; // time charged to its #define and #undef lines
  // Record mode: its first #define's event number, the nesting at its first
  // and last #define, the pragma state at its first, and each name defined
  // or undefined, with the event number of that #define or #undef. Also the
  // nesting under the fixed observer at its first and last #define, and the
  // full pragma state and SV_* flags at its first.
  size_t DefOrder = SIZE_MAX;
  int8_t DefBefore = -1, DefAfter = -1, DefSBefore = -1, DefSAfter = -1;
  uint64_t DefPragma = 0, DefPrag = 0;
  uint32_t DefServer = 0;
  std::vector<std::pair<size_t, unsigned>> Names;
};

// What record mode finds about a fragment instance (see HandleTranslationUnit).
struct Rec {
  uint64_t Hash = FNVBasis, Pragma = 0, Oracle = 0;
  int8_t Before = -1, After = -1;
  size_t Order = SIZE_MAX;
  std::map<unsigned, uint64_t> Ids; // name id -> macro state, first sighting
  llvm::SmallSetVector<int, 8> Uses, Deps;
  bool UsesMain = false, DepsMain = false, Param = false;
  bool MainRef = false; // the main file used one of its declarations
  // For the server's checks: Before and After under the fixed observer; the
  // full pragma state at its first token; SV_* flags; the names it declares
  // at file scope (2.5's bindings), and those of the file-scope entities it
  // references, as [namespace (0: ordinary, 1: tag), name id].
  int8_t SBefore = -1, SAfter = -1;
  uint64_t Prag = 0;
  uint32_t Server = 0;
  std::vector<std::pair<uint8_t, unsigned>> Binds;
  llvm::SmallSetVector<std::pair<uint8_t, unsigned>, 8> Refs;
  // The identifiers it handed the parser: every name its parse could have
  // looked up, a superset of Refs that also covers what the AST drops (a
  // vector_size(N) keeps no reference to N).
  std::set<unsigned> TokNames;
};

// Every name a declaration's parse resolved to a declaration (Lookup): references,
// types written by name, and the earlier declaration a redeclaration merges
// with. Bothner records only these (p.27). Also, for the oracle and blank.py
// only (not Lookup), what the meaning depends on without a lookup: the
// declarations of the tag a typedef names, and of the struct a member access
// or a designator reaches into.
class UseVisitor : public RecursiveASTVisitor<UseVisitor> {
public:
  std::function<void(SourceLocation, const Decl *, bool Lookup)> Ref;
  bool VisitDeclRefExpr(DeclRefExpr *E) {
    Ref(E->getLocation(), E->getDecl(), true);
    return true;
  }
  bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl(), true);
    const Type *T = TL.getDecl()->getUnderlyingType().getCanonicalType().getTypePtr();
    while (T->isAnyPointerType() || T->isArrayType())
      T = T->getPointeeOrArrayElementType()->getCanonicalTypeInternal().getTypePtr();
    if (const TagDecl *Tag = T->getAsTagDecl())
      Ref(TL.getBeginLoc(), Tag, false);
    return true;
  }
  bool VisitRecordTypeLoc(RecordTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl(), true);
    return true;
  }
  bool VisitEnumTypeLoc(EnumTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl(), true);
    return true;
  }
  bool VisitDecl(Decl *D) {
    if (Decl *P = D->getPreviousDecl())
      Ref(D->getLocation(), P, true);
    // An attribute's function argument, which the traversal does not visit.
    if (auto *A = D->getAttr<CleanupAttr>())
      if (FunctionDecl *F = A->getFunctionDecl())
        Ref(A->getLocation(), F, true);
    return true;
  }
  // __builtin_convertvector's destination type, which the traversal does not
  // visit.
  bool VisitConvertVectorExpr(ConvertVectorExpr *E) {
    if (TypeSourceInfo *T = E->getTypeSourceInfo())
      TraverseTypeLoc(T->getTypeLoc());
    return true;
  }
  // Every tag declaration, for the tags a failed lookup declared at block
  // scope.
  std::function<void(const TagDecl *)> Tag;
  bool VisitTagDecl(TagDecl *D) {
    if (Tag)
      Tag(D);
    return true;
  }
  bool VisitMemberExpr(MemberExpr *E) {
    if (auto *F = dyn_cast<FieldDecl>(E->getMemberDecl()))
      Ref(E->getMemberLoc(), F->getParent(), false);
    return true;
  }
  bool VisitDesignatedInitExpr(DesignatedInitExpr *E) {
    for (const DesignatedInitExpr::Designator &D : E->designators())
      if (D.isFieldDesignator())
        if (const FieldDecl *F = D.getFieldDecl())
          Ref(D.getFieldLoc(), F->getParent(), false);
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
    uint8_t Kind = 0; // a directive's SEP_* kind
    unsigned End = 0; // a directive's: just after its line
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
    // Just after the line holding the end of a directive's last token.
    auto lineEnd = [&](unsigned P) {
      while (P < Buf.size() && Buf[P] != '\n')
        ++P;
      return std::min<unsigned>(P + 1, Buf.size());
    };
    while (T.isNot(tok::eof)) {
      if (T.is(tok::hash) && T.isAtStartOfLine()) {
        Item D{off(T), off(T), true, false, false};
        D.End = lineEnd(off(T) + 1);
        L.LexFromRawLexer(T);
        if (T.is(tok::eof) || T.isAtStartOfLine()) {
          V.push_back(D); // the null directive: no effect
          continue;
        }
        D.NameOff = off(T);
        D.Kind = SEP_OTHER;
        if (T.is(tok::raw_identifier)) {
          StringRef N = T.getRawIdentifier();
          D.DefineOrUndef = N == "define" || N == "undef";
          D.Opens = N == "if" || N == "ifdef" || N == "ifndef";
          if (D.DefineOrUndef)
            D.Kind = SEP_DEFINE;
          else if (D.Opens || N == "elif" || N == "elifdef" || N == "elifndef" || N == "else" ||
                   N == "endif")
            D.Kind = SEP_COND;
          else if (N == "include" || N == "include_next" || N == "import")
            D.Kind = SEP_INCLUDE;
          else if (N == "pragma")
            D.Kind = SEP_PRAGMA;
        }
        unsigned Last = off(T) + T.getLength();
        L.LexFromRawLexer(T);
        while (T.isNot(tok::eof) && !T.isAtStartOfLine()) {
          Last = off(T) + T.getLength();
          L.LexFromRawLexer(T);
        }
        D.End = lineEnd(Last);
        V.push_back(D);
        continue;
      }
      V.push_back(Item{off(T), off(T), false, false, false});
      L.LexFromRawLexer(T);
    }
    return V;
  }

  // The option-2 fragments of one header entry, in order, and the option-3
  // spans holding them.
  void fragments(FileID FID, unsigned Entry, std::vector<Frag> &Out,
                 std::vector<Span> &Spans) {
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
    int Cur = -1; // the open span
    unsigned Key = 0; // just after the last processed directive
    uint8_t Sep2 = SEP_FIRST, Sep3 = SEP_FIRST;
    auto open = [&](unsigned Off) {
      if (Cur < 0) {
        Spans.push_back({Entry, Off, ~0u, Key, Sep3});
        Cur = Spans.size() - 1;
        Sep3 = 0;
      }
    };
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
        open(I.Off);
        if (!Run2) {
          Run2 = true;
          Out.push_back({Entry, I.Off, I.Off, 0, Start3, Key, Sep2});
          Sep2 = 0;
          Spans[Cur].Members.push_back(Out.size() - 1);
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
      Sep2 |= I.Kind;
      if (I.DefineOrUndef) {
        open(I.Off);
        ++Spans[Cur].Defines;
      } else {
        Run3 = false;
        if (Cur >= 0)
          Spans[Cur].End = I.Off;
        Cur = -1;
        Sep3 |= I.Kind;
      }
      Key = I.End;
    }
    if (Cur >= 0)
      Spans[Cur].End = SM.getFileIDSize(FID);
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

  void HandleTranslationUnit(ASTContext &Ctx) override {
    S.T1 = __rdtsc();
    S.C1 = std::chrono::steady_clock::now();
    const char *Out = getenv("BOTHNER_TRACE_OUT");
    if (!Out)
      return;
    SourceManager &SM = CI.getSourceManager();
    FileID Main = SM.getMainFileID();

    // Fragment instances, and per header FileID the first of its fragments.
    std::vector<Frag> Frags;
    std::vector<Span> Spans;
    llvm::DenseMap<FileID, std::pair<unsigned, unsigned>> Range, SpanRange; // [first, end)
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
      unsigned First = Frags.size(), FirstSpan = Spans.size();
      fragments(FID, Entries.size(), Frags, Spans);
      Range[FID] = {First, unsigned(Frags.size())};
      SpanRange[FID] = {FirstSpan, unsigned(Spans.size())};
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
    // The span holding the #define or #undef whose name is at Raw.
    auto spanOf = [&](SourceLocation::UIntTy Raw) -> int {
      SourceLocation L = SourceLocation::getFromRawEncoding(Raw);
      if (L.isInvalid())
        return -1;
      auto [FID, Off] = SM.getDecomposedLoc(SM.getExpansionLoc(L));
      auto It = SpanRange.find(FID);
      if (It == SpanRange.end())
        return -1;
      auto [B, E] = It->second;
      auto P = std::upper_bound(Spans.begin() + B, Spans.begin() + E, Off,
                                [](unsigned O, const Span &X) { return O < X.Start; });
      if (P == Spans.begin() + B || Off >= (P - 1)->End)
        return -1;
      return int(P - 1 - Spans.begin());
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
    int LastDef = -1; // the span of the last directive event, if a #define or #undef
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
        if (!Parser && LastDef >= 0)
          Spans[LastDef].DefTicks += T; // the end of a #define, up to this token
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
        int Sp = E.D == D_define || E.D == D_undef ? spanOf(E.Loc) : -1;
        uint64_t Mine = T; // what this directive gets
        if (Parser) {
          uint64_t ToDir = std::min(T, Calib[E.D]);
          add(O, ToDir);
          add(LastTok, T - ToDir);
          if (LastTok >= 0)
            Frags[LastTok].Edge += T - ToDir;
          Split += T;
          Calibrated += ToDir;
          Mine = ToDir;
        } else {
          add(O, T);
        }
        if (Sp >= 0)
          Spans[Sp].DefTicks += Mine;
        LastDef = Sp;
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
    std::vector<std::array<uint64_t, 3>> MainDecls; // order, namespace, name id
    unsigned PragmaTokens = 0;
    if (S.Record) {
      // The server's view at each directive event:
      //   VIEW_STRICT   between declarations under the fixed observer (2.9):
      //                 the observer says so, and the last token handed to the
      //                 parser ended a declaration (';' at file scope, a
      //                 function body's '}') or a pragma, or there was none;
      //   VIEW_PENDING  a parser pragma's action may not have run yet: no
      //                 token from outside the pragma's line has been handed
      //                 out since its annotation token (2.4);
      //   VIEW_ARGS     inside a function-like macro invocation's arguments.
      std::vector<uint8_t> View(S.Events.size());
      std::sort(S.Invocations.begin(), S.Invocations.end());
      std::vector<SourceLocation::UIntTy> MaxEnd(S.Invocations.size());
      for (size_t K = 0; K < S.Invocations.size(); ++K)
        MaxEnd[K] = std::max(K ? MaxEnd[K - 1] : 0, S.Invocations[K].second);
      auto lineEnd = [&](FileID F, unsigned Off) -> unsigned {
        bool Invalid = false;
        StringRef Buf = SM.getBufferData(F, &Invalid);
        if (Invalid)
          return Off + 1;
        size_t P = Off;
        while (P < Buf.size() && !(Buf[P] == '\n' && (P == 0 || Buf[P - 1] != '\\')))
          ++P;
        return unsigned(P);
      };
      {
        int PrevKind = -1;
        bool InPragma = false; // the last token was a pragma's annotation or on its line
        FileID PragFID;
        unsigned PragEnd = 0;
        for (size_t I = 0; I < S.Events.size(); ++I) {
          const Ev &E = S.Events[I];
          if (E.Kind == E_TOK) {
            unsigned K = S.Info[I] & 0xFFFF;
            auto [FID, Off] =
                SM.getDecomposedExpansionLoc(SourceLocation::getFromRawEncoding(E.Loc));
            if (tok::isPragmaAnnotation(tok::TokenKind(K))) {
              ++PragmaTokens;
              InPragma = true;
              PragFID = FID;
              PragEnd = lineEnd(FID, Off);
            } else if (InPragma && !(FID == PragFID && Off < PragEnd)) {
              InPragma = false;
            }
            PrevKind = K;
          } else if (E.Kind == E_DIR) {
            unsigned Ctx = S.Info[I] & 0xFFFF;
            bool Ended = !(S.Info[I] & INFO_PROTO) &&
                         (PrevKind < 0 || InPragma ||
                          (PrevKind == tok::semi && Ctx == Decl::TranslationUnit) ||
                          (PrevKind == tok::r_brace && Ctx == Decl::Function));
            uint8_t V = (E.Boundary && Ended ? VIEW_STRICT : 0) | (InPragma ? VIEW_PENDING : 0);
            auto P = std::upper_bound(S.Invocations.begin(), S.Invocations.end(),
                                      std::make_pair(E.Loc, ~SourceLocation::UIntTy(0)));
            if (P != S.Invocations.begin() && MaxEnd[P - S.Invocations.begin() - 1] >= E.Loc)
              V |= VIEW_ARGS;
            View[I] = V;
          }
        }
      }
      // At file scope a '{' only opens a function body: a directive right
      // before one is inside a definition, where the design's fixed
      // observer is still raised (INFO_PROTO already covers K&R parameter
      // declarations). And a #pragma whose handler queued an annotation
      // token is pending at its own end, before the parser has the token.
      {
        int NextKind = -1;
        bool Annot = false;
        for (size_t I = S.Events.size(); I-- > 0;) {
          if (S.Events[I].Kind == E_TOK) {
            NextKind = S.Info[I] & 0xFFFF;
            Annot = tok::isPragmaAnnotation(tok::TokenKind(NextKind));
          } else if (S.Events[I].Kind == E_DIR) {
            if (NextKind == tok::l_brace)
              View[I] &= ~VIEW_STRICT;
            if (Annot) {
              View[I] |= VIEW_PENDING;
              if (S.Events[I].D == D_pragma)
                Annot = false;
            }
          }
        }
      }
      // SV_* flags from the view and the pragma flags at an event.
      auto server = [&](size_t I) -> uint32_t {
        uint32_t F = S.Info[I] >> 16;
        return (View[I] & VIEW_ARGS ? SV_ARGS : 0) | (View[I] & VIEW_PENDING ? SV_PENDING : 0) |
               (F & PF_WEAK ? SV_WEAK : 0) | (F & PF_ATTRIBUTE ? SV_ATTRIBUTE : 0);
      };

      bool Last = true;
      uint64_t LastPragma = 0;
      size_t LastDir = SIZE_MAX;
      int Cur = -1;
      std::vector<std::pair<unsigned, size_t>> MainToks; // offset, event number
      for (size_t I = 0; I < S.Events.size(); ++I) {
        const Ev &E = S.Events[I];
        if (E.Kind == E_DIR) {
          Last = E.Boundary;
          LastPragma = S.Aux[I].second;
          LastDir = I;
          if (Cur >= 0 && R[Cur].After < 0) {
            R[Cur].After = E.Boundary;
            R[Cur].SAfter = View[I] & VIEW_STRICT;
            if (View[I] & VIEW_ARGS) // it ends inside a macro's arguments
              R[Cur].Server |= SV_ARGS;
          }
          Cur = -1;
          if (E.D == D_define || E.D == D_undef) {
            Defs.push_back({I, S.Aux[I].first, E.D == D_define});
            if (int Sp = spanOf(E.Loc); Sp >= 0) {
              Span &X = Spans[Sp];
              if (X.DefOrder == SIZE_MAX) {
                X.DefOrder = I;
                X.DefBefore = E.Boundary;
                X.DefPragma = S.Aux[I].second;
                X.DefSBefore = View[I] & VIEW_STRICT;
                X.DefPrag = S.Prag[I];
                X.DefServer = server(I);
              }
              X.DefAfter = E.Boundary;
              X.DefSAfter = View[I] & VIEW_STRICT;
              X.Names.push_back({I, unsigned(S.Aux[I].first)});
            }
          }
        } else if (E.Kind == E_TOK) {
          int O = ownerOf(E.Loc, O_LOOSE);
          if (O == O_MAIN) {
            auto [FID, Off] =
                SM.getDecomposedExpansionLoc(SourceLocation::getFromRawEncoding(E.Loc));
            MainToks.push_back({Off, I});
          }
          if (O < 0)
            continue;
          Rec &F = R[O];
          if (F.Order == SIZE_MAX) {
            F.Order = I;
            F.Before = Last;
            F.Pragma = LastPragma;
            if (LastDir != SIZE_MAX) {
              F.SBefore = View[LastDir] & VIEW_STRICT;
              F.Server = server(LastDir) & (SV_ARGS | SV_PENDING);
            } else {
              F.SBefore = 1;
            }
            F.Prag = S.Prag[I];
            F.Server |= server(I) & (SV_WEAK | SV_ATTRIBUTE);
            if (Cur >= 0 && Cur != O && R[Cur].After < 0) {
              // A directive with no callback (the null directive, #line,
              // #warning, #error) ended the previous fragment: no view of
              // the parser there, so both count as inside a declaration.
              R[Cur].After = R[Cur].SAfter = 0;
              F.Before = F.SBefore = 0;
            }
          }
          F.Hash = (F.Hash ^ S.Aux[I].first) * FNVPrime;
          if (S.Aux[I].second)
            F.TokNames.insert(unsigned(S.Aux[I].second - 1));
          Cur = O;
        } else if (E.Kind == E_MAC) {
          int O = ownerOf(E.Loc, O_LOOSE, /*Count=*/false);
          if (O >= 0)
            R[O].Ids.insert({unsigned(S.Aux[I].first), S.Aux[I].second});
        }
      }

      // Never-reusable builtins.
      for (auto [L, What] : S.Builtins) {
        SourceLocation X = SM.getExpansionLoc(L);
        int O = ownerOf(X.getRawEncoding(), O_LOOSE, /*Count=*/false);
        if (O < 0)
          continue;
        if (What == 2) {
          auto [FID, Off] = SM.getDecomposedLoc(X);
          auto It = S.LineDirective.find(FID);
          if (It == S.LineDirective.end() || Off < It->second)
            continue;
        }
        R[O].Server |= SV_BUILTIN;
      }

      // Uses and Deps, from the declarations each fragment made.
      auto declOwner = [&](const Decl *D) {
        return ownerOf(SM.getExpansionLoc(D->getLocation()).getRawEncoding(), O_PRE,
                       /*Count=*/false);
      };
      auto startOf = [&](int F) {
        return SM.getComposedLoc(EntryFID[Frags[F].Entry], Frags[F].Start);
      };
      // In C a tag or enumerator inside a struct is at file scope too.
      auto fileScope = [](const Decl *D) {
        const DeclContext *DC = D->getDeclContext();
        while (DC && isa<TagDecl>(DC))
          DC = DC->getParent();
        return DC && DC->isTranslationUnit();
      };
      UseVisitor V;
      V.Ref = [&](SourceLocation At, const Decl *D, bool Lookup) {
        int U = ownerOf(SM.getExpansionLoc(At).getRawEncoding(), O_PRE, false);
        if (U == O_MAIN && D) {
          // Which fragments the main file depends on: only those it does not
          // can be blanked to check the time model (blank.py).
          for (const Decl *Q : D->redecls())
            if (!Q->isImplicit())
              if (int OQ = declOwner(Q); OQ >= 0)
                R[OQ].MainRef = true;
          return;
        }
        if (U < 0 || !D)
          return;
        if (auto *ND = dyn_cast<NamedDecl>(D); ND && ND->getIdentifier() && fileScope(D))
          R[U].Refs.insert({uint8_t(isa<TagDecl>(D)), S.name(ND->getIdentifier())});
        // The entity's first declaration that is not implicit. Usually its
        // first declaration; for a library builtin (memcpy), the header's:
        // GCC's builtins were there from the start (p.23), but in C99 a call
        // to one is valid only once a header declared it (ours).
        const Decl *First = nullptr;
        for (const Decl *Q : D->redecls())
          if (!Q->isImplicit() &&
              (!First || SM.isBeforeInTranslationUnit(Q->getLocation(), First->getLocation())))
            First = Q;
        if (!First)
          return; // a builtin no header declares
        if (Lookup) {
          int O = declOwner(First);
          if (O >= 0 && O != U && R[O].Order < R[U].Order)
            R[U].Uses.insert(O);
          else if (O == O_MAIN && SM.isBeforeInTranslationUnit(First->getLocation(), startOf(U)))
            R[U].UsesMain = true;
        }
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
      // A tag first named at block scope without a definition, as in
      // `struct x *p;` inside a function: the file-scope lookup found nothing,
      // a negative dependency (with a file-scope `struct x` it would be found).
      V.Tag = [&](const TagDecl *TD) {
        if (!TD->getIdentifier() || fileScope(TD) || TD->isThisDeclarationADefinition() ||
            TD->getPreviousDecl())
          return;
        int U = ownerOf(SM.getExpansionLoc(TD->getLocation()).getRawEncoding(), O_PRE, false);
        if (U >= 0)
          R[U].Refs.insert({1, S.name(TD->getIdentifier())});
      };
      for (DeclGroupRef G : S.Groups)
        for (Decl *D : G)
          V.TraverseDecl(D);

      // What each fragment declares at file scope, from the translation
      // unit's declarations (so also a tag a declaration only names), and
      // each main-file declaration with the event number of its name.
      auto mainOrder = [&](const Decl *D) -> uint64_t {
        unsigned Off = SM.getDecomposedExpansionLoc(D->getLocation()).second;
        auto P = std::lower_bound(MainToks.begin(), MainToks.end(), std::make_pair(Off, size_t(0)));
        return P == MainToks.end() ? S.Events.size() : P->second;
      };
      std::function<void(const DeclContext *)> declare = [&](const DeclContext *DC) {
        for (const Decl *D : DC->decls()) {
          if (D->isImplicit() || (!DC->isTranslationUnit() && !isa<TagDecl, EnumConstantDecl>(D)))
            continue;
          if (auto *ND = dyn_cast<NamedDecl>(D); ND && ND->getIdentifier()) {
            int O = declOwner(D);
            uint8_t NS = isa<TagDecl>(D);
            unsigned Id = S.name(ND->getIdentifier());
            if (O >= 0)
              R[O].Binds.push_back({NS, Id});
            else if (O == O_MAIN)
              MainDecls.push_back({mainOrder(D), NS, Id});
            if (auto *FD = dyn_cast<FunctionDecl>(D); FD && O >= 0)
              if (!FD->doesThisDeclarationHaveABody() &&
                  (!FD->isExternallyVisible() || FD->isInlineSpecified())) {
                // Not when the same fragment defines it: its reuse restores
                // the definition too.
                const FunctionDecl *Def = nullptr;
                bool Defined = FD->isDefined(Def);
                if (!Defined || declOwner(Def) != O)
                  R[O].Server |= SV_UNDEFINED | (Defined ? 0 : SV_UNDEFINED_END);
              }
          }
          if (auto *Tag = dyn_cast<TagDecl>(D); Tag && Tag->isThisDeclarationADefinition())
            declare(Tag);
        }
      };
      declare(Ctx.getTranslationUnitDecl());

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
       << "\",\"full_config\":\"" << llvm::format_hex_no_prefix(S.FullConfig, 16)
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
    // parameter list, 8: the main file uses it), uses, [[name id, macro state (low 32 bits; 0: not a
    // macro)], ...], deps]. The simulator recomputes the oracle from the
    // token hash, the pragma state and the deps, leaving out deps at what its
    // rule counts as the fragment's own location. Then, for the server's
    // checks: [nesting before, nesting after, under the fixed observer; full
    // pragma state; SV_* flags; [[namespace, name id] it declares], [[namespace,
    // name id] it references]; its key (Frag::Key); what separates it from the
    // previous fragment (SEP_*); [name ids of the identifiers it handed the
    // parser]].
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
           << "\"," << (X.UsesMain | X.DepsMain << 1 | X.Param << 2 | X.MainRef << 3) << ",[";
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
        OS << "]," << int(X.SBefore) << "," << int(X.SAfter) << ",\"" << hex(X.Prag) << "\","
           << X.Server << ",[";
        for (size_t K = 0; K < X.Binds.size(); ++K)
          OS << (K ? "," : "") << "[" << int(X.Binds[K].first) << "," << X.Binds[K].second << "]";
        OS << "],[";
        for (size_t K = 0; K < X.Refs.size(); ++K)
          OS << (K ? "," : "") << "[" << int(X.Refs[K].first) << "," << X.Refs[K].second << "]";
        OS << "]," << F.Key << "," << int(F.Sep) << ",[";
        bool FirstName = true;
        for (unsigned N : X.TokNames) {
          OS << (FirstName ? "" : ",") << N;
          FirstName = false;
        }
        OS << "]";
      }
      OS << "]";
    }
    // Option-3 spans: [entry, start, end, [option-2 fragments], #defines and
    // #undefs, their ns]; in record mode also [first #define's event number
    // (-1: none), nesting there, nesting at the last, pragma state there,
    // [[event number, name id] of each #define or #undef], nesting at the
    // first and last under the fixed observer, full pragma state and SV_*
    // flags at the first, its key, what separates it from the previous span].
    OS << "],\"spans\":[";
    for (size_t I = 0; I < Spans.size(); ++I) {
      const Span &X = Spans[I];
      OS << (I ? "," : "") << "[" << X.Entry << "," << X.Start << "," << X.End << ",[";
      for (size_t K = 0; K < X.Members.size(); ++K)
        OS << (K ? "," : "") << X.Members[K];
      OS << "]," << X.Defines << "," << ns(X.DefTicks);
      if (S.Record) {
        OS << "," << (X.DefOrder == SIZE_MAX ? -1 : (long long)X.DefOrder) << "," << int(X.DefBefore)
           << "," << int(X.DefAfter) << ",\"" << hex(X.DefPragma) << "\",[";
        for (size_t K = 0; K < X.Names.size(); ++K)
          OS << (K ? "," : "") << "[" << X.Names[K].first << "," << X.Names[K].second << "]";
        OS << "]," << int(X.DefSBefore) << "," << int(X.DefSAfter) << ",\"" << hex(X.DefPrag)
           << "\"," << X.DefServer << "," << X.Key << "," << int(X.Sep);
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
      // Every file-scope declaration of the main file: [event number of its
      // name, namespace, name id].
      OS << "],\"pragma_tokens\":" << PragmaTokens
         << ",\"line_directives\":" << S.LineDirective.size() << ",\"main_decls\":[";
      for (size_t I = 0; I < MainDecls.size(); ++I)
        OS << (I ? "," : "") << "[" << MainDecls[I][0] << "," << MainDecls[I][1] << ","
           << MainDecls[I][2] << "]";
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

// The compile server's configuration key (COMPILE_SERVER_DESIGN.md 2.1): the
// above, and the rest of the parsed invocation the front end reads: every
// LangOption, including those getModuleHash skips as benign, NoBuiltinFuncs,
// the target options, every -D, -U and -include as written, and every warning
// option.
uint64_t fullConfiguration(CompilerInstance &CI, uint64_t Model) {
  std::string K;
  auto Num = [&](uint64_t V) {
    K.append(reinterpret_cast<const char *>(&V), sizeof(V));
  };
  auto Str = [&](StringRef V) {
    K += V;
    K += '\0';
  };
  Num(Model);
  const LangOptions &LO = CI.getLangOpts();
#define LANGOPT(Name, Bits, Default, Compatibility, Description) Num(LO.Name);
#define ENUM_LANGOPT(Name, Type, Bits, Default, Compatibility, Description)                   \
  Num(static_cast<uint64_t>(LO.get##Name()));
#include "clang/Basic/LangOptions.def"
  for (const std::string &F : LO.NoBuiltinFuncs)
    Str(F);
  const TargetOptions &TO = CI.getTargetOpts();
  Str(TO.Triple);
  Str(TO.CPU);
  Str(TO.TuneCPU);
  Str(TO.FPMath);
  Str(TO.ABI);
  for (const std::string &F : TO.FeaturesAsWritten)
    Str(F);
  for (const std::string &F : TO.Features)
    Str(F);
  for (const auto &[M, Undef] : CI.getPreprocessorOpts().Macros) {
    Str(M);
    Num(Undef);
  }
  for (const std::string &F : CI.getPreprocessorOpts().Includes)
    Str(F);
  for (const std::string &W : CI.getDiagnosticOpts().Warnings)
    Str(W);
  for (const std::string &W : CI.getDiagnosticOpts().Remarks)
    Str(W);
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
    S->FullConfig = fullConfiguration(CI, S->Config);
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
