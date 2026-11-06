// BothnerServer: Bothner's 2003 GCC compile server, realized in Clang
// (clang/lib/ASTCache/COMPILE_SERVER_DESIGN.md).
//
// A long-lived clang -cc1 that serves compiles one at a time (2.1). For each
// configuration, the working directory and the full parsed invocation, it
// keeps the FileManager, SourceManager, TargetInfo, DiagnosticsEngine, the
// identifier, selector and builtin tables, and the ASTContext (2.2). Each
// compile gets its own HeaderSearch, Preprocessor, Sema, Parser, consumer and
// translation unit, after the resets of 2.3.
//
// With reuse=1 (steps S2-S4) the server also keeps every header fragment it
// parses: the text between two directives, option 2 of the paper. At each
// fragment start the preprocessor asks the server (HeaderCacheHook's
// fragment calls). The server either reuses the stored version, so the
// preprocessor skips the text and a queued annot_module_include restores the
// stored declarations once the parser is back at file scope (2.4, 2.5), or
// lets the fragment be parsed and records it (3). The rule is R0, p.28's
// versioning, with the checks of 2.6; the undo of 2.8 runs at each compile's
// start. Without reuse=1 nothing is kept (step S1).
//
// The requests come from a file, one JSON object a line:
//   {"cwd": "<dir>", "args": ["<cc1 argument>", ...], "dump": "<path>",
//    "build": "<id>"}
// "args" are the arguments after -cc1. The first request of a new "build"
// checks every file the configuration read before (2.10). With -emit-llvm among them the module
// is printed to the -o path (the validation mode of section 5), and restored
// declarations go to the code generator as a parse would hand them over.
// "dump", when present, receives the declaration summary of section 5. One
// JSON line a request goes to the results file: status, diagnostics, times,
// address space, memory, and the decision counts.
//
//   clang -cc1 -load BothnerServer.so -plugin bothner-server \
//     -plugin-arg-bothner-server requests=<file> \
//     -plugin-arg-bothner-server results=<file> \
//     [-plugin-arg-bothner-server reuse=1] -x c /dev/null
//
// Loaded into plain clang with -fplugin, the same library adds the action
// bothner-dump, which writes the same declaration summary to
// $BOTHNER_DUMP_OUT at the end of the compile, for the comparison. The
// server's process also runs it on its dummy input, so the server must not
// see that variable.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/CommentCommandTraits.h"
#include "clang/ASTCache/CacheProtocol.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTCache/MacroTokens.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/SourceManagerInternals.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/BackendUtil.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/DeclBoundaryObserver.h"
#include "clang/Lex/ExpansionCandidateObserver.h"
#include "clang/Lex/HeaderCacheHook.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/ModuleLoader.h"
#include "clang/Lex/ModuleMap.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_socket_stream.h"
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace clang;

namespace {

// Private members that the resets of 2.3 and the bindings of 3 need. An
// explicit instantiation skips access checks, so no Clang header changes.
template <typename Tag, typename Tag::type M> struct Rob {
  friend typename Tag::type get(Tag) { return M; }
};
struct ExternCContextTag {
  using type = ExternCContextDecl *ASTContext::*;
  friend type get(ExternCContextTag);
};
struct TypeInfoTag {
  using type = llvm::DenseMap<const Type *, TypeInfo> ASTContext::*;
  friend type get(TypeInfoTag);
};
struct UnadjustedAlignTag {
  using type = llvm::DenseMap<const Type *, unsigned> ASTContext::*;
  friend type get(UnadjustedAlignTag);
};
struct RecordLayoutsTag {
  using type = llvm::DenseMap<const RecordDecl *, const ASTRecordLayout *>
      ASTContext::*;
  friend type get(RecordLayoutsTag);
};
struct CommentCommandsTag {
  using type = SmallVector<comments::CommandInfo *, 4> comments::CommandTraits::*;
  friend type get(CommentCommandsTag);
};
struct TokenCountTag {
  using type = unsigned Preprocessor::*;
  friend type get(TokenCountTag);
};
struct LastDeclTag {
  using type = Decl *DeclContext::*;
  friend type get(LastDeclTag);
};
// The file check of 2.10: FileManager's caches of names seen, and of files
// by identity.
struct SeenFilesTag {
  using type = llvm::StringMap<llvm::ErrorOr<FileEntryRef::MapValue>,
                               llvm::BumpPtrAllocator> FileManager::*;
  friend type get(SeenFilesTag);
};
struct SeenDirsTag {
  using type = llvm::StringMap<llvm::ErrorOr<DirectoryEntry &>,
                               llvm::BumpPtrAllocator> FileManager::*;
  friend type get(SeenDirsTag);
};
struct UniqueFilesTag {
  using type = llvm::DenseMap<llvm::sys::fs::UniqueID, FileEntry *> FileManager::*;
  friend type get(UniqueFilesTag);
};
template struct Rob<ExternCContextTag, &ASTContext::ExternCContext>;
template struct Rob<TypeInfoTag, &ASTContext::MemoizedTypeInfo>;
template struct Rob<UnadjustedAlignTag, &ASTContext::MemoizedUnadjustedAlign>;
template struct Rob<RecordLayoutsTag, &ASTContext::ASTRecordLayouts>;
template struct Rob<TokenCountTag, &Preprocessor::TokenCount>;
template struct Rob<CommentCommandsTag, &comments::CommandTraits::RegisteredCommands>;
template struct Rob<LastDeclTag, &DeclContext::LastDecl>;
template struct Rob<SeenFilesTag, &FileManager::SeenFileEntries>;
template struct Rob<SeenDirsTag, &FileManager::SeenDirEntries>;
template struct Rob<UniqueFilesTag, &FileManager::UniqueRealFiles>;

// The chain repair of 2.8: a redeclaration chain's latest pointer lives in
// its first declaration (Redeclarable.h). A class derived from Redeclarable
// may form a pointer to that protected member.
template <typename T> struct ChainAccess : Redeclarable<T> {
  static void setLatest(T *D) {
    auto Link = &ChainAccess::RedeclLink;
    (D->getFirstDecl()->*Link).setLatest(D);
  }
};

void repairChain(Decl *D) {
  if (auto *F = dyn_cast<FunctionDecl>(D))
    ChainAccess<FunctionDecl>::setLatest(F);
  else if (auto *V = dyn_cast<VarDecl>(D))
    ChainAccess<VarDecl>::setLatest(V);
  else if (auto *T = dyn_cast<TypedefNameDecl>(D))
    ChainAccess<TypedefNameDecl>::setLatest(T);
  else if (auto *G = dyn_cast<TagDecl>(D))
    ChainAccess<TagDecl>::setLatest(G);
}

// Decl::Used, which records that an entity was used (odr-used), is private
// and has no clearing setter, but each compile must start with it clear on
// the declarations it can restore (2.8). Its place is found once, from a
// declaration made for the purpose: the one bit setIsUsed changes.
struct UsedBitPlace {
  size_t Byte = 0;
  unsigned char Mask = 0;
};
UsedBitPlace findUsedBit(ASTContext &Ctx) {
  Decl *D = EmptyDecl::Create(Ctx, Ctx.getTranslationUnitDecl(),
                              SourceLocation());
  unsigned char Before[sizeof(EmptyDecl)], After[sizeof(EmptyDecl)];
  memcpy(Before, static_cast<void *>(D), sizeof(EmptyDecl));
  D->setIsUsed();
  memcpy(After, static_cast<void *>(D), sizeof(EmptyDecl));
  UsedBitPlace P;
  unsigned Changed = 0;
  for (size_t I = 0; I < sizeof(EmptyDecl); ++I)
    if (unsigned char X = Before[I] ^ After[I]) {
      Changed += llvm::popcount(X);
      P = {I, X};
    }
  if (Changed != 1 || !D->isUsed(false))
    llvm::report_fatal_error("bothner-server: cannot find Decl::Used");
  return P;
}
void clearUsed(Decl *D, UsedBitPlace P) {
  reinterpret_cast<unsigned char *>(static_cast<void *>(D))[P.Byte] &= ~P.Mask;
  assert(!D->isUsed(false) && "Decl::Used not cleared");
}

double msSince(std::chrono::steady_clock::time_point T) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - T)
      .count();
}

double cpuMs() {
  timespec TS;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &TS);
  return TS.tv_sec * 1e3 + TS.tv_nsec / 1e6;
}

// The server's instructions, user and kernel, counted as perfc counts a
// compile's (header-cache-bench/perf/perfc.c): a compile's work runs here,
// not in the driver that the harness measures. 0 where the counter cannot
// be opened.
uint64_t instructions() {
  static int Fd = [] {
    perf_event_attr A;
    memset(&A, 0, sizeof A);
    A.size = sizeof A;
    A.type = PERF_TYPE_HARDWARE;
    A.config = PERF_COUNT_HW_INSTRUCTIONS;
    A.inherit = 1;
    return int(syscall(SYS_perf_event_open, &A, 0, -1, -1, 0));
  }();
  uint64_t V = 0;
  if (Fd < 0 || ::read(Fd, &V, sizeof V) != sizeof V)
    return 0;
  return V;
}

uint64_t rssKiB() {
  long Pages = 0, Resident = 0;
  if (FILE *F = fopen("/proc/self/statm", "r")) {
    if (fscanf(F, "%ld %ld", &Pages, &Resident) != 2)
      Resident = 0;
    fclose(F);
  }
  return Resident * (sysconf(_SC_PAGESIZE) / 1024);
}

constexpr uint64_t FNVBasis = 14695981039346656037ull, FNVPrime = 1099511628211ull;
uint64_t fnv(StringRef S, uint64_t H = FNVBasis) {
  for (unsigned char C : S)
    H = (H ^ C) * FNVPrime;
  return H;
}
uint64_t mix(uint64_t H, uint64_t V) { return (H ^ V) * FNVPrime ^ (V >> 29); }

// ---------------------------------------------------------------------------
// The declaration summary of section 5: every file-scope declaration of the
// compile, with its type, completeness and definition, record layout,
// attributes, storage class, linkage, whether it is a definition, and where
// its previous declaration is. Members of records and enums follow their
// parent, indented. Implicit declarations are left out, except as a previous
// declaration. The entries are sorted by location, then text, so that a
// compile whose declarations came partly from restores compares equal to a
// parse.
// ---------------------------------------------------------------------------

std::string where(const SourceManager &SM, SourceLocation L) {
  if (L.isInvalid())
    return "-";
  PresumedLoc P = SM.getPresumedLoc(L);
  if (P.isInvalid())
    return "?";
  return (llvm::Twine(P.getFilename()) + ":" + llvm::Twine(P.getLine()) + ":" +
          llvm::Twine(P.getColumn()))
      .str();
}

void dumpDecl(ASTContext &Ctx, const Decl *D, llvm::raw_ostream &OS,
              unsigned Depth) {
  if (D->isImplicit())
    return;
  const SourceManager &SM = Ctx.getSourceManager();
  PrintingPolicy Policy(Ctx.getLangOpts());
  OS.indent(2 * Depth) << D->getDeclKindName();
  if (const auto *ND = dyn_cast<NamedDecl>(D))
    OS << " '" << ND->getNameAsString() << "'";
  OS << " @" << where(SM, D->getLocation());
  if (D->isInvalidDecl())
    OS << " invalid";
  if (const auto *VD = dyn_cast<ValueDecl>(D))
    OS << " type=" << VD->getType().getAsString(Policy)
       << " canon=" << VD->getType().getCanonicalType().getAsString(Policy);
  if (const auto *TD = dyn_cast<TypedefNameDecl>(D))
    OS << " underlying=" << TD->getUnderlyingType().getAsString(Policy)
       << " canon="
       << TD->getUnderlyingType().getCanonicalType().getAsString(Policy);
  if (const auto *ND = dyn_cast<NamedDecl>(D))
    if (!isa<FieldDecl, EnumConstantDecl>(D))
      OS << " linkage=" << unsigned(ND->getFormalLinkage());
  if (const auto *FD = dyn_cast<FunctionDecl>(D))
    OS << " sc=" << unsigned(FD->getStorageClass())
       << " inline=" << FD->isInlineSpecified()
       << " def=" << FD->isThisDeclarationADefinition()
       << " defined=" << FD->isDefined() << " builtin=" << FD->getBuiltinID()
       << " variadic=" << FD->isVariadic();
  if (const auto *VD = dyn_cast<VarDecl>(D))
    if (!isa<ParmVarDecl>(D))
      OS << " sc=" << unsigned(VD->getStorageClass())
         << " def=" << unsigned(VD->isThisDeclarationADefinition())
         << " init=" << VD->hasInit() << " tls=" << unsigned(VD->getTLSKind());
  if (const auto *TD = dyn_cast<TagDecl>(D)) {
    OS << " complete=" << TD->isCompleteDefinition();
    if (const TagDecl *Def = TD->getDefinition())
      OS << " definition@" << where(SM, Def->getLocation());
  }
  if (const auto *RD = dyn_cast<RecordDecl>(D))
    if (RD->isCompleteDefinition() && !RD->isInvalidDecl()) {
      const ASTRecordLayout &L = Ctx.getASTRecordLayout(RD);
      OS << " size=" << L.getSize().getQuantity()
         << " align=" << L.getAlignment().getQuantity() << " offsets=";
      for (unsigned I = 0, E = L.getFieldCount(); I != E; ++I)
        OS << L.getFieldOffset(I) << ',';
    }
  if (const auto *ED = dyn_cast<EnumDecl>(D))
    if (ED->isComplete())
      OS << " int=" << ED->getIntegerType().getAsString(Policy);
  if (const auto *EC = dyn_cast<EnumConstantDecl>(D))
    OS << " value=" << llvm::toString(EC->getInitVal(), 10);
  if (const auto *FD = dyn_cast<FieldDecl>(D))
    if (FD->isBitField())
      OS << " bits=" << FD->getBitWidthValue();
  if (const Decl *Prev = D->getPreviousDecl())
    OS << " prev@" << where(SM, Prev->getLocation())
       << (Prev->isImplicit() ? "(implicit)" : "");
  for (const Attr *A : D->attrs()) {
    OS << " [";
    A->printPretty(OS, Policy);
    OS << (A->isInherited() ? " inherited]" : "]");
  }
  OS << '\n';
  if (isa<RecordDecl, EnumDecl>(D))
    for (const Decl *M : cast<DeclContext>(D)->decls())
      dumpDecl(Ctx, M, OS, Depth + 1);
}

// The compile's translation unit, and \p Restored: the top-level declarations
// that restores brought in from earlier compiles' translation units.
void dumpDecls(ASTContext &Ctx, llvm::raw_ostream &OS,
               ArrayRef<Decl *> Restored = {}) {
  // A layout only the summary asks for must not report (-Wpadded), and must
  // not stay: plain clang's summary runs before code generation, which would
  // then find the layout made and not report it either.
  DiagnosticsEngine &Diags = Ctx.getDiagnostics();
  bool Quiet = Diags.getSuppressAllDiagnostics();
  Diags.setSuppressAllDiagnostics(true);
  auto &Layouts = Ctx.*get(RecordLayoutsTag());
  auto &Infos = Ctx.*get(TypeInfoTag());
  auto &Unadjusted = Ctx.*get(UnadjustedAlignTag());
  llvm::DenseSet<const RecordDecl *> HadLayout;
  llvm::DenseSet<const Type *> HadInfo, HadUnadjusted;
  for (const auto &E : Layouts)
    HadLayout.insert(E.first);
  for (const auto &E : Infos)
    HadInfo.insert(E.first);
  for (const auto &E : Unadjusted)
    HadUnadjusted.insert(E.first);
  std::vector<std::pair<std::string, std::string>> Entries;
  auto Add = [&](const Decl *D) {
    if (D->isImplicit())
      return;
    std::string Text;
    llvm::raw_string_ostream TS(Text);
    dumpDecl(Ctx, D, TS, 0);
    Entries.push_back({where(Ctx.getSourceManager(), D->getLocation()), Text});
  };
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls())
    Add(D);
  for (const Decl *D : Restored)
    Add(D);
  llvm::stable_sort(Entries);
  for (const auto &E : Entries)
    OS << E.second;
  llvm::SmallVector<const RecordDecl *, 64> NewLayouts;
  llvm::SmallVector<const Type *, 64> NewInfos, NewUnadjusted;
  for (const auto &E : Layouts)
    if (!HadLayout.count(E.first))
      NewLayouts.push_back(E.first);
  for (const auto &E : Infos)
    if (!HadInfo.count(E.first))
      NewInfos.push_back(E.first);
  for (const auto &E : Unadjusted)
    if (!HadUnadjusted.count(E.first))
      NewUnadjusted.push_back(E.first);
  for (const RecordDecl *RD : NewLayouts)
    Layouts.erase(RD);
  for (const Type *T : NewInfos)
    Infos.erase(T);
  for (const Type *T : NewUnadjusted)
    Unadjusted.erase(T);
  Diags.setSuppressAllDiagnostics(Quiet);
}

// ---------------------------------------------------------------------------
// The server's state.
// ---------------------------------------------------------------------------

// Hands every compile's Preprocessor the server's own identifiers (2.2):
// never null, so a name is always the persistent table's entry.
class PersistentLookup : public IdentifierInfoLookup {
  IdentifierTable &Table;

public:
  explicit PersistentLookup(IdentifierTable &Table) : Table(Table) {}
  IdentifierInfo *get(StringRef Name) override { return &Table.get(Name); }
};

constexpr unsigned MainKey = ~0u;

// One declaration of a name, in the order a compile made it: the fragment
// version that declared it, or (MainKey, a serial that never repeats) for a
// main-file declaration (simulate.py's per-name sequences).
struct Seen {
  unsigned Key = 0, Version = 0;
  bool operator==(const Seen &O) const {
    return Key == O.Key && Version == O.Version;
  }
};

// A name as C looks it up: an identifier, ordinary or tag.
using Name = uintptr_t;
Name nameOf(const NamedDecl *D) {
  return reinterpret_cast<uintptr_t>(D->getIdentifier()) | isa<TagDecl>(D);
}

// Why a fragment was parsed rather than reused, in the order the checks run
// (2.6; simulate.py's order). W_Reused and W_Empty are the reuses.
enum Why : unsigned {
  W_Reused,
  W_Empty,
  W_NoSkip,
  W_Pending,
  W_Nested,
  W_Repeat,
  W_New,
  W_NestedEnd,
  W_Param,
  W_Main,
  W_Builtin,
  W_Undefined,
  W_Args,
  W_File,
  W_Diag,
  W_Uses,
  W_Macro,
  W_Pragma,
  W_Negative,
  W_Deps,
  W_Predecessor,
  W_Tokens,
  W_N
};
const char *WhyName[W_N] = {
    "reused",     "empty", "no skip", "pending", "nested",   "repeat",
    "new",        "nested end", "param", "main", "builtin",  "undefined",
    "macro args", "file",  "diagnostics", "uses",    "macro",   "pragma",   "negative",
    "deps",       "predecessor", "token names"};

// Option 3 (2.7): a #define or #undef line inside a fragment, kept so that
// a reuse makes the same definition. The tokens point into the file's text
// and the identifier table, which outlive the compile.
struct MacroEffect {
  IdentifierInfo *II = nullptr;
  bool Define = false;
  SourceLocation Loc, DefLoc, EndLoc;
  bool FunctionLike = false, C99Varargs = false, GNUVarargs = false,
       CommaPasting = false, AllowRedefinition = false;
  std::vector<IdentifierInfo *> Params;
  std::vector<Token> Tokens;
  // The definition's state (defState) in the compile that parsed it.
  uint64_t State = 0;
};

// Where the line table puts a fragment's start: whether the file is a
// system header there, and the #line, line marker or #pragma GCC
// system_header before it, if any: the name it gave (an index into the
// SourceManager's list of #line names, -1 for the file's own name), its
// presumed line minus the physical line, and the include it pretends. A
// restored declaration keeps the location its parse gave it, so a reuse
// must find the same place. Clang reads the kind only as system or not when
// it compiles (Sema, diagnostics, profile instrumentation); an extern "C"
// system header differs only in -E output.
struct LinePlace {
  bool System = false;
  int FilenameID = -2; // -2: no line entry before the start
  int Delta = 0;
  unsigned IncludeOffset = 0;
  bool operator==(const LinePlace &O) const {
    return System == O.System && FilenameID == O.FilenameID &&
           Delta == O.Delta && IncludeOffset == O.IncludeOffset;
  }
  bool operator!=(const LinePlace &O) const { return !(*this == O); }
};

// What a parse of a fragment left: kept until the next parse of the same
// location replaces it (R0 keeps one stored version a location, 2.6).
struct Version {
  unsigned Id = 0, Key = 0, End = 0;
  bool HadTokens = false, ExpandedMacro = false, Empty = false;
  // A reason it is never reused, found by its own parse.
  Why Never = W_Reused;
  // V1: each name that reached the macro-expansion decision, with its state
  // then (0: not a macro).
  std::vector<std::pair<const IdentifierInfo *, uint64_t>> Macros;
  uint64_t Pragma = 0;
  // The fragments whose declarations it used (p.28), and those that declared
  // earlier declarations of what it redeclares or uses, at the versions this
  // compile had.
  llvm::SmallSetVector<std::pair<unsigned, unsigned>, 8> Uses, Deps;
  // Its file-scope bindings in parse order, nested tags and enumerators
  // included (2.5), and the translation unit's declarations it made.
  std::vector<NamedDecl *> Binds;
  llvm::DenseSet<Name> BindNames;
  std::vector<Decl *> TopLevel;
  // For every name it declares or references: the declarations of that name
  // the compile had made before it.
  std::vector<std::pair<Name, std::vector<Seen>>> Lists;
  // Decision 9: the names its identifier tokens spell that it neither binds
  // nor references in the AST, in both name spaces, with the declarations
  // the compile had made of them before it. The AST drops some names the
  // parse looks up, such as an enumerator in vector_size(N * 4). Most of
  // these names had no declaration (locals, fields, attribute names).
  std::vector<Name> TokenAbsent;
  std::vector<std::pair<Name, std::vector<Seen>>> TokenLists;
  // Option 3: its #define and #undef lines, in order; and for a name such a
  // line names before any use, the state the line found. Plain clang
  // compares the two definitions there (-Wmacro-redefined), so a reuse needs
  // the same earlier one.
  std::vector<MacroEffect> Effects;
  std::vector<std::pair<const IdentifierInfo *, uint64_t>> Priors;
  // The file as its parse saw it: where the line table put the start, and
  // for a fragment that expanded __FILE__ or __FILE_NAME__, the presumed
  // name it spelled.
  LinePlace Place;
  uint64_t FileName = 0;
  // The diagnostic state at its start (Store::DiagStates): a reuse prints
  // nothing, so the parse must have printed nothing under the same state.
  unsigned DiagState = 0;
  // The tokens its parse handed the parser: a reuse counts them as read, as
  // -Wmax-tokens and #pragma clang max_tokens_here compare the count.
  unsigned NumTokens = 0;
  // Each binding's attributes and, for a variable, its type, as its parse
  // left them: later code changes both in place (#pragma weak and
  // redefine_extname; a tentative definition of incomplete array type
  // becomes T[1] at the end of the compile), and a restore puts them back
  // (2.8).
  std::vector<AttrVec> BindAttrs;
  std::vector<QualType> BindTypes;
  // Block-scope externs inside its functions, which a restore registers
  // again in the compile's extern "C" context (2.3).
  std::vector<NamedDecl *> LocalExterns;
  // The declarations whose parse entered their section in the ASTContext's
  // table, in parse order, with the section's kind; a restore enters them
  // again, so a later conflict is found as a parse finds it.
  std::vector<std::pair<NamedDecl *, int>> Sections;
  // The entities (first declarations) it odr-used, that were marked used
  // when its parse handed the declaration over: a restore marks them again.
  llvm::SetVector<Decl *> UsedDecls;
  // The file-scope declarations its expressions name, which its parse marked
  // referenced (Sema::MarkDeclRefReferenced): a restore clears the bit on
  // what it binds and marks these again. End-of-compile warnings read it
  // (-Wunused-function, -Wunneeded-internal-declaration).
  llvm::SetVector<Decl *> RefDecls;
  // Sema's lists the restore gives back (2.5).
  std::vector<VarDecl *> Tentative;
  std::vector<const DeclaratorDecl *> UnusedFileScoped;
  std::vector<TypedefNameDecl *> ExtVector;
  // While recording only.
  llvm::DenseSet<Name> Refs;
  llvm::DenseSet<const IdentifierInfo *> MacroSeen;
  std::vector<const IdentifierInfo *> Tokens;
  // Option 3: names it defined or removed, whose later uses depend on the
  // fragment alone.
  llvm::DenseSet<const IdentifierInfo *> DefinedHere;
};

// The fragments of one configuration.
struct Store {
  // A key per (file, reading << 32 | offset): the reading is 0 unless
  // keys=reading numbers a location's readings in a compile (decision 8).
  llvm::DenseMap<std::pair<const FileEntry *, uint64_t>, unsigned> KeyOf;
  std::vector<std::unique_ptr<Version>> Latest; // per key
  // Who declared each binding: the version, or MainKey for the main file.
  llvm::DenseMap<const Decl *, Seen> Owner;
  unsigned NextVersion = 1, NextMain = 1;
  // The undo of 2.8, run at the next compile's start: every binding the
  // last compile restored or stored, with its attributes and type as its
  // parse left them.
  std::vector<std::pair<Decl *, AttrVec>> Attrs;
  std::vector<std::pair<VarDecl *, QualType>> Types;
  // First declarations the last compile could have marked used: the
  // bindings it parsed or restored. The next compile starts with them clear.
  std::vector<Decl *> UsedToClear;
  // Diagnostic states, numbered: a compile's warning options, and each
  // #pragma diagnostic line on top of the state before it.
  llvm::StringMap<unsigned> DiagStates;
  unsigned diagState(StringRef Key) {
    return DiagStates.try_emplace(Key, DiagStates.size()).first->second;
  }
};

// What lives for a configuration (2.2). The members are destroyed in reverse
// order, so the ASTContext goes before the tables it refers to.
struct Config {
  unsigned Id = 0;
  std::string Cwd;
  LangOptions LangOpts;
  TargetOptions TargetOpts;
  DiagnosticOptions DiagOpts;
  IntrusiveRefCntPtr<DiagnosticsEngine> Diags;
  IntrusiveRefCntPtr<FileManager> FileMgr;
  IntrusiveRefCntPtr<SourceManager> SourceMgr;
  IntrusiveRefCntPtr<TargetInfo> Target;
  IdentifierTable Idents;
  SelectorTable Selectors;
  Builtin::Context Builtins;
  PersistentLookup Lookup{Idents};
  IntrusiveRefCntPtr<ASTContext> Ctx;
  Store Fragments;
  unsigned Compiles = 0;
  // The build whose files were last checked (2.10).
  std::string Build;
};

// A parsed invocation, kept per working directory and arguments less the
// input, the output and the main file's name (2.1).
struct CachedInvocation {
  std::shared_ptr<CompilerInvocation> Inv;
  // Its configuration's key (configKey); the space is chosen per main file.
  std::string ConfigKey;
};

// Every name a declaration's parse resolved to a declaration (Lookup):
// references, types written by name, and the earlier declaration a
// redeclaration merges with; Bothner records only these (p.27). Also, with
// Lookup false, what a meaning depends on without a lookup: the tag a typedef
// names, the struct a member access or a designator reaches into. As
// BothnerTrace's record mode.
class UseVisitor : public RecursiveASTVisitor<UseVisitor> {
public:
  std::function<void(SourceLocation, const Decl *, bool Lookup)> Ref;
  std::function<void(const TagDecl *)> Tag;
  // A reference that Sema counted as an odr-use, which marks the entity
  // used; and any reference, which marks it referenced.
  std::function<void(SourceLocation, Decl *)> OdrUse, Referenced;
  bool VisitDeclRefExpr(DeclRefExpr *E) {
    Ref(E->getLocation(), E->getDecl(), true);
    if (OdrUse && E->isNonOdrUse() == NOUR_None)
      OdrUse(E->getLocation(), E->getDecl());
    if (Referenced)
      Referenced(E->getLocation(), E->getDecl());
    return true;
  }
  bool VisitTypedefTypeLoc(TypedefTypeLoc TL) {
    Ref(TL.getBeginLoc(), TL.getDecl(), true);
    const Type *T =
        TL.getDecl()->getUnderlyingType().getCanonicalType().getTypePtr();
    while (T->isAnyPointerType() || T->isArrayType())
      T = T->getPointeeOrArrayElementType()
              ->getCanonicalTypeInternal()
              .getTypePtr();
    if (const TagDecl *TD = T->getAsTagDecl())
      Ref(TL.getBeginLoc(), TD, false);
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
  std::function<void(Decl *)> Local;
  // A declaration whose parse put its section into the ASTContext's table
  // (Sema::UnifySection): a function with the attribute written on it, or
  // a definition with static storage.
  std::function<void(NamedDecl *)> Section;
  bool VisitDecl(Decl *D) {
    if (Local && D->isLocalExternDecl())
      Local(D);
    if (Section)
      if (auto *A = D->getAttr<SectionAttr>()) {
        auto *VD = dyn_cast<VarDecl>(D);
        if ((isa<FunctionDecl>(D) && !A->isInherited()) ||
            (VD && VD->hasGlobalStorage() &&
             VD->isThisDeclarationADefinition()))
          Section(cast<NamedDecl>(D));
      }
    if (Decl *P = D->getPreviousDecl())
      Ref(D->getLocation(), P, true);
    if (auto *A = D->getAttr<CleanupAttr>())
      if (FunctionDecl *F = A->getFunctionDecl()) {
        Ref(A->getLocation(), F, true);
        if (OdrUse)
          OdrUse(A->getLocation(), F);
        if (Referenced)
          Referenced(A->getLocation(), F);
      }
    // An alias or ifunc marks its target used by name (handleAliasAttr):
    // not recorded, so the fragment is not reused.
    if (OdrUse && (D->hasAttr<AliasAttr>() || D->hasAttr<IFuncAttr>()))
      OdrUse(D->getLocation(), nullptr);
    return true;
  }
  bool VisitConvertVectorExpr(ConvertVectorExpr *E) {
    if (TypeSourceInfo *T = E->getTypeSourceInfo())
      TraverseTypeLoc(T->getTypeLoc());
    return true;
  }
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

// In C a tag or an enumerator inside a struct is at file scope too.
bool fileScope(const Decl *D) {
  const DeclContext *DC = D->getDeclContext();
  while (DC && isa<TagDecl>(DC))
    DC = DC->getParent();
  return DC && DC->isTranslationUnit();
}

// A fragment instance of one compile.
struct Frag {
  FileID FID;
  unsigned Start = 0, End = ~0u;
  unsigned Key = MainKey, Version = 0;
  bool Main = false, Reused = false;
  std::unique_ptr<struct Version> Rec; // a parse being recorded
  size_t SeqPos = 0;                   // the per-name sequences at its start
  Decl *TUBefore = nullptr;            // the translation unit's last declaration
  // The struct and enum bodies open at its start, innermost first, with the
  // last declaration of each: a member declared inside one (a tag first
  // named there, an enumerator) is bound at file scope too.
  std::vector<std::pair<TagDecl *, Decl *>> OpenTags;
  // Groups (2.7): conditionals opened inside it and not yet closed.
  int Depth = 0;
  unsigned TokensAtStart = 0;
};

// The fragment state of one compile: recording (3), the rule (2.6) and the
// restore (2.5).
class Compile {
public:
  Store &St;
  ASTContext &Ctx;
  SourceManager &SM;
  Preprocessor &PP;
  Sema *S = nullptr;
  CodeGenerator *CodeGen = nullptr;
  Module *RestoreModule = nullptr;
  bool Restoring = false;
  // Option 3 (2.7): #define and #undef lines join the fragment. Groups
  // (2.7, §11.2): conditionals join it while the parser is nested, and until
  // the conditionals the fragment opened are closed. InDefine is set from a
  // joined directive's start to its end.
  bool Option3 = false, Groups = false;
  // Decision 8(b), keys=reading: the n-th reading of a location in this
  // compile is a key of its own, as the model's occurrence keys (R2), so
  // that bits/mathcalls.h read for double, float and long double keeps three
  // versions instead of parsing the second and third readings as repeats.
  bool PerReading = false;
  llvm::DenseMap<std::pair<const FileEntry *, unsigned>, unsigned> Readings;
  bool InDefine = false;
  unsigned DefineAt = 0;
  bool DefineInArgs = false;

  std::vector<Frag> Frags;
  llvm::DenseMap<FileID, std::vector<unsigned>> FragsOf;
  // Each key this compile processed: the version, and the instance.
  llvm::DenseMap<unsigned, std::pair<unsigned, unsigned>> Processed;
  // Per name, its declarations so far, with their sequence positions.
  llvm::DenseMap<Name, std::vector<std::pair<Seen, size_t>>> Declared;
  size_t SeqPos = 0;
  int Open = -1;
  std::deque<Version *> Queue;
  std::vector<Version *> Restored;
  std::vector<Decl *> RestoredTop;
  // The diagnostic state (2.6), with #pragma diagnostic push and pop.
  std::vector<unsigned> DiagStack;
  // The declarations Sema listed for the unused warnings during the parse.
  // The end of the compile drops those the compile used, so the list is
  // read as it grows: a restore must list what its parse listed.
  llvm::DenseSet<const Decl *> ListedUnused;
  size_t UnusedSeen = 0;
  void readUnused() {
    auto B = S->UnusedFileScopedDecls.begin(nullptr, true),
         E = S->UnusedFileScopedDecls.end();
    size_t N = E - B;
    for (; UnusedSeen < N; ++UnusedSeen)
      ListedUnused.insert(*(B + UnusedSeen));
  }
  // A restored fragment's last top-level declaration, until the parser's
  // next token: a parse hands it to the code generator only after reading
  // past the directive that ends the fragment, so the directive's
  // diagnostics come before the code generator's (-Wpadded).
  Decl *Held = nullptr;
  void releaseHeld() {
    if (Decl *D = Held) {
      Held = nullptr;
      CodeGen->HandleTopLevelDecl(DeclGroupRef(D));
    }
  }
  // The last token the parser got, for the fixed observer (2.9), and a
  // parser pragma handed out but maybe not yet acted on (2.4).
  bool HaveLast = false;
  tok::TokenKind LastKind = tok::unknown;
  bool Pending = false;
  llvm::DenseMap<const MacroInfo *, uint64_t> MacroHash;
  uint64_t Unreadable = 0;
  llvm::SmallPtrSet<const IdentifierInfo *, 8> NeverIds, LineIds;
  const IdentifierInfo *LineII = nullptr;
  unsigned Counts[W_N] = {};
  unsigned NsRestored = 0;
  // The files whose fragments were reused, for the tests.
  llvm::StringMap<unsigned> ReusedFiles;
  // Parsed declarations as the parser handed them over, before later code
  // could change them in place; and block-scope externs, by fragment.
  llvm::DenseMap<const Decl *, std::pair<AttrVec, QualType>> Snap;
  std::vector<std::pair<int, NamedDecl *>> LocalExterns;

  Compile(Store &St, ASTContext &Ctx, Preprocessor &PP)
      : St(St), Ctx(Ctx), SM(Ctx.getSourceManager()), PP(PP) {
    // Builtins whose value or effect the macro list cannot see (2.4).
    for (StringRef N : {"_Pragma", "__COUNTER__", "__DATE__", "__TIME__",
                        "__TIMESTAMP__", "__BASE_FILE__", "__INCLUDE_LEVEL__"})
      NeverIds.insert(PP.getIdentifierInfo(N));
    // In a group's conditionals: their answer depends on files, which the
    // per-build file check does not cover for a file that appears.
    for (StringRef N : {"__has_include", "__has_include_next", "__has_embed"})
      NeverIds.insert(PP.getIdentifierInfo(N));
    for (StringRef N : {"__FILE__", "__FILE_NAME__", "__LINE__"})
      LineIds.insert(PP.getIdentifierInfo(N));
    LineII = PP.getIdentifierInfo("__LINE__");
  }

  LinePlace placeOf(FileID FID, unsigned Offset) {
    const SrcMgr::FileInfo &FI = SM.getSLocEntry(FID).getFile();
    const LineEntry *E =
        FI.hasLineDirectives()
            ? SM.getLineTable().FindNearestLineEntry(FID, Offset)
            : nullptr;
    LinePlace P;
    if (!E) {
      P.System = SrcMgr::isSystem(FI.getFileCharacteristic());
      return P;
    }
    // As SourceManager::getPresumedLoc counts.
    P.System = SrcMgr::isSystem(E->FileKind);
    P.FilenameID = E->FilenameID;
    P.Delta = int(E->LineNo) - int(SM.getLineNumber(FID, E->FileOffset)) - 1;
    P.IncludeOffset = E->IncludeOffset;
    return P;
  }

  // The presumed name __FILE__ spells at a location: the path the compile
  // first entered the file by (EntryNames), or a #line name.
  static uint64_t nameHash(const PresumedLoc &P) {
    return P.isValid() ? fnv(P.getFilename()) | 1 : 1;
  }

  unsigned keyOf(FileID FID, unsigned Offset) {
    const FileEntry *FE = SM.getFileEntryForID(FID);
    uint64_t Reading = PerReading ? Readings[{FE, Offset}]++ : 0;
    auto [It, New] =
        St.KeyOf.try_emplace({FE, Reading << 32 | Offset}, St.Latest.size());
    if (New)
      St.Latest.emplace_back();
    return It->second;
  }

  Decl *lastDecl() {
    DeclContext *TU = Ctx.getTranslationUnitDecl();
    return TU->*get(LastDeclTag());
  }

  bool inPrototype() {
    for (Scope *Sc = S ? S->getCurScope() : nullptr; Sc; Sc = Sc->getParent())
      if (Sc->isFunctionPrototypeScope())
        return true;
    return false;
  }

  // The fixed observer (2.9), as the S0 records had it: the parser's
  // between-declarations answer, and the last token it got was a ';' that
  // ended a file-scope declaration, a function body's '}', or an annotation;
  // or it got none yet.
  bool atBoundary() {
    if (DeclBoundaryObserver *O = PP.getDeclBoundaryObserver())
      if (!O->isAtDeclarationBoundary())
        return false;
    if (!HaveLast || tok::isAnnotation(LastKind) || !S)
      return true;
    if (LastKind == tok::semi)
      return S->CurContext && S->CurContext->isTranslationUnit() &&
             !inPrototype();
    if (LastKind == tok::r_brace)
      return S->CurContext && isa<FunctionDecl>(S->CurContext);
    return false;
  }

  // Every pragma state 2.6 compares, as BothnerTrace's fullPragmaState.
  uint64_t pragmaState() {
    if (!S)
      return 0;
    uint64_t H = mix(mix(FNVBasis, Sema::AlignPackInfo::getRawEncoding(
                                       S->AlignPackStack.CurrentValue)),
                     S->CurFPFeatures.getAsOpaqueInt());
    H = mix(H, S->FpPragmaStack.CurrentValue.getAsOpaqueInt());
    H = mix(H, S->MSStructPragmaOn);
    if (auto *V = static_cast<std::vector<std::pair<unsigned, SourceLocation>> *>(
            S->VisContext)) {
      H = mix(H, V->size() + 1);
      if (!V->empty())
        H = mix(H, V->back().first);
    }
    for (const Sema::PragmaClangSection *P :
         {&S->PragmaClangBSSSection, &S->PragmaClangDataSection,
          &S->PragmaClangRodataSection, &S->PragmaClangRelroSection,
          &S->PragmaClangTextSection})
      H = mix(H, P->Valid ? fnv(P->SectionName) : 0);
    for (const StringLiteral *L :
         {S->DataSegStack.CurrentValue, S->BSSSegStack.CurrentValue,
          S->ConstSegStack.CurrentValue, S->CodeSegStack.CurrentValue})
      H = mix(H, L ? fnv(L->getBytes()) : 0);
    H = mix(H, S->StrictGuardStackCheckStack.CurrentValue);
    H = mix(H, S->OptimizeOffPragmaLocation.isValid());
    H = mix(H, S->PragmaAttributeStack.size());
    H = mix(H, PP.getPragmaAssumeNonNullLoc().isValid());
    return H | 1;
  }
  // A pending #pragma weak or redefine_extname, or an open #pragma clang
  // attribute: a restore could not carry what they would add.
  bool pragmaPending() {
    return S && (!S->WeakUndeclaredIdentifiers.empty() ||
                 !S->ExtnameUndeclaredIdentifiers.empty() ||
                 !S->PragmaAttributeStack.empty());
  }

  // V1's state of a name: not a macro, or a macro with this definition. Two
  // definitions have one state when C's rule for a redefinition finds them
  // identical (MacroInfo::isIdenticalTo, lexically): the same kind,
  // parameters in order, variadic form, and tokens with the same spelling and
  // the same spaces and line starts between them. The body's tokens alone
  // are not enough: F(x, y) x and F(y, x) x differ, and so do a+b and a + b
  // once a stringification spells them.
  uint64_t macroState(const IdentifierInfo *II) {
    uint64_t H = defState(PP.getMacroDefinition(II).getMacroInfo());
    // What the compile's pragmas set on the name: #pragma GCC poison makes
    // a use an error, #pragma clang deprecated, restrict_expansion and final
    // make an expansion or a redefinition warn (2.3 clears them each
    // compile).
    unsigned Flags = II->isPoisoned() | II->isDeprecatedMacro() << 1 |
                     II->isRestrictExpansion() << 2 | II->isFinal() << 3;
    return Flags ? mix(H, 0x100 | Flags) | 1 : H;
  }
  uint64_t defState(const MacroInfo *MI) {
    if (!MI)
      return 0;
    auto [It, New] = MacroHash.try_emplace(MI, 0);
    if (New) {
      uint64_t H = mix(FNVBasis, MI->isFunctionLike() | MI->isC99Varargs() << 1 |
                                     MI->isGNUVarargs() << 2 |
                                     MI->isBuiltinMacro() << 3);
      H = mix(H, MI->getNumParams());
      for (const IdentifierInfo *P : MI->params())
        H = fnv(P->getName(), mix(H, 0x50));
      H = mix(H, MI->getNumTokens());
      bool First = true, Invalid = false;
      for (const Token &T : MI->tokens()) {
        H = mix(H, T.getKind());
        if (!First)
          H = mix(H, T.isAtStartOfLine() << 1 | T.hasLeadingSpace());
        First = false;
        if (const IdentifierInfo *TI = T.getIdentifierInfo())
          H = fnv(TI->getName(), H);
        else
          H = fnv(PP.getSpelling(T, &Invalid), H);
        if (Invalid)
          break;
      }
      // A definition that cannot be read never matches.
      if (Invalid)
        H = mix(fnv("\x01unreadable"),
                ++Unreadable + (uint64_t(St.NextVersion) << 32));
      It->second = H | 1;
    }
    return It->second;
  }

  bool processedAt(std::pair<unsigned, unsigned> U) {
    auto It = Processed.find(U.first);
    return It != Processed.end() && It->second.first == U.second;
  }

  bool sameList(Name N, const std::vector<Seen> &Stored) {
    auto It = Declared.find(N);
    size_t Have = It == Declared.end() ? 0 : It->second.size();
    if (Have != Stored.size())
      return false;
    for (size_t I = 0; I < Have; ++I)
      if (!(It->second[I].first == Stored[I]))
        return false;
    return true;
  }

  // Option 3: whether a name's earlier definition, now not the one the
  // fragment's parse found, changes what its first line about the name does.
  // A #define line compares the two only to report a redefinition
  // (-Wmacro-redefined), which no earlier definition and an identical one
  // (C's rule, as defState compares) do not cause; an #undef line does not
  // compare them. The parse reported nothing: a fragment whose parse printed
  // a diagnostic is never reused. What else the earlier definition changes
  // (-Wunused-macros, a final or predefined name) the effects check in
  // decide() compares, and a name a pragma marks must have its state.
  bool samePrior(const Version &V, const IdentifierInfo *II) {
    if (II->isPoisoned() || II->isDeprecatedMacro() ||
        II->isRestrictExpansion() || II->isFinal())
      return false;
    for (const MacroEffect &E : V.Effects) {
      if (E.II != II)
        continue;
      if (!E.Define)
        return true;
      const MacroInfo *Cur = PP.getMacroInfo(II);
      return !Cur || (!Cur->isAllowRedefinitionsWithoutWarning() &&
                      defState(Cur) == E.State);
    }
    return false;
  }

  // R0 with the checks of 2.6, in simulate.py's order; then the file checks
  // and decision 9's token names.
  Why decide(FileID FID, unsigned Offset, unsigned Key, Version *V,
             bool CanSkip) {
    if (V && V->Empty && CanSkip)
      return W_Empty;
    if (!CanSkip)
      return W_NoSkip;
    if (Pending)
      return W_Pending;
    if (!atBoundary())
      return W_Nested;
    if (Processed.count(Key))
      return W_Repeat;
    if (!V)
      return W_New;
    if (V->Never != W_Reused)
      return V->Never;
    // Sema treats a system header differently, the stored declarations keep
    // their lines, and __FILE__ became a string in them.
    if (V->Place != placeOf(FID, Offset) ||
        (V->FileName &&
         V->FileName !=
             nameHash(SM.getPresumedLoc(SM.getComposedLoc(FID, Offset)))))
      return W_File;
    if (V->DiagState != DiagStack.back())
      return W_Diag;
    for (const auto &U : V->Uses)
      if (!processedAt(U))
        return W_Uses;
    for (const auto &[II, H] : V->Macros)
      if (macroState(II) != H)
        return W_Macro;
    for (const auto &[II, H] : V->Priors)
      if (macroState(II) != H && !samePrior(*V, II))
        return W_Macro;
    // What the earlier definition of a name its lines make or remove changes
    // there besides: an unused macro of the main file is reported or
    // forgotten (-Wunused-macros), a final or predefined one is reported.
    for (const MacroEffect &E : V->Effects) {
      if (E.II->isFinal())
        return W_Macro;
      if (const MacroInfo *Cur = PP.getMacroInfo(E.II))
        if (Cur->isWarnIfUnused() ||
            SM.isWrittenInBuiltinFile(Cur->getDefinitionLoc()))
          return W_Macro;
    }
    if (pragmaState() != V->Pragma || pragmaPending())
      return W_Pragma;
    bool Neg = false, Dep = false, Pred = false;
    for (const auto &[N, Stored] : V->Lists)
      if (!sameList(N, Stored)) {
        if (!V->BindNames.count(N))
          Dep = true;
        else if (!Stored.empty())
          Pred = true;
        else
          Neg = true;
      }
    for (const auto &D : V->Deps)
      if (!processedAt(D))
        Dep = true;
    if (Neg || Dep || Pred)
      return Neg ? W_Negative : Dep ? W_Deps : W_Predecessor;
    for (Name N : V->TokenAbsent)
      if (Declared.count(N))
        return W_Tokens;
    for (const auto &[N, Stored] : V->TokenLists)
      if (!sameList(N, Stored))
        return W_Tokens;
    return W_Reused;
  }

  void push(Frag F) {
    unsigned I = Frags.size();
    FragsOf[F.FID].push_back(I);
    Frags.push_back(std::move(F));
    Open = I;
  }

  bool start(FileID FID, unsigned Offset, bool CanSkip,
             HeaderCacheHook::FragmentSkip &Skip) {
    if (InDefine) {
      InDefine = false;
      if (Open >= 0 && Frags[Open].FID == FID)
        return false;
      // Not expected after a #define line: end the fragment there.
      if (Open >= 0)
        close(Frags[Open].FID, DefineAt, DefineInArgs);
    }
    Open = -1;
    Frag F;
    F.FID = FID;
    F.Start = Offset;
    F.SeqPos = SeqPos;
    F.TUBefore = lastDecl();
    for (DeclContext *DC = S ? S->CurContext : nullptr; DC && isa<TagDecl>(DC);
         DC = DC->getLexicalParent())
      F.OpenTags.push_back({cast<TagDecl>(DC), DC->*get(LastDeclTag())});
    if (FID == SM.getMainFileID()) {
      F.Main = true;
      push(std::move(F));
      return false;
    }
    F.Key = keyOf(FID, Offset);
    Version *V = St.Latest[F.Key].get();
    Why W = decide(FID, Offset, F.Key, V, CanSkip);
    ++Counts[W];
    // BOTHNER_WHY=<reason>: where the server refuses for that reason, on
    // standard error, for looking into a count.
    static const char *Watch = getenv("BOTHNER_WHY");
    if (Watch && WhyName[W] == StringRef(Watch)) {
      PresumedLoc P = SM.getPresumedLoc(SM.getComposedLoc(FID, Offset));
      auto Show = [](const LinePlace &L) {
        return (L.System ? "system " : "user ") + std::to_string(L.FilenameID) +
               ' ' + std::to_string(L.Delta) + ' ' +
               std::to_string(L.IncludeOffset);
      };
      llvm::errs() << "bothner-why " << Watch << ' '
                   << (P.isValid() ? P.getFilename() : "?") << ':'
                   << (P.isValid() ? P.getLine() : 0) << " place "
                   << Show(placeOf(FID, Offset)) << " / "
                   << (V ? Show(V->Place) : std::string("none")) << '\n';
    }
    if (W == W_Empty || W == W_Reused) {
      F.Reused = true;
      F.Version = V->Id;
      Processed[F.Key] = {V->Id, unsigned(Frags.size())};
      Skip.End = V->End;
      Skip.HadTokens = V->HadTokens;
      Skip.ExpandedMacro = V->ExpandedMacro;
      // Its tokens count as read; the queued token of a restore counts as
      // one when the parser takes it.
      PP.*get(TokenCountTag()) += V->NumTokens - (W == W_Reused ? 1u : 0u);
      // Before the next directive is read: it may test what they define.
      replay(*V);
      if (W == W_Reused) {
        for (NamedDecl *B : V->Binds)
          Declared[nameOf(B)].push_back({{F.Key, V->Id}, SeqPos++});
        // The macros its parse expanded count as used (-Wunused-macros), as
        // the expansion would have marked them (2.5).
        for (const auto &[II, H] : V->Macros)
          if (H)
            if (MacroInfo *MI = PP.getMacroInfo(II))
              MI->setIsUsed(true);
        Queue.push_back(V);
        Skip.Restore = RestoreModule;
        if (OptionalFileEntryRef FE = SM.getFileEntryRefForID(FID))
          ++ReusedFiles[llvm::sys::path::filename(FE->getName())];
      }
      push(std::move(F));
      return true;
    }
    F.TokensAtStart = PP.getTokenCount();
    F.Rec = std::make_unique<Version>();
    F.Rec->Id = St.NextVersion++;
    F.Rec->Key = F.Key;
    F.Rec->Pragma = pragmaState();
    F.Rec->Place = placeOf(FID, Offset);
    F.Rec->DiagState = DiagStack.back();
    // A diagnostic that points into a restored declaration later (a struct
    // laid out at its first use, the end of the compile) has the state of
    // the warning options: the restore's locations lie in the parse's
    // files, which no #pragma diagnostic of this compile reaches. So the
    // parse must have had that state too.
    if (DiagStack.back() != DiagStack.front() && F.Rec->Never == W_Reused)
      F.Rec->Never = W_Diag;
    // A parse that started where a skip was not possible: its start state is
    // not one a reuse could match. Nor is a parse while a #pragma weak or
    // redefine_extname waits for a name, or #pragma clang attribute is open:
    // what they add to its declarations is no part of the pragma state.
    if (W == W_NoSkip || W == W_Pending || W == W_Nested)
      F.Rec->Never = W;
    else if (pragmaPending())
      F.Rec->Never = W_Pragma;
    F.Version = F.Rec->Id;
    Processed[F.Key] = {F.Version, unsigned(Frags.size())};
    push(std::move(F));
    return false;
  }

  void bind(Frag &F, NamedDecl *ND) {
    Seen W = F.Main ? Seen{MainKey, St.NextMain++} : Seen{F.Key, F.Version};
    St.Owner[ND] = W;
    Declared[nameOf(ND)].push_back({W, SeqPos++});
    if (F.Rec) {
      F.Rec->Binds.push_back(ND);
      F.Rec->BindNames.insert(nameOf(ND));
    }
  }

  bool inTUScope(const NamedDecl *ND) {
    return S && S->TUScope && ND->getIdentifier() &&
           S->TUScope->isDeclScope(ND);
  }

  // The members of a tag from \p M on that C binds at file scope.
  void members(Frag &F, Decl *M) {
    for (; M; M = M->getNextDeclInContext()) {
      if (auto *EC = dyn_cast<EnumConstantDecl>(M)) {
        if (inTUScope(EC))
          bind(F, EC);
      } else if (auto *T = dyn_cast<TagDecl>(M)) {
        if (inTUScope(T))
          bind(F, T);
        nested(F, T);
      }
    }
  }
  void nested(Frag &F, TagDecl *Tag) { members(F, *Tag->decls_begin()); }

  // What the translation unit gained during the fragment, bound at file
  // scope at its end (2.5's bindings).
  void bindings(Frag &F) {
    TranslationUnitDecl *TU = Ctx.getTranslationUnitDecl();
    Decl *D = F.TUBefore ? F.TUBefore->getNextDeclInContext()
                         : *TU->decls_begin();
    for (; D; D = D->getNextDeclInContext()) {
      if (D->isImplicit())
        continue;
      if (F.Rec)
        F.Rec->TopLevel.push_back(D);
      if (auto *ND = dyn_cast<NamedDecl>(D); ND && inTUScope(ND))
        bind(F, ND);
      if (auto *Tag = dyn_cast<TagDecl>(D))
        nested(F, Tag);
    }
    // Members added to a body that an earlier fragment opened.
    for (const auto &[Tag, Last] : F.OpenTags)
      members(F, Last ? Last->getNextDeclInContext() : *Tag->decls_begin());
  }

  // The directive at Offset, read from the text: its name. Anything unusual
  // before the name (a comment, a line splice) gives none, and the directive
  // ends the fragment as in option 2.
  StringRef directiveName(FileID FID, unsigned Offset) {
    bool Invalid = false;
    StringRef Text = SM.getBufferData(FID, &Invalid);
    if (Invalid || Offset >= Text.size())
      return {};
    StringRef T = Text.drop_front(Offset);
    if (!T.consume_front("#") && !T.consume_front("%:"))
      return {};
    T = T.ltrim(" \t\f\v");
    return T.take_while([](char C) { return isAsciiIdentifierContinue(C); });
  }

  // Whether the directive joins the open fragment (option 3, groups).
  bool joins(StringRef Name) {
    const Frag &F = Frags[Open];
    if (Name == "define" || Name == "undef")
      return Option3;
    if (!Groups)
      return false;
    if (Name == "if" || Name == "ifdef" || Name == "ifndef")
      return F.Depth > 0 || !atBoundary();
    if (Name == "else" || Name == "elif" || Name == "elifdef" ||
        Name == "elifndef" || Name == "endif")
      return F.Depth > 0;
    return false;
  }

  void end(FileID FID, unsigned Offset, bool InMacroArgs) {
    InDefine = false;
    if (Open < 0)
      return;
    if ((Option3 || Groups) && !InMacroArgs && Frags[Open].FID == FID &&
        joins(directiveName(FID, Offset))) {
      InDefine = true;
      DefineAt = Offset;
      DefineInArgs = InMacroArgs;
      return;
    }
    close(FID, Offset, InMacroArgs);
  }

  // Groups: a joined conditional opens or closes a level; the names it tests
  // are part of the macro list, since a reuse takes its branches unseen.
  void condOpened() {
    if (InDefine && Open >= 0)
      ++Frags[Open].Depth;
  }
  void condClosed() {
    if (InDefine && Open >= 0 && Frags[Open].Depth > 0)
      --Frags[Open].Depth;
  }
  void condTested(const Token &Name) {
    if (!InDefine || Open < 0)
      return;
    Version *R = Frags[Open].Rec.get();
    const IdentifierInfo *II = Name.getIdentifierInfo();
    if (!R || !II || R->DefinedHere.count(II) || !R->MacroSeen.insert(II).second)
      return;
    R->Macros.push_back({II, macroState(II)});
  }

  void close(FileID FID, unsigned Offset, bool InMacroArgs) {
    if (Open < 0)
      return;
    Frag &F = Frags[Open];
    Open = -1;
    if (F.FID != FID)
      return;
    F.End = Offset;
    if (F.Reused)
      return;
    bindings(F);
    if (Version *R = F.Rec.get()) {
      R->End = Offset;
      R->NumTokens = PP.getTokenCount() - F.TokensAtStart;
      if (InMacroArgs && R->Never == W_Reused)
        R->Never = W_Args;
      // A group that ends inside a conditional it opened: a reuse would skip
      // the conditional's start but not its end.
      if (F.Depth != 0 && R->Never == W_Reused)
        R->Never = W_NestedEnd;
      if (!atBoundary() && R->Never == W_Reused)
        R->Never = W_NestedEnd;
      R->Empty = !R->HadTokens && R->Macros.empty() && R->Binds.empty() &&
                 R->TopLevel.empty() && R->Effects.empty() &&
                 R->Never == W_Reused;
    }
  }

  void token(const Token &T) {
    releaseHeld();
    tok::TokenKind K = T.getKind();
    bool Annot = tok::isAnnotation(K);
    // A pragma's token stays pending until the token request after the one
    // that handed it out has returned: some handlers consume it first.
    Pending = Annot && K != tok::annot_module_include &&
              K != tok::annot_module_begin && K != tok::annot_module_end &&
              K != tok::annot_repl_input_end;
    HaveLast = true;
    LastKind = K;
    if (Open >= 0 && !Annot)
      if (Version *R = Frags[Open].Rec.get()) {
        R->HadTokens = true;
        // A source location the AST can keep as a value: a file-scope
        // initializer is evaluated once, in the parse that stored it.
        if (R->Never == W_Reused &&
            (K == tok::kw___builtin_FILE || K == tok::kw___builtin_FILE_NAME ||
             K == tok::kw___builtin_LINE || K == tok::kw___builtin_COLUMN))
          R->Never = W_Builtin;
      }
  }

  void candidate(const Token &T) {
    if (Open < 0)
      return;
    Version *R = Frags[Open].Rec.get();
    const IdentifierInfo *II = T.getIdentifierInfo();
    if (!R || !II)
      return;
    if (NeverIds.count(II) && R->Never == W_Reused)
      R->Never = W_Builtin;
    if (LineIds.count(II) && R->Never == W_Reused) {
      // The line is fixed per key, unless a #line moved this file's lines.
      // The name is the path the compile entered the file by, which a reuse
      // must match (decide).
      SourceLocation L = SM.getExpansionLoc(T.getLocation());
      PresumedLoc P = SM.getPresumedLoc(L);
      if (P.isInvalid() || P.getLine() != SM.getExpansionLineNumber(L) ||
          P.getFileID() != SM.getFileID(L))
        R->Never = W_Builtin;
      else if (II != LineII)
        R->FileName = nameHash(P);
    }
    if (R->MacroSeen.insert(II).second) {
      // A name the fragment defined or removed before this use means what
      // the fragment made it mean.
      if (!R->DefinedHere.count(II)) {
        uint64_t H = macroState(II);
        R->Macros.push_back({II, H});
        if (H)
          R->ExpandedMacro = true;
      } else if (PP.getMacroDefinition(II)) {
        R->ExpandedMacro = true;
      }
      if (II->getTokenID() == tok::identifier)
        R->Tokens.push_back(II);
    }
  }

  // Option 3: a #define or #undef line joined to the open fragment.
  void macroLine(const Token &Name, const MacroInfo *MI, SourceLocation Loc,
                 const MacroInfo *Prior) {
    if (!InDefine || Open < 0)
      return;
    Version *R = Frags[Open].Rec.get();
    if (!R)
      return;
    MacroEffect E;
    E.II = Name.getIdentifierInfo();
    E.Loc = Loc;
    // A definition #pragma push_macro let be redefined without a warning
    // never matches.
    if (!R->DefinedHere.count(E.II) && R->MacroSeen.insert(E.II).second)
      R->Priors.push_back(
          {E.II, defState(Prior) ^
                     (Prior && Prior->isAllowRedefinitionsWithoutWarning() ? 2
                                                                           : 0)});
    if (MI) {
      E.Define = true;
      E.DefLoc = MI->getDefinitionLoc();
      E.EndLoc = MI->getDefinitionEndLoc();
      E.FunctionLike = MI->isFunctionLike();
      E.C99Varargs = MI->isC99Varargs();
      E.GNUVarargs = MI->isGNUVarargs();
      E.CommaPasting = MI->hasCommaPasting();
      E.AllowRedefinition = MI->isAllowRedefinitionsWithoutWarning();
      for (const IdentifierInfo *P : MI->params())
        E.Params.push_back(const_cast<IdentifierInfo *>(P));
      E.Tokens.assign(MI->tokens().begin(), MI->tokens().end());
      E.State = defState(MI);
    }
    R->DefinedHere.insert(E.II);
    R->Effects.push_back(std::move(E));
  }

  // A reuse makes the fragment's #define and #undef lines again, in order,
  // as the Preprocessor would have (HandleDefineDirective,
  // HandleUndefDirective).
  void replay(const Version &V) {
    for (const MacroEffect &E : V.Effects) {
      if (!E.Define) {
        if (PP.getMacroInfo(E.II))
          PP.appendMacroDirective(
              E.II, new (PP.getPreprocessorAllocator()) UndefMacroDirective(E.Loc));
        continue;
      }
      MacroInfo *MI = PP.AllocateMacroInfo(E.DefLoc);
      if (E.FunctionLike)
        MI->setIsFunctionLike();
      if (E.C99Varargs)
        MI->setIsC99Varargs();
      if (E.GNUVarargs)
        MI->setIsGNUVarargs();
      if (E.CommaPasting)
        MI->setHasCommaPasting();
      MI->setIsAllowRedefinitionsWithoutWarning(E.AllowRedefinition);
      MI->setParameterList(E.Params, PP.getPreprocessorAllocator());
      MI->setTokens(E.Tokens, PP.getPreprocessorAllocator());
      MI->setDefinitionEndLoc(E.EndLoc);
      PP.appendDefMacroDirective(E.II, MI, E.Loc);
    }
  }

  // The fragment instance a location lies in, or -1.
  int fragAt(SourceLocation L) {
    if (L.isInvalid())
      return -1;
    L = SM.getExpansionLoc(L);
    auto [FID, Off] = SM.getDecomposedLoc(L);
    auto It = FragsOf.find(FID);
    if (It == FragsOf.end())
      return -1;
    const std::vector<unsigned> &V = It->second;
    auto P = std::upper_bound(V.begin(), V.end(), Off, [&](unsigned O, unsigned I) {
      return O < Frags[I].Start;
    });
    if (P == V.begin())
      return -1;
    unsigned I = *(P - 1);
    if (Off >= Frags[I].End)
      return -1;
    return I;
  }

  // What decides which diagnostics a parse prints: the compile's warning
  // options, then the #pragma diagnostic lines it has seen.
  void diagOptions(const DiagnosticOptions &O) {
    std::string K = "options";
    for (const std::string &W : O.Warnings)
      K += "\x1fW" + W;
    for (const std::string &R : O.Remarks)
      K += "\x1fR" + R;
    K += O.IgnoreWarnings ? "\x1fw" : "";
    K += O.Pedantic ? "\x1fpedantic" : "";
    K += O.PedanticErrors ? "\x1fpedantic-errors" : "";
    DiagStack.assign(1, St.diagState(K));
  }
  void diagPragma(StringRef Line) {
    DiagStack.back() =
        St.diagState((Twine(DiagStack.back()) + "\x1f" + Line).str());
  }
  void diagPush() { DiagStack.push_back(DiagStack.back()); }
  void diagPop() {
    if (DiagStack.size() > 1)
      DiagStack.pop_back();
  }
  // A diagnostic printed: the fragments that printed it are never reused,
  // since a reuse prints nothing. It belongs to the open fragment, and to
  // the fragment it points into: the parser may finish a fragment's last
  // declaration after the next directive. A note goes with the diagnostic
  // before it.
  void diagnosed(DiagnosticsEngine::Level L, SourceLocation Loc) {
    if (L == DiagnosticsEngine::Note)
      return;
    auto Mark = [](Frag &F) {
      if (F.Rec && F.Rec->Never == W_Reused)
        F.Rec->Never = W_Diag;
    };
    if (Open >= 0)
      Mark(Frags[Open]);
    if (int I = fragAt(Loc); I >= 0)
      Mark(Frags[I]);
  }

  // Who made a declaration, and the instance where this compile processed
  // it (-1: not at that version, or not at all).
  std::pair<Seen, int> owner(const Decl *D) {
    auto It = St.Owner.find(D);
    if (It != St.Owner.end() && It->second.Key != MainKey) {
      auto P = Processed.find(It->second.Key);
      if (P != Processed.end() && P->second.first == It->second.Version)
        return {It->second, int(P->second.second)};
      return {It->second, -1};
    }
    int I = fragAt(D->getLocation());
    if (I < 0)
      return {{}, -1};
    const Frag &F = Frags[I];
    return {F.Main ? Seen{MainKey, 0} : Seen{F.Key, F.Version}, I};
  }

  // Uses, deps and references of a parsed top-level declaration (3),
  // attributed by location: a group reaches the consumer only after the
  // token that follows it.
  void snapshot(Decl *D) {
    Snap[D] = {D->hasAttrs() ? D->getAttrs() : AttrVec(),
               isa<VarDecl>(D) ? cast<VarDecl>(D)->getType() : QualType()};
    if (auto *Tag = dyn_cast<TagDecl>(D))
      for (Decl *M : Tag->decls())
        if (isa<TagDecl, EnumConstantDecl>(M))
          snapshot(M);
  }

  void uses(Decl *TopDecl) {
    snapshot(TopDecl);
    UseVisitor V;
    V.Ref = [&](SourceLocation At, const Decl *D, bool Lookup) {
      int U = fragAt(At);
      if (U < 0 || !D)
        return;
      Version *R = Frags[U].Rec.get();
      if (!R)
        return;
      if (auto *ND = dyn_cast<NamedDecl>(D);
          ND && ND->getIdentifier() && fileScope(D))
        R->Refs.insert(nameOf(ND));
      // The entity's first declaration that is not implicit, by the order
      // this compile processed them: for a library builtin, the header's.
      std::pair<Seen, int> First = {{}, -1};
      for (const Decl *Q : D->redecls())
        if (!Q->isImplicit())
          if (auto O = owner(Q);
              O.second >= 0 && (First.second < 0 || O.second < First.second))
            First = O;
      if (First.second < 0)
        return;
      if (Lookup && First.second != U && First.second < U) {
        if (Frags[First.second].Main) {
          if (R->Never == W_Reused)
            R->Never = W_Main;
        } else {
          R->Uses.insert({First.first.Key, First.first.Version});
        }
      }
      for (const Decl *Q : D->redecls()) {
        if (Q->isImplicit())
          continue;
        auto O = owner(Q);
        if (O.second < 0 || O.second == U || O.second > U)
          continue;
        if (Frags[O.second].Main) {
          if (R->Never == W_Reused)
            R->Never = W_Main;
        } else {
          R->Deps.insert({O.first.Key, O.first.Version});
        }
      }
      // p.28-29: a tag first declared in a parameter list, which C moves
      // into the function.
      if (auto *TD = dyn_cast<TagDecl>(D))
        if (auto *FD = dyn_cast<FunctionDecl>(TD->getDeclContext()))
          if (!FD->doesThisDeclarationHaveABody() ||
              SM.isBeforeInTranslationUnit(TD->getLocation(),
                                           FD->getBody()->getBeginLoc()))
            if (R->Never == W_Reused)
              R->Never = W_Param;
    };
    // A tag first named at block scope without a definition: the file-scope
    // lookup found nothing, a negative dependency.
    V.Tag = [&](const TagDecl *TD) {
      if (!TD->getIdentifier() || fileScope(TD) ||
          TD->isThisDeclarationADefinition() || TD->getPreviousDecl())
        return;
      int U = fragAt(TD->getLocation());
      if (U >= 0)
        if (Version *R = Frags[U].Rec.get())
          R->Refs.insert(nameOf(TD));
    };
    V.Local = [&](Decl *D) {
      if (auto *ND = dyn_cast<NamedDecl>(D))
        LocalExterns.push_back({fragAt(TopDecl->getLocation()), ND});
    };
    V.Section = [&](NamedDecl *D) {
      int U = fragAt(D->getLocation());
      if (U >= 0)
        if (Version *R = Frags[U].Rec.get())
          R->Sections.push_back({D, 0});
    };
    V.OdrUse = [&](SourceLocation At, Decl *D) {
      int U = fragAt(At);
      if (U < 0)
        return;
      Version *R = Frags[U].Rec.get();
      if (!R)
        return;
      if (!D) {
        if (R->Never == W_Reused)
          R->Never = W_Builtin;
        return;
      }
      Decl *First = D->getCanonicalDecl();
      if (First->isUsed(false))
        R->UsedDecls.insert(First);
    };
    V.Referenced = [&](SourceLocation At, Decl *D) {
      if (!D->getDeclContext()->getRedeclContext()->isFileContext())
        return;
      int U = fragAt(At);
      if (U < 0)
        return;
      if (Version *R = Frags[U].Rec.get())
        R->RefDecls.insert(D);
    };
    V.TraverseDecl(TopDecl);
  }

  // The restore of 2.5, when the parser reaches the queued token at file
  // scope.
  void restoreNext() {
    if (Queue.empty() || !S)
      return;
    Version *V = Queue.front();
    Queue.pop_front();
    Restoring = true;
    for (size_t I = 0; I < V->Binds.size(); ++I) {
      NamedDecl *D = V->Binds[I];
      repairChain(D);
      D->setReferenced(false);
      D->dropAttrs();
      if (!V->BindAttrs[I].empty())
        D->setAttrs(V->BindAttrs[I]);
      St.Attrs.push_back({D, V->BindAttrs[I]});
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        VD->setType(V->BindTypes[I]);
        St.Types.push_back({VD, V->BindTypes[I]});
      }
      S->PushOnScopeChains(D, S->TUScope, /*AddToContext=*/false);
      if (auto *TD = dyn_cast<TypedefNameDecl>(D))
        if (IdentifierInfo *II = TD->getIdentifier())
          switch (II->getNotableIdentifierID()) {
          case tok::NotableIdentifierKind::FILE:
            Ctx.setFILEDecl(TD);
            break;
          case tok::NotableIdentifierKind::jmp_buf:
            Ctx.setjmp_bufDecl(TD);
            break;
          case tok::NotableIdentifierKind::sigjmp_buf:
            Ctx.setsigjmp_bufDecl(TD);
            break;
          case tok::NotableIdentifierKind::ucontext_t:
            Ctx.setucontext_tDecl(TD);
            break;
          default:
            break;
          }
    }
    // A block-scope extern is a redeclaration a later compile's parse can
    // extend, like a binding: its chain is repaired too.
    for (NamedDecl *D : V->LocalExterns) {
      repairChain(D);
      Ctx.getExternCContextDecl()->makeDeclVisibleInContext(D);
    }
    for (const auto &[D, Flags] : V->Sections)
      S->UnifySection(D->getAttr<SectionAttr>()->getName(), Flags, D);
    for (Decl *D : V->UsedDecls)
      D->setIsUsed();
    for (Decl *D : V->RefDecls)
      D->setReferenced();
    Restored.push_back(V);
    for (VarDecl *VD : V->Tentative)
      S->TentativeDefinitions.push_back(VD);
    for (const DeclaratorDecl *D : V->UnusedFileScoped)
      S->UnusedFileScopedDecls.push_back(D);
    for (TypedefNameDecl *D : V->ExtVector)
      S->ExtVectorDecls.push_back(D);
    releaseHeld();
    for (Decl *D : V->TopLevel) {
      RestoredTop.push_back(D);
      if (CodeGen) {
        if (auto *Tag = dyn_cast<TagDecl>(D))
          completed(Tag);
        if (D == V->TopLevel.back())
          Held = D;
        else
          CodeGen->HandleTopLevelDecl(DeclGroupRef(D));
      }
    }
    NsRestored += V->Binds.size();
    Restoring = false;
  }

  // The code generator hears of each tag definition as a parse would tell it
  // (Sema::ActOnTagFinishDefinition), inner tags first, before the
  // declaration that holds them: it completes a type it first saw
  // incomplete, in the IR and in the debug information.
  void completed(TagDecl *Tag) {
    for (Decl *M : Tag->decls())
      if (auto *Inner = dyn_cast<TagDecl>(M))
        completed(Inner);
    if (Tag->isCompleteDefinition())
      CodeGen->HandleTagDeclDefinition(Tag);
  }

  // At the compile's end, without errors: the new versions are completed
  // and stored, each replacing its location's last.
  void finalize(bool Errors) {
    Open = -1;
    // What the next compile clears, errors or not (2.8): the entities of the
    // bindings this compile parsed or restored, and those their code used.
    auto Clear = [&](Version &V) {
      for (NamedDecl *B : V.Binds)
        St.UsedToClear.push_back(B->getCanonicalDecl());
      llvm::append_range(St.UsedToClear, V.UsedDecls);
    };
    for (Version *V : Restored)
      Clear(*V);
    for (Frag &F : Frags)
      if (F.Rec)
        Clear(*F.Rec);
    if (Errors || !S)
      return;
    llvm::DenseSet<const Decl *> Tent, ExtV;
    const llvm::DenseSet<const Decl *> &Unused = ListedUnused;
    for (auto It = S->TentativeDefinitions.begin(nullptr, true),
              E = S->TentativeDefinitions.end();
         It != E; ++It)
      Tent.insert(*It);
    for (auto It = S->ExtVectorDecls.begin(nullptr, true),
              E = S->ExtVectorDecls.end();
         It != E; ++It)
      ExtV.insert(*It);
    for (Frag &F : Frags) {
      Version *R = F.Rec.get();
      if (!R)
        continue;
      auto List = [&](Name N) {
        std::vector<Seen> L;
        if (auto It = Declared.find(N); It != Declared.end())
          for (const auto &[W, Pos] : It->second)
            if (Pos < F.SeqPos)
              L.push_back(W);
        return L;
      };
      for (Name N : R->BindNames)
        R->Lists.push_back({N, List(N)});
      for (Name N : R->Refs)
        if (!R->BindNames.count(N))
          R->Lists.push_back({N, List(N)});
      for (const IdentifierInfo *II : R->Tokens)
        for (Name N : {Name(II), Name(II) | 1})
          if (!R->BindNames.count(N) && !R->Refs.count(N)) {
            std::vector<Seen> L = List(N);
            if (L.empty())
              R->TokenAbsent.push_back(N);
            else
              R->TokenLists.push_back({N, std::move(L)});
          }
      // 2.8: an internal or inline function it declares and does not define
      // itself; its Used bit would leak.
      if (R->Never == W_Reused)
        for (NamedDecl *B : R->Binds)
          if (auto *FD = dyn_cast<FunctionDecl>(B))
            if ((FD->getFormalLinkage() == Linkage::Internal ||
                 FD->isInlineSpecified()) &&
                !llvm::any_of(R->TopLevel, [&](Decl *D) {
                  auto *G = dyn_cast<FunctionDecl>(D);
                  return G && G->getCanonicalDecl() == FD->getCanonicalDecl() &&
                         G->isThisDeclarationADefinition();
                })) {
              R->Never = W_Undefined;
              break;
            }
      for (NamedDecl *B : R->Binds) {
        auto It = Snap.find(B);
        AttrVec A = It != Snap.end() ? It->second.first
                    : B->hasAttrs()  ? B->getAttrs()
                                     : AttrVec();
        R->BindAttrs.push_back(A);
        St.Attrs.push_back({B, A});
        QualType T;
        if (auto *VD = dyn_cast<VarDecl>(B)) {
          T = It != Snap.end() ? It->second.second : VD->getType();
          St.Types.push_back({VD, T});
        }
        R->BindTypes.push_back(T);
        if (Tent.count(B))
          R->Tentative.push_back(cast<VarDecl>(B));
        if (Unused.count(B))
          R->UnusedFileScoped.push_back(cast<DeclaratorDecl>(B));
        if (ExtV.count(B))
          R->ExtVector.push_back(cast<TypedefNameDecl>(B));
      }
      for (const auto &[I, D] : LocalExterns)
        if (I >= 0 && &Frags[I] == &F)
          R->LocalExterns.push_back(D);
      // The compile had no conflict, so each declaration entered its
      // section with the kind the table holds. The exception, a
      // __declspec(allocate) or #pragma section that yields to a declared
      // kind, is not reused.
      for (auto &[D, Flags] : R->Sections) {
        const SectionAttr *A = D->getAttr<SectionAttr>();
        auto It = A ? Ctx.SectionInfos.find(A->getName())
                    : Ctx.SectionInfos.end();
        if (It == Ctx.SectionInfos.end() || A->isImplicit() ||
            A->getSyntax() == AttributeCommonInfo::AS_Declspec ||
            (It->second.SectionFlags & ASTContext::PSF_Implicit)) {
          if (R->Never == W_Reused)
            R->Never = W_Pragma;
          continue;
        }
        Flags = It->second.SectionFlags;
      }
      R->Refs = llvm::DenseSet<Name>();
      R->MacroSeen = llvm::DenseSet<const IdentifierInfo *>();
      R->Tokens = std::vector<const IdentifierInfo *>();
      R->DefinedHere = llvm::DenseSet<const IdentifierInfo *>();
      St.Latest[F.Key] = std::move(F.Rec);
    }
  }
};

class FragmentHook : public HeaderCacheHook {
  Compile &C;

public:
  explicit FragmentHook(Compile &C) : C(C) { WantsFragments = true; }
  Module *tryServe(FileEntryRef, SourceLocation,
                   ConstSearchDirIterator) override {
    return nullptr;
  }
  void fragmentEnd(FileID FID, unsigned Offset, bool InMacroArgs) override {
    C.end(FID, Offset, InMacroArgs);
  }
  bool fragmentStart(FileID FID, unsigned Offset, bool CanSkip,
                     FragmentSkip &Skip) override {
    return C.start(FID, Offset, CanSkip, Skip);
  }
};

class Candidates : public ExpansionCandidateObserver {
  Compile &C;

public:
  explicit Candidates(Compile &C) : C(C) {}
  void expansionCandidate(const Token &T) override { C.candidate(T); }
};

// A file's name is the path its compile first entered it by. The
// SourceManager keeps that path in the file's content cache (OrigEntry), and
// presumed locations, __FILE__, the debug information and the directory a
// quoted #include searches first all read it. The server's SourceManager
// outlives the compile, so at the compile's first entry of each file the
// path becomes the one this compile used, as in a SourceManager of its own.
// Locations of restored declarations, which point into an earlier compile's
// entry of the same file, then read this compile's path too.
class EntryNames : public PPCallbacks {
  SourceManager &SM;
  FileManager &FM;
  llvm::DenseSet<const SrcMgr::ContentCache *> Entered;

public:
  EntryNames(SourceManager &SM, FileManager &FM) : SM(SM), FM(FM) {}
  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID) override {
    if (Reason != EnterFile)
      return;
    bool Invalid = false;
    const SrcMgr::SLocEntry &E = SM.getSLocEntry(SM.getFileID(Loc), &Invalid);
    if (Invalid || !E.isFile())
      return;
    // The entry just made set the cache's Filename to the path it used.
    auto &CC = const_cast<SrcMgr::ContentCache &>(E.getFile().getContentCache());
    if (!CC.OrigEntry || !Entered.insert(&CC).second ||
        CC.OrigEntry->getName() == CC.Filename)
      return;
    if (OptionalFileEntryRef Ref = FM.getOptionalFileRef(CC.Filename))
      if (&Ref->getFileEntry() == &CC.OrigEntry->getFileEntry())
        CC.OrigEntry = *Ref;
  }
};

// The #pragma diagnostic lines (GCC and clang), as the DiagnosticsEngine
// took them: Pragma.cpp calls these only when the line changed its state.
class DiagPragmas : public PPCallbacks {
  Compile &C;

public:
  explicit DiagPragmas(Compile &C) : C(C) {}
  void PragmaDiagnosticPush(SourceLocation, StringRef) override {
    C.diagPush();
  }
  void PragmaDiagnosticPop(SourceLocation, StringRef) override {
    C.diagPop();
  }
  void PragmaDiagnostic(SourceLocation, StringRef, diag::Severity Mapping,
                        StringRef Str) override {
    C.diagPragma((Twine(unsigned(Mapping)) + Str).str());
  }
};

// The compile's diagnostics go to the reply as plain clang prints them, and
// each one is reported to the fragments (Compile::diagnosed).
class DiagWatch : public DiagnosticConsumer {
  std::unique_ptr<DiagnosticConsumer> Out;

public:
  Compile *C = nullptr;
  explicit DiagWatch(std::unique_ptr<DiagnosticConsumer> Out)
      : Out(std::move(Out)) {}
  void BeginSourceFile(const LangOptions &LO, const Preprocessor *PP) override {
    Out->BeginSourceFile(LO, PP);
  }
  void EndSourceFile() override { Out->EndSourceFile(); }
  void finish() override { Out->finish(); }
  bool IncludeInDiagnosticCounts() const override {
    return Out->IncludeInDiagnosticCounts();
  }
  // The compile ran out of source locations (2.10).
  bool SpaceEnded = false;
  void HandleDiagnostic(DiagnosticsEngine::Level L,
                        const Diagnostic &Info) override {
    if (Info.getID() == diag::err_sloc_space_too_large)
      SpaceEnded = true;
    DiagnosticConsumer::HandleDiagnostic(L, Info);
    Out->HandleDiagnostic(L, Info);
    if (C)
      C->diagnosed(L, Info.getLocation());
  }
};

// Option 3: the #define and #undef lines the Preprocessor handles.
class MacroLines : public PPCallbacks {
  Compile &C;

public:
  explicit MacroLines(Compile &C) : C(C) {}
  // Groups: the conditionals joined to a fragment.
  void If(SourceLocation, SourceRange, ConditionValueKind) override {
    C.condOpened();
  }
  void Ifdef(SourceLocation, const Token &Name,
             const MacroDefinition &) override {
    C.condTested(Name);
    C.condOpened();
  }
  void Ifndef(SourceLocation, const Token &Name,
              const MacroDefinition &) override {
    C.condTested(Name);
    C.condOpened();
  }
  void Elifdef(SourceLocation, const Token &Name,
               const MacroDefinition &) override {
    C.condTested(Name);
  }
  void Elifndef(SourceLocation, const Token &Name,
                const MacroDefinition &) override {
    C.condTested(Name);
  }
  void Defined(const Token &Name, const MacroDefinition &,
               SourceRange) override {
    C.condTested(Name);
  }
  void Endif(SourceLocation, SourceLocation) override { C.condClosed(); }
  void MacroDefined(const Token &Name, const MacroDirective *MD) override {
    const MacroInfo *Prior = nullptr;
    if (const MacroDirective *P = MD->getPrevious())
      if (const MacroDirective::DefInfo D = P->getDefinition();
          D && !D.isUndefined())
        Prior = D.getMacroInfo();
    C.macroLine(Name, MD->getMacroInfo(), MD->getLocation(), Prior);
  }
  void MacroUndefined(const Token &Name, const MacroDefinition &Def,
                      const MacroDirective *Undef) override {
    C.macroLine(Name, nullptr,
                Undef ? Undef->getLocation() : Name.getLocation(),
                Def.getMacroInfo());
  }
};

// Sema calls makeModuleVisible for the queued annot_module_include, once the
// parser is back at file scope (SemaModule.cpp, BuildModuleInclude).
class ServerLoader : public TrivialModuleLoader {
public:
  Compile *C = nullptr;
  void makeModuleVisible(Module *Mod, Module::NameVisibilityKind,
                         SourceLocation) override {
    if (C && Mod == C->RestoreModule)
      C->restoreNext();
  }
};

// The server's consumer. It pushes the implicit typedefs that the ASTContext
// caches across compiles into the new compile's scope without adding them to
// its translation unit (2.3): Sema::Initialize then finds them bound and does
// not add them to a second translation unit's declaration list. It runs
// first, as Sema::Initialize calls InitializeSema before its own pushes. With
// reuse it also records each parsed declaration's uses.
class ServerConsumer : public SemaConsumer {
  Compile *C;

public:
  explicit ServerConsumer(Compile *C) : C(C) {}
  void InitializeSema(Sema &S) override {
    ASTContext &Ctx = S.getASTContext();
    const TargetInfo &T = Ctx.getTargetInfo();
    auto Push = [&](NamedDecl *D) {
      S.PushOnScopeChains(D, S.TUScope, /*AddToContext=*/false);
    };
    if (T.hasInt128Type()) {
      Push(Ctx.getInt128Decl());
      Push(Ctx.getUInt128Decl());
    }
    Push(Ctx.getCFConstantStringDecl());
    if (T.hasBuiltinMSVaList())
      Push(Ctx.getBuiltinMSVaListDecl());
    Push(Ctx.getBuiltinVaListDecl());
    if (C)
      C->S = &S;
  }
  bool HandleTopLevelDecl(DeclGroupRef G) override {
    if (C && !C->Restoring) {
      C->readUnused();
      for (Decl *D : G)
        C->uses(D);
    }
    return true;
  }
  // First of the consumers, so before the code generator's.
  void HandleTranslationUnit(ASTContext &) override {
    if (C)
      C->releaseHeld();
  }
};

// The configuration identity of 2.1: the working directory, every language
// option (also those getModuleHash leaves out), the functions -fno-builtin
// names, and the target. Equal configurations have equal LangOptions, which
// one ASTContext requires.
std::string configKey(StringRef Cwd, const CompilerInvocation &Inv) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << Cwd << '\n';
  const LangOptions &L = Inv.getLangOpts();
#define LANGOPT(Name, Bits, Default, Compatibility, Description)              \
  OS << #Name << '=' << unsigned(L.Name) << ';';
#define ENUM_LANGOPT(Name, Type, Bits, Default, Compatibility, Description)   \
  OS << #Name << '=' << unsigned(L.get##Name()) << ';';
#include "clang/Basic/LangOptions.def"
  OS << "\nstd=" << unsigned(L.LangStd)
     << " sanitize=" << size_t(L.Sanitize.Mask.hash_value());
  for (const std::string &F : L.NoBuiltinFuncs)
    OS << " nobuiltin=" << F;
  for (const std::string &F : L.ModuleFeatures)
    OS << " feature=" << F;
  for (const auto &P : L.MacroPrefixMap)
    OS << " prefix=" << P.first << '=' << P.second;
  const TargetOptions &T = Inv.getTargetOpts();
  OS << "\ntriple=" << T.Triple << " cpu=" << T.CPU << " tune=" << T.TuneCPU
     << " fpmath=" << T.FPMath << " abi=" << T.ABI
     << " cmodel=" << T.CodeModel << " int128=" << T.ForceEnableInt128;
  for (const std::string &F : T.FeaturesAsWritten)
    OS << " w" << F;
  for (const std::string &F : T.Features)
    OS << ' ' << F;
  return S;
}

class Server {
  llvm::StringMap<CachedInvocation> Invocations;
  // Each configuration's memory spaces, side by side (user, 2026-11-06): a
  // space is a configuration of its own, with its own source-location space
  // and store.
  llvm::StringMap<std::vector<std::unique_ptr<Config>>> Configs;
  // The space a main file was first compiled in: it goes back there in later
  // builds, to its own store.
  llvm::StringMap<unsigned> SpaceOf;
  RawPCHContainerReader PCHReader;

  std::unique_ptr<Config> newConfig(const std::string &Cwd,
                                    CompilerInvocation &Inv);
  void reset(Config &C);
  unsigned checkFiles(Config &C);

  // The most source-location space one compile took so far, the spaces
  // replaced because they ran low, and the next configuration's number.
  uint64_t MaxCompileSpace = 0;
  unsigned Spaces = 0, NextConfigId = 0;

public:
  bool Reuse = false, Option3 = false, Groups = false, PerReading = false;
  // space-share=<s>: a new main file goes to the newest space only while
  // that space could take it within s of its 2 GiB; else a new space opens.
  // 0.5 leaves each space room for the same compiles again.
  double SpaceShare = 1.0;
  llvm::json::Object serve(const std::string &Cwd,
                           const std::vector<std::string> &Args,
                           const std::string &DumpPath,
                           const std::string &Build);
};

// 2.10, once per build: a file whose size, modification time or identity
// changed is forgotten, so that its next lookup reads it again as a new
// FileEntry, and its stored fragments are dropped; failed lookups are
// forgotten too, as the file may exist now. The SourceManager keeps the old
// text for the old entry, so earlier compiles' locations stay valid. A
// StringMap entry erased from a BumpPtrAllocator map keeps its memory, which
// the old FileEntryRefs still point to.
unsigned Server::checkFiles(Config &C) {
  FileManager &FM = *C.FileMgr;
  auto &Seen = FM.*get(SeenFilesTag());
  llvm::SmallPtrSet<const FileEntry *, 8> Changed;
  std::vector<std::string> Forget;
  for (auto &E : Seen) {
    if (!E.second) {
      Forget.push_back(E.first().str());
      continue;
    }
    const FileEntry &FE = FileEntryRef(E).getFileEntry();
    llvm::ErrorOr<llvm::vfs::Status> St =
        FM.getVirtualFileSystem().status(E.first());
    if (!St || St->getSize() != uint64_t(FE.getSize()) ||
        llvm::sys::toTimeT(St->getLastModificationTime()) !=
            FE.getModificationTime() ||
        St->getUniqueID() != FE.getUniqueID())
      Changed.insert(&FE);
  }
  for (auto &E : Seen)
    if (E.second && Changed.count(&FileEntryRef(E).getFileEntry()))
      Forget.push_back(E.first().str());
  for (const std::string &K : Forget)
    Seen.erase(K);
  auto &Unique = FM.*get(UniqueFilesTag());
  for (const FileEntry *FE : Changed)
    Unique.erase(FE->getUniqueID());
  auto &Dirs = FM.*get(SeenDirsTag());
  std::vector<std::string> Gone;
  for (auto &E : Dirs)
    if (!E.second)
      Gone.push_back(E.first().str());
  for (const std::string &K : Gone)
    Dirs.erase(K);
  Store &St = C.Fragments;
  for (auto &[Key, Id] : St.KeyOf)
    if (Changed.count(Key.first))
      St.Latest[Id].reset();
  return Changed.size();
}

std::unique_ptr<Config> Server::newConfig(const std::string &Cwd,
                                          CompilerInvocation &Inv) {
  auto Slot = std::make_unique<Config>();
  Config &C = *Slot;
  C.Id = NextConfigId++;
  C.Cwd = Cwd;
  C.LangOpts = Inv.getLangOpts();
  C.TargetOpts = Inv.getTargetOpts();
  C.DiagOpts = Inv.getDiagnosticOpts();
  C.Diags = new DiagnosticsEngine(DiagnosticIDs::create(), C.DiagOpts,
                                  new IgnoringDiagConsumer(), true);
  C.FileMgr = new FileManager(Inv.getFileSystemOpts(),
                              createVFSFromCompilerInvocation(Inv, *C.Diags));
  C.SourceMgr = new SourceManager(*C.Diags, *C.FileMgr);
  C.Target = TargetInfo::CreateTargetInfo(*C.Diags, C.TargetOpts);
  C.Target->adjust(*C.Diags, C.LangOpts, /*Aux=*/nullptr);
  // Builtins once per configuration, on the persistent table (2.2).
  C.Builtins.InitializeTarget(*C.Target, /*AuxTarget=*/nullptr);
  C.Builtins.initializeBuiltins(C.Idents, C.LangOpts);
  // TU_Incremental lets every compile add a translation unit; the
  // Preprocessor and Sema stay TU_Complete (2.2).
  C.Ctx = new ASTContext(C.LangOpts, *C.SourceMgr, C.Idents, C.Selectors,
                         C.Builtins, TU_Incremental);
  C.Ctx->InitBuiltinTypes(*C.Target, /*AuxTarget=*/nullptr);
  return Slot;
}

// The per-compile resets of 2.3, and the undo of 2.8: what the previous
// compile's instance left in objects that outlive it.
void Server::reset(Config &C) {
  for (const auto &Entry : C.Idents) {
    IdentifierInfo *II = Entry.getValue();
    // The previous Sema's file-scope bindings.
    II->setFETokenInfo(nullptr);
    // Macro state lives in the identifier, not only in the macro table.
    II->setIsFinal(false);
    II->setIsDeprecatedMacro(false);
    II->setIsRestrictExpansion(false);
    II->setHasMacroDefinition(false);
    II->setIsPoisoned(false);
    // The next Preprocessor sets the keyword's token kind again.
    if (II->hasRevertedTokenIDToIdentifier())
      II->revertIdentifierToTokenID(tok::identifier);
  }
  ASTContext &Ctx = *C.Ctx;
  Ctx.setFILEDecl(nullptr);
  Ctx.setjmp_bufDecl(nullptr);
  Ctx.setsigjmp_bufDecl(nullptr);
  Ctx.setucontext_tDecl(nullptr);
  // A fresh extern "C" context each compile: block-scope externs of earlier
  // compiles must not be found.
  Ctx.*get(ExternCContextTag()) = nullptr;
  (Ctx.*get(TypeInfoTag())).clear();
  (Ctx.*get(UnadjustedAlignTag())).clear();
  // Record layouts too: plain clang lays a struct out at its first use in a
  // compile, and -Wpadded and -Wpacked report there.
  (Ctx.*get(RecordLayoutsTag())).clear();
  // A documentation command an earlier compile found unknown (@literal) is
  // registered, so the next lookup would find it and not warn
  // (-Wdocumentation-unknown-command). Its name goes; its number stays for
  // the comments parsed before.
  for (comments::CommandInfo *CI :
       Ctx.getCommentCommandTraits().*get(CommentCommandsTag()))
    if (CI->IsUnknownCommand)
      CI->Name = "";
  // Each section's kind, as the compile's declarations entered it: another
  // compile's must not conflict (a restore enters its own again).
  Ctx.SectionInfos.clear();
  // 2.8: attributes a #pragma weak or redefine_extname added to restored
  // declarations, and the T[1] a tentative definition of incomplete array
  // type became at the end of the compile.
  Store &St = C.Fragments;
  for (auto &[D, A] : St.Attrs) {
    D->dropAttrs();
    if (!A.empty())
      D->setAttrs(A);
  }
  for (auto &[VD, T] : St.Types)
    VD->setType(T);
  St.Attrs.clear();
  St.Types.clear();
  // 2.8: the Used bits the last compile set on declarations a restore can
  // bring back. A restore sets those its fragment's parse set.
  static const UsedBitPlace Used = findUsedBit(Ctx);
  for (Decl *D : St.UsedToClear)
    clearUsed(D, Used);
  St.UsedToClear.clear();
}

llvm::json::Object Server::serve(const std::string &Cwd,
                                 const std::vector<std::string> &Args,
                                 const std::string &DumpPath,
                                 const std::string &Build) {
  auto Start = std::chrono::steady_clock::now();
  double Cpu0 = cpuMs();
  uint64_t Instr0 = instructions();
  llvm::json::Object R;
  auto Fail = [&](StringRef Why) {
    R["status"] = 2;
    R["error"] = Why.str();
    return std::move(R);
  };
  if (::chdir(Cwd.c_str()) != 0)
    return Fail("chdir failed");

  // The per-compile arguments, and the key of the parsed invocation.
  std::string Input, Output, MainName, Key = Cwd;
  for (size_t I = 0; I < Args.size(); ++I) {
    const std::string &A = Args[I];
    if ((A == "-o" || A == "-main-file-name") && I + 1 < Args.size()) {
      (A == "-o" ? Output : MainName) = Args[++I];
      Key += '\0' + A;
    } else if (I + 1 == Args.size() && !A.empty() && A[0] != '-') {
      Input = A;
    } else {
      Key += '\0' + A;
    }
  }
  if (Input.empty())
    return Fail("no input");

  CachedInvocation &CI = Invocations[Key];
  bool NewInvocation = !CI.Inv;
  bool NewConfig = false;
  if (NewInvocation) {
    std::vector<const char *> Argv;
    for (const std::string &A : Args)
      Argv.push_back(A.c_str());
    DiagnosticOptions DO;
    std::string Err;
    llvm::raw_string_ostream EOS(Err);
    DiagnosticsEngine D(DiagnosticIDs::create(), DO,
                        new TextDiagnosticPrinter(EOS, DO), true);
    auto Inv = std::make_shared<CompilerInvocation>();
    if (!CompilerInvocation::CreateFromArgs(*Inv, Argv, D) ||
        D.hasErrorOccurred()) {
      Invocations.erase(Key);
      return Fail("bad arguments: " + Err);
    }
    if (Inv->getFrontendOpts().Inputs.size() != 1)
      return Fail("one input expected");
    CI.Inv = Inv;
    CI.ConfigKey = configKey(Cwd, *Inv);
  }
  // Memory spaces side by side (user, 2026-11-06). A configuration's compiles
  // are spread over spaces, each with its own 2 GiB of source locations and
  // its own store. A main file always goes back to the space it was first
  // compiled in, where its fragments are. A new main file goes to the newest
  // space if that space could still take it within space-share of its 2 GiB
  // (twice the most any compile took so far); else a new space opens. A
  // space that cannot hold one more compile is replaced by an empty one, and
  // the old one is freed: its declarations keep locations in its own space,
  // so none of them can move. Only there is a store lost.
  const uint64_t Limit = uint64_t(1) << 31;
  auto Fits = [&](Config &S, double Share) {
    return S.SourceMgr->getNextLocalOffset() + 2 * MaxCompileSpace <=
           Share * double(Limit);
  };
  std::vector<std::unique_ptr<Config>> &Sp = Configs[CI.ConfigKey];
  auto [SpaceIt, NewMain] =
      SpaceOf.try_emplace(CI.ConfigKey + '\0' + Cwd + '\0' + Input, 0);
  if (NewMain) {
    if (Sp.empty() || !Fits(*Sp.back(), SpaceShare)) {
      Sp.push_back(newConfig(Cwd, *CI.Inv));
      NewConfig = true;
    }
    SpaceIt->second = Sp.size() - 1;
  }
  unsigned SpaceIdx = SpaceIt->second;
  bool NewSpace = false;
  if (!Fits(*Sp[SpaceIdx], 1.0)) {
    Sp[SpaceIdx] = newConfig(Cwd, *CI.Inv);
    NewSpace = true;
    ++Spaces;
  }
  CompilerInvocation &Inv = *CI.Inv;
  Config &C = *Sp[SpaceIdx];
  unsigned NumSpaces = Sp.size();
  uint64_t SpaceBefore = C.SourceMgr->getNextLocalOffset();
  FrontendOptions &FEOpts = Inv.getFrontendOpts();
  FEOpts.Inputs[0] = FrontendInputFile(Input, FEOpts.Inputs[0].getKind());
  FEOpts.OutputFile = Output;
  Inv.getCodeGenOpts().MainFileName = MainName;
  // Code generation, for the validation mode: the backend runs as plain
  // clang's (CodeGenAction.cpp), so -S, -emit-llvm and -c write what plain
  // clang writes, and objects can be compared byte for byte as the header
  // cache's are (artifact/check.sh).
  std::optional<BackendAction> Backend;
  switch (FEOpts.ProgramAction) {
  case frontend::ParseSyntaxOnly:
    break;
  case frontend::EmitLLVM:
    Backend = Backend_EmitLL;
    break;
  case frontend::EmitBC:
    Backend = Backend_EmitBC;
    break;
  case frontend::EmitAssembly:
    Backend = Backend_EmitAssembly;
    break;
  case frontend::EmitObj:
    Backend = Backend_EmitObj;
    break;
  default:
    return Fail("only -fsyntax-only, -emit-llvm, -emit-llvm-bc, -S and -c are served");
  }
  bool EmitLLVM = Backend.has_value();
  // What the server cannot do as plain clang does is not served: the driver
  // then compiles the job itself (status 2). LLVM options (-mllvm) are
  // process-wide; macro debug information, coverage mapping, time traces,
  // header traces and serialized diagnostics need callbacks or files that
  // cc1 or the code-generation action sets up; the server loads no modules,
  // PCH or plugins, and writes no output to standard output. A target that
  // gives external declarations debug information (BPF) has Sema list them
  // as they are declared, and a restore does not list them again.
  {
    const CodeGenOptions &CG = Inv.getCodeGenOpts();
    const DependencyOutputOptions &DO = Inv.getDependencyOutputOpts();
    if (!FEOpts.LLVMArgs.empty() || CG.MacroDebugInfo || CG.CoverageMapping ||
        !FEOpts.TimeTracePath.empty() || !FEOpts.Plugins.empty() ||
        !FEOpts.AddPluginActions.empty() || !FEOpts.ActionName.empty() ||
        DO.ShowHeaderIncludes || !DO.HeaderIncludeOutputFile.empty() ||
        !DO.DOTOutputFile.empty() ||
        !Inv.getDiagnosticOpts().DiagnosticSerializationFile.empty() ||
        Inv.getLangOpts().Modules ||
        !Inv.getPreprocessorOpts().ImplicitPCHInclude.empty() ||
        Output == "-" || llvm::Triple(Inv.getTargetOpts().Triple).isBPF())
      return Fail("options the server does not serve");
  }

  // Resets (2.3) and undo (2.8), and the file check of a new build (2.10).
  auto T = std::chrono::steady_clock::now();
  reset(C);
  unsigned FilesChanged = 0;
  bool Checked = !Build.empty() && Build != C.Build;
  if (Checked) {
    FilesChanged = checkFiles(C);
    C.Build = Build;
  }
  ASTContext &Ctx = *C.Ctx;
  std::string DiagText;
  llvm::raw_string_ostream DOS(DiagText);
  C.DiagOpts = Inv.getDiagnosticOpts();
  auto *Watch =
      new DiagWatch(std::make_unique<TextDiagnosticPrinter>(DOS, C.DiagOpts));
  C.Diags->setClient(Watch, true);
  C.Diags->Reset(/*soft=*/false);
  ProcessWarningOptions(*C.Diags, C.DiagOpts,
                        C.FileMgr->getVirtualFileSystem(),
                        /*ReportDiags=*/false);
  double ResetMs = msSince(T);

  // The compile's own objects (2.2).
  T = std::chrono::steady_clock::now();
  const LangOptions &LangOpts = Inv.getLangOpts();
  ServerLoader Loader;
  auto *HS = new HeaderSearch(Inv.getHeaderSearchOpts(), *C.SourceMgr,
                              *C.Diags, LangOpts, C.Target.get());
  auto PP = std::make_shared<Preprocessor>(
      Inv.getPreprocessorOpts(), *C.Diags, LangOpts, *C.SourceMgr, *HS, Loader,
      &C.Lookup, /*OwnsHeaderSearch=*/true, TU_Complete);
  PP->Initialize(*C.Target, /*AuxTarget=*/nullptr);
  InitializePreprocessor(*PP, Inv.getPreprocessorOpts(), PCHReader, FEOpts,
                         Inv.getCodeGenOpts());
  ApplyHeaderSearchOptions(PP->getHeaderSearchInfo(), Inv.getHeaderSearchOpts(),
                           LangOpts, C.Target->getTriple());
  PP->setPreprocessedOutput(false);
  PP->addPPCallbacks(std::make_unique<EntryNames>(*C.SourceMgr, *C.FileMgr));
  // -MD and the like: the dependency file, written at the end of the main
  // file, as CompilerInstance::createPreprocessor attaches it.
  std::shared_ptr<DependencyFileGenerator> Deps;
  if (!Inv.getDependencyOutputOpts().OutputFile.empty()) {
    Deps = std::make_shared<DependencyFileGenerator>(
        Inv.getDependencyOutputOpts());
    Deps->attachToPreprocessor(*PP);
  }
  bool HaveMain = CompilerInstance::InitializeSourceManager(
      FEOpts.Inputs[0], *C.Diags, *C.FileMgr, *C.SourceMgr);

  Ctx.addTranslationUnitDecl();
  Ctx.setTraversalScope({Ctx.getTranslationUnitDecl()});
  std::unique_ptr<Compile> Frags;
  std::unique_ptr<Candidates> Cands;
  if (Reuse) {
    Frags = std::make_unique<Compile>(C.Fragments, Ctx, *PP);
    Frags->Option3 = Option3;
    Frags->Groups = Groups;
    Frags->PerReading = PerReading;
    Frags->diagOptions(C.DiagOpts);
    PP->addPPCallbacks(std::make_unique<DiagPragmas>(*Frags));
    Watch->C = Frags.get();
    if (Option3 || Groups)
      PP->addPPCallbacks(std::make_unique<MacroLines>(*Frags));
    Frags->RestoreModule = HS->getModuleMap().findOrCreateModuleFirst(
        "__bothner_restore", nullptr, false, false);
    Loader.C = Frags.get();
    PP->setHeaderCacheHook(std::make_unique<FragmentHook>(*Frags));
    Cands = std::make_unique<Candidates>(*Frags);
    PP->setExpansionCandidateObserver(*Cands);
    Compile *FC = Frags.get();
    PP->setTokenWatcher([FC](const Token &Tok) { FC->token(Tok); });
  }
  std::vector<std::unique_ptr<ASTConsumer>> Consumers;
  Consumers.push_back(std::make_unique<ServerConsumer>(Frags.get()));
  std::unique_ptr<llvm::LLVMContext> LLVMCtx;
  CodeGenerator *CodeGen = nullptr;
  if (EmitLLVM) {
    LLVMCtx = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<CodeGenerator> G = CreateLLVMCodeGen(
        *C.Diags, Input, C.FileMgr->getVirtualFileSystemPtr(),
        Inv.getHeaderSearchOpts(), Inv.getPreprocessorOpts(),
        Inv.getCodeGenOpts(), *LLVMCtx);
    CodeGen = G.get();
    Consumers.push_back(std::move(G));
  }
  if (Frags)
    Frags->CodeGen = CodeGen;
  auto Consumer = std::make_unique<MultiplexConsumer>(std::move(Consumers));
  Consumer->Initialize(Ctx);
  auto S = std::make_unique<Sema>(*PP, Ctx, *Consumer, TU_Complete, nullptr);
  double SetupMs = msSince(T);

  T = std::chrono::steady_clock::now();
  C.Diags->getClient()->BeginSourceFile(LangOpts, PP.get());
  if (HaveMain)
    ParseAST(*S, /*PrintStats=*/false, /*SkipFunctionBodies=*/false);
  C.Diags->getClient()->EndSourceFile();
  PP->EndSourceFile();
  double ParseMs = msSince(T);
  bool Errors = C.Diags->hasErrorOccurred() || !HaveMain;

  T = std::chrono::steady_clock::now();
  if (Frags)
    Frags->finalize(Errors);
  double FinalizeMs = msSince(T);

  T = std::chrono::steady_clock::now();
  if (CodeGen && !Errors) {
    std::unique_ptr<llvm::Module> M(CodeGen->ReleaseModule());
    std::error_code EC;
    bool Binary = *Backend == Backend_EmitBC || *Backend == Backend_EmitObj;
    auto OS = std::make_unique<llvm::raw_fd_ostream>(
        Output, EC, Binary ? llvm::sys::fs::OF_None : llvm::sys::fs::OF_Text);
    if (EC || !M) {
      Errors = true;
    } else {
      // As BackendConsumer::HandleTranslationUnit, without its clearing of
      // the AST (-clear-ast-before-backend), which the server keeps.
      llvm::LLVMContext &LC = M->getContext();
      LC.setDefaultTargetCPU(Inv.getTargetOpts().CPU);
      LC.setDefaultTargetFeatures(llvm::join(Inv.getTargetOpts().Features, ","));
      EmbedBitcode(M.get(), Inv.getCodeGenOpts(), llvm::MemoryBufferRef());
      CompilerInstance BackendCI(CI.Inv);
      BackendCI.setDiagnostics(C.Diags);
      BackendCI.setVirtualFileSystem(C.FileMgr->getVirtualFileSystemPtr());
      emitBackendOutput(BackendCI, Inv.getCodeGenOpts(),
                        Ctx.getTargetInfo().getDataLayoutString(), M.get(),
                        *Backend, C.FileMgr->getVirtualFileSystemPtr(),
                        std::move(OS));
      Errors |= C.Diags->hasErrorOccurred();
    }
  }
  if (!DumpPath.empty()) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(DumpPath, EC);
    if (!EC)
      dumpDecls(Ctx, OS, Frags ? ArrayRef<Decl *>(Frags->RestoredTop)
                               : ArrayRef<Decl *>());
  }
  double OutputMs = msSince(T);

  llvm::json::Object Decisions;
  unsigned Reused = 0, Parsed = 0;
  if (Frags) {
    for (unsigned W = 0; W < W_N; ++W)
      if (Frags->Counts[W])
        Decisions[WhyName[W]] = Frags->Counts[W];
    Reused = Frags->Counts[W_Reused];
    for (unsigned W = W_NoSkip; W < W_N; ++W)
      Parsed += Frags->Counts[W];
    R["restored_decls"] = Frags->NsRestored;
    llvm::json::Object Files;
    // The keys are copied: the map dies with the compile, before the
    // answer is written.
    for (const auto &E : Frags->ReusedFiles)
      Files[E.first().str()] = E.second;
    R["reused_files"] = std::move(Files);
  }

  T = std::chrono::steady_clock::now();
  unsigned NumErrors = C.Diags->getNumErrors();
  unsigned NumWarnings = C.Diags->getNumWarnings();
  // The count line cc1 prints after the diagnostics, from the printer's
  // counts (CompilerInstance::printDiagnosticStats).
  std::string Stats;
  if (C.DiagOpts.ShowCarets) {
    unsigned W = Watch->getNumWarnings(), E = Watch->getNumErrors();
    llvm::raw_string_ostream SS(Stats);
    if (W)
      SS << W << " warning" << (W == 1 ? "" : "s");
    if (W && E)
      SS << " and ";
    if (E)
      SS << E << " error" << (E == 1 ? "" : "s");
    if (W || E)
      SS << " generated.\n";
  }
  bool SpaceEnded = Watch->SpaceEnded;
  Watch->C = nullptr;
  S.reset();
  Consumer.reset();
  LLVMCtx.reset();
  PP.reset();
  Cands.reset();
  Frags.reset();
  C.Diags->setClient(new IgnoringDiagConsumer(), true);
  double TeardownMs = msSince(T);
  ++C.Compiles;

  R["status"] = Errors ? 1 : 0;
  // 2.10: the server's earlier compiles used up the source locations, which
  // a plain compile of this file alone has whole. The driver compiles it.
  if (SpaceEnded) {
    R["status"] = 2;
    R["error"] = "source locations used up";
  }
  R["errors"] = NumErrors;
  R["warnings"] = NumWarnings;
  R["config"] = C.Id;
  R["new_config"] = NewConfig;
  R["new_invocation"] = NewInvocation;
  R["config_compiles"] = C.Compiles;
  R["reset_ms"] = ResetMs;
  R["setup_ms"] = SetupMs;
  R["parse_ms"] = ParseMs;
  R["finalize_ms"] = FinalizeMs;
  R["output_ms"] = OutputMs;
  R["teardown_ms"] = TeardownMs;
  R["wall_ms"] = msSince(Start);
  R["cpu_ms"] = cpuMs() - Cpu0;
  R["instructions"] = instructions() - Instr0;
  uint64_t SpaceAfter = C.SourceMgr->getNextLocalOffset();
  MaxCompileSpace = std::max(MaxCompileSpace, SpaceAfter - SpaceBefore);
  R["sloc_bytes"] = SpaceAfter;
  R["new_space"] = NewSpace;
  R["spaces_replaced"] = Spaces;
  R["space"] = SpaceIdx;
  R["spaces"] = NumSpaces;
  R["rss_kib"] = rssKiB();
  R["idents"] = uint64_t(C.Idents.size());
  if (Checked)
    R["files_changed"] = FilesChanged;
  if (Reuse) {
    R["reused"] = Reused;
    R["parsed"] = Parsed;
    R["decisions"] = std::move(Decisions);
  }
  R["diags"] = DiagText;
  R["stats"] = Stats;
  return R;
}

class ServerAction : public PluginASTAction {
  std::string Requests, Results, Socket;
  bool Reuse = false, Option3 = false, Groups = false, PerReading = false;
  double SpaceShare = 1.0;

protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 StringRef) override {
    return std::make_unique<ASTConsumer>();
  }

  bool ParseArgs(const CompilerInstance &,
                 const std::vector<std::string> &Args) override {
    for (StringRef A : Args) {
      if (A.consume_front("requests="))
        Requests = A.str();
      else if (A.consume_front("results="))
        Results = A.str();
      else if (A.consume_front("socket="))
        Socket = A.str();
      else if (A == "reuse=1")
        Reuse = true;
      else if (A == "option=3")
        Option3 = true;
      else if (A == "groups=1")
        Groups = true;
      else if (A == "keys=reading")
        PerReading = true;
      else if (A.consume_front("space-share="))
        A.getAsDouble(SpaceShare);
    }
    if (Requests.empty() == Socket.empty() || Results.empty()) {
      llvm::errs() << "bothner-server: requests= or socket=, and results=, "
                      "are required\n";
      return false;
    }
    return true;
  }

  // The serial loop of 2.1, over a file of requests, or with requests=- over
  // standard input, one request answered before the next is read (a test can
  // then change files between two requests). results=- answers on standard
  // output. With socket=<path> the requests come from drivers (driver.cpp,
  // CLANG_COMPILE_SERVER), one a connection, in the order they connect; each
  // is answered on its connection and also written to the results. A
  // request {"quit": true} stops the server.
  void ExecuteAction() override {
    std::error_code EC;
    std::unique_ptr<llvm::raw_fd_ostream> File;
    if (Results != "-") {
      File = std::make_unique<llvm::raw_fd_ostream>(Results, EC);
      if (EC) {
        llvm::errs() << "bothner-server: cannot write " << Results << "\n";
        return;
      }
    }
    llvm::raw_ostream &Out = File ? *File : llvm::outs();
    Server Srv;
    Srv.Reuse = Reuse;
    Srv.Option3 = Option3;
    Srv.Groups = Groups;
    Srv.PerReading = PerReading;
    Srv.SpaceShare = SpaceShare;
    unsigned Index = 0;
    bool Quit = false;
    auto Handle = [&](StringRef Line) {
      llvm::Expected<llvm::json::Value> V = llvm::json::parse(Line);
      llvm::json::Object Result;
      const llvm::json::Object *O = V ? V->getAsObject() : nullptr;
      if (!V)
        llvm::consumeError(V.takeError());
      if (!O) {
        Result["status"] = 2;
        Result["error"] = "bad request line";
      } else if (O->getBoolean("quit").value_or(false)) {
        Quit = true;
        Result["status"] = 0;
      } else {
        std::string Cwd = O->getString("cwd").value_or("").str();
        std::string Dump = O->getString("dump").value_or("").str();
        std::string Build = O->getString("build").value_or("").str();
        std::vector<std::string> Args;
        if (const llvm::json::Array *A = O->getArray("args"))
          for (const llvm::json::Value &E : *A)
            Args.push_back(E.getAsString().value_or("").str());
        Result = Srv.serve(Cwd, Args, Dump, Build);
      }
      Result["i"] = Index++;
      std::string Text;
      llvm::raw_string_ostream(Text) << llvm::json::Value(std::move(Result));
      Out << Text << '\n';
      Out.flush();
      return Text;
    };
    if (!Socket.empty()) {
      if (llvm::Error E = ast_cache::checkSocketPath(Socket)) {
        llvm::errs() << "bothner-server: " << llvm::toString(std::move(E))
                     << "\n";
        return;
      }
      llvm::Expected<llvm::ListeningSocket> L =
          llvm::ListeningSocket::createUnix(Socket);
      if (!L) {
        llvm::errs() << "bothner-server: cannot listen on " << Socket << ": "
                     << llvm::toString(L.takeError()) << "\n";
        return;
      }
      llvm::errs() << "bothner-server: listening on " << Socket << "\n";
      llvm::errs().flush();
      while (!Quit) {
        llvm::Expected<std::unique_ptr<llvm::raw_socket_stream>> C =
            L->accept();
        if (!C) {
          llvm::consumeError(C.takeError());
          continue;
        }
        std::optional<std::string> Body = ast_cache::receiveMessage(**C);
        if (!Body || Body->empty() ||
            (*Body)[0] != char(ast_cache::MessageKind::CompileRequest))
          continue;
        std::string Answer(4, '\0');
        Answer.push_back(char(ast_cache::MessageKind::CompileReply));
        Answer += Handle(StringRef(*Body).drop_front());
        llvm::support::endian::write32le(Answer.data(), Answer.size() - 4);
        ast_cache::sendMessage(**C, Answer);
      }
      return;
    }
    if (Requests == "-") {
      char *Line = nullptr;
      size_t Cap = 0;
      ssize_t N;
      while ((N = ::getline(&Line, &Cap, stdin)) > 0) {
        StringRef L(Line, N);
        L = L.trim();
        if (!L.empty())
          Handle(L);
      }
      free(Line);
      return;
    }
    auto Buf = llvm::MemoryBuffer::getFile(Requests);
    if (!Buf) {
      llvm::errs() << "bothner-server: cannot read " << Requests << "\n";
      return;
    }
    SmallVector<StringRef, 0> Lines;
    (*Buf)->getBuffer().split(Lines, '\n', -1, /*KeepEmpty=*/false);
    for (StringRef Line : Lines)
      Handle(Line);
  }

  ActionType getActionType() override { return CmdlineAfterMainAction; }
};

class DumpConsumer : public ASTConsumer {
public:
  void HandleTranslationUnit(ASTContext &Ctx) override {
    const char *Path = getenv("BOTHNER_DUMP_OUT");
    if (!Path)
      return;
    std::error_code EC;
    llvm::raw_fd_ostream OS(Path, EC);
    if (!EC)
      dumpDecls(Ctx, OS);
  }
};

class DumpAction : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 StringRef) override {
    return std::make_unique<DumpConsumer>();
  }
  bool ParseArgs(const CompilerInstance &,
                 const std::vector<std::string> &) override {
    return true;
  }
  // Before the main action: with -clear-ast-before-backend the code
  // generator frees the AST at the end of the compile.
  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<ServerAction>
    XServer("bothner-server", "Bothner's compile server");
static FrontendPluginRegistry::Add<DumpAction>
    XDump("bothner-dump", "Declaration summary for the server's checks");
