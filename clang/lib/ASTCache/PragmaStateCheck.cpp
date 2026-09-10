//===- PragmaStateCheck.cpp -------------------------------------------------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ASTCache/PragmaStateCheck.h"
#include "clang/Sema/Sema.h"

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
