//===- PragmaStateCheck.cpp -------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Sema/Sema.h"
#include "llvm/Support/xxhash.h"

using namespace clang;
using namespace clang::ast_cache;

namespace {

/// A PragmaStack is at its default when nothing has been pushed and the
/// current value is still the one the stack was constructed with. Checking
/// CurrentValue alone would miss "#pragma pack(push, 1)" followed by a pop
/// back to the default, which leaves a residual stack a spliced import
/// cannot reproduce.
template <typename T> bool isDefault(const Sema::PragmaStack<T> &Stack) {
  return Stack.Stack.empty() && !Stack.hasValue();
}

} // namespace

const char *clang::ast_cache::findNonDefaultPragmaState(const Sema &S) {
  // #pragma pack and #pragma float_control are *not* checked for a default
  // current value: both are captured in a PragmaSnapshot, so a non-default
  // value is a recorded dependency rather than a reason to decline.
  //
  // Their stacks must still be empty. applyPragmaDelta replays a net change
  // with a plain set, which cannot reproduce a stack a header pushed and did
  // not pop -- and a header that pops more than it pushed reads its
  // includer's stack rather than a single current value, which no snapshot
  // can express either.
  if (!S.AlignPackStack.Stack.empty())
    return "pack (unbalanced push)";
  if (!S.FpPragmaStack.Stack.empty())
    return "float_control (unbalanced push)";

  // Everything below is state a PragmaSnapshot does not carry, so it has to
  // be at its default in order to be irrelevant.
  if (!isDefault(S.VtorDispStack))
    return "vtordisp";
  if (!isDefault(S.DataSegStack))
    return "data_seg";
  if (!isDefault(S.BSSSegStack))
    return "bss_seg";
  if (!isDefault(S.ConstSegStack))
    return "const_seg";
  if (!isDefault(S.CodeSegStack))
    return "code_seg";
  if (!isDefault(S.StrictGuardStackCheckStack))
    return "strict_gs_check";

  if (S.MSStructPragmaOn)
    return "ms_struct";
  if (S.CurInitSeg)
    return "init_seg";
  // Lazily allocated on the first "#pragma GCC visibility push" and nulled by
  // FreeVisContext. A non-null but empty stack is treated as non-default:
  // over-declining costs a miss, and the alternative needs the definition of
  // VisStack, which is local to SemaAttr.cpp.
  if (S.VisContext)
    return "GCC visibility";
  if (!S.PragmaAttributeStack.empty())
    return "clang attribute";
  if (S.OptimizeOffPragmaLocation.isValid())
    return "clang optimize";

  if (S.PragmaClangBSSSection.Valid || S.PragmaClangDataSection.Valid ||
      S.PragmaClangRodataSection.Valid || S.PragmaClangRelroSection.Valid ||
      S.PragmaClangTextSection.Valid)
    return "clang section";

  return nullptr;
}

PragmaSnapshot clang::ast_cache::capturePragmaState(const Sema &S) {
  PragmaSnapshot Snap;
  Snap.AlignPackRaw =
      Sema::AlignPackInfo::getRawEncoding(S.AlignPackStack.CurrentValue);
  Snap.FpPragmaRaw = S.FpPragmaStack.CurrentValue.getAsOpaqueInt();
  return Snap;
}

void clang::ast_cache::applyPragmaDelta(Sema &S, const PragmaSnapshot &Entry,
                                        const PragmaSnapshot &Exit) {
  // Only a net change is replayed. Everything the header did *internally* is
  // already baked into the Decls it produced, so the stack's intermediate
  // history is not needed -- only what it left standing for the includer.
  if (Entry == Exit)
    return;

  // PSK_Set with no label, which is what "#pragma pack(n)" performs: replace
  // the current value without touching the stack. A header that left a net
  // *push* behind is not cached at all (findNonDefaultPragmaState rejects a
  // non-empty stack at write time), so there is never a stack shape to
  // reproduce here.
  if (Entry.AlignPackRaw != Exit.AlignPackRaw)
    S.AlignPackStack.Act(SourceLocation(), Sema::PSK_Set, llvm::StringRef(),
                         Sema::AlignPackInfo::getFromRawEncoding(
                             Exit.AlignPackRaw));

  if (Entry.FpPragmaRaw != Exit.FpPragmaRaw)
    S.FpPragmaStack.Act(
        SourceLocation(), Sema::PSK_Set, llvm::StringRef(),
        FPOptionsOverride::getFromOpaqueInt(Exit.FpPragmaRaw));
}

uint64_t clang::ast_cache::warningFlagsHash(const DiagnosticOptions &Opts) {
  std::string Key;
  Key += Opts.IgnoreWarnings ? 'w' : '-';
  Key += Opts.Pedantic ? 'p' : '-';
  Key += Opts.PedanticErrors ? 'e' : '-';
  for (const std::vector<std::string> *List :
       {&Opts.Warnings, &Opts.Remarks, &Opts.SystemHeaderWarningsModules}) {
    Key += '\0';
    for (const std::string &Flag : *List) {
      Key += Flag;
      Key += '\1';
    }
  }
  Key += '\0';
  Key += Opts.DiagnosticSuppressionMappingsFile;
  return llvm::xxh3_64bits(Key);
}

const DiagnosticsEngine &WarningPragmaTracker::commandLine() const {
  // The levels the flags alone give. The compile's own engine cannot say:
  // a pragma at a file's first offset changes its first state in place.
  if (!CommandLine) {
    CommandLine = std::make_unique<DiagnosticsEngine>(
        PP->getDiagnostics().getDiagnosticIDs(), *Opts,
        new IgnoringDiagConsumer());
    ProcessWarningOptions(*CommandLine, *Opts,
                          PP->getFileManager().getVirtualFileSystem(),
                          /*ReportDiags=*/false);
  }
  return *CommandLine;
}

uint64_t WarningPragmaTracker::fingerprint(SourceLocation At) const {
  bool ByLevel = PP && At.isValid() &&
                 !PP->getSourceManager().isInSystemHeader(
                     PP->getSourceManager().getExpansionLoc(At));
  std::optional<uint64_t> &Memo = ByLevel ? Cached : CachedText;
  if (Memo)
    return *Memo;
  std::string Key;
  for (const Level &L : Levels) {
    Key += '\2';
    Key += std::to_string(L.Opaque);
  }
  // What the pragmas in force change: each warning they name whose level at
  // \p At differs from the command line's. Without a preprocessor, or for
  // -Weverything, their text.
  std::vector<std::pair<unsigned, unsigned>> Changed;
  const DiagnosticsEngine *Diags = ByLevel ? &PP->getDiagnostics() : nullptr;
  for (const Level &L : Levels)
    for (const WarningPragma &P : L.Changes) {
      StringRef Option = P.Option;
      SmallVector<diag::kind, 16> IDs;
      if (!Diags || Option.size() < 3 ||
          Option.substr(2) == "everything" ||
          Diags->getDiagnosticIDs()->getDiagnosticsInGroup(
              Option[1] == 'W' ? diag::Flavor::WarningOrError
                               : diag::Flavor::Remark,
              Option.substr(2), IDs)) {
        Key += '\1';
        Key += char('0' + P.Severity);
        Key += P.Option;
        continue;
      }
      for (diag::kind ID : IDs) {
        auto Now = Diags->getDiagnosticLevel(ID, At);
        if (Now != commandLine().getDiagnosticLevel(ID, SourceLocation()))
          Changed.push_back({ID, Now});
      }
    }
  llvm::sort(Changed);
  Changed.erase(llvm::unique(Changed), Changed.end());
  for (auto [ID, Lvl] : Changed) {
    Key += '\3';
    Key += std::to_string(ID);
    Key += ':';
    Key += std::to_string(Lvl);
  }
  Memo = llvm::xxh3_64bits(Key);
  return *Memo;
}

WarningPragmaTracker::Mark WarningPragmaTracker::mark() const {
  return {Levels.size(), Levels.back().Id, Levels.back().Changes.size(),
          Levels.back().Opaque};
}

std::optional<std::vector<WarningPragma>>
WarningPragmaTracker::since(const Mark &Begin) const {
  const Level &Top = Levels.back();
  if (Levels.size() != Begin.Depth || Top.Id != Begin.Level ||
      Top.Opaque != Begin.Opaque || Top.Changes.size() < Begin.Changes)
    return std::nullopt;
  return std::vector<WarningPragma>(Top.Changes.begin() + Begin.Changes,
                                    Top.Changes.end());
}

void WarningPragmaTracker::apply(DiagnosticsEngine &Diags,
                                 const WarningPragma &P, SourceLocation Loc) {
  // As PragmaDiagnosticHandler does.
  StringRef Option = P.Option;
  if (Option.size() < 3 || Option[0] != '-' ||
      (Option[1] != 'W' && Option[1] != 'R'))
    return;
  diag::Flavor Flavor =
      Option[1] == 'W' ? diag::Flavor::WarningOrError : diag::Flavor::Remark;
  auto Severity = static_cast<diag::Severity>(P.Severity);
  StringRef Group = Option.substr(2);
  if (Group == "everything")
    Diags.setSeverityForAll(Flavor, Severity, Loc);
  else if (Diags.setSeverityForGroup(Flavor, Group, Severity, Loc))
    return;
  Levels.back().Changes.push_back(P);
  Cached.reset();
  CachedText.reset();
}

uint64_t clang::ast_cache::warningState(uint64_t FlagsHash,
                                        const WarningPragmaTracker *Pragmas,
                                        SourceLocation At) {
  static const uint64_t NoPragmas = WarningPragmaTracker().fingerprint();
  uint64_t Words[2] = {FlagsHash,
                       Pragmas ? Pragmas->fingerprint(At) : NoPragmas};
  return llvm::xxh3_64bits(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Words), sizeof(Words)));
}
