// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <iterator>

#include "Common/ScopeGuard.h"
#include "Common/x64ABI.h"
#include "Core/Core.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/EmuCodeBlock.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/System.h"

#include <gtest/gtest.h>

namespace
{
using namespace Gen;

class StateOffsetEmitter final : public EmuCodeBlock
{
public:
  explicit StateOffsetEmitter(Jit64& jit) : EmuCodeBlock(jit) { AllocCodeSpace(4096); }

  void CheckScalars()
  {
    const auto& state = GetPPCState();
    CheckOperand(PPCSTATE(Exceptions), &state.Exceptions);
    CheckOperand(PPCSTATE(downcount), &state.downcount);
    CheckOperand(PPCSTATE(feature_flags), &state.feature_flags);
    CheckOperand(PPCSTATE(gather_pipe_base_ptr), &state.gather_pipe_base_ptr);
    CheckOperand(PPCSTATE(gather_pipe_ptr), &state.gather_pipe_ptr);
    CheckOperand(PPCSTATE(mem_ptr), &state.mem_ptr);
    CheckOperand(PPCSTATE(msr), &state.msr);
    CheckOperand(PPCSTATE(npc), &state.npc);
    CheckOperand(PPCSTATE(pagetable_update_pending), &state.pagetable_update_pending);
    CheckOperand(PPCSTATE(pc), &state.pc);
    CheckOperand(PPCSTATE(stored_stack_pointer), &state.stored_stack_pointer);
    CheckOperand(PPCSTATE(cr), &state.cr);
    CheckOperand(PPCSTATE(fpscr), &state.fpscr);
    CheckOperand(PPCSTATE(xer_ca), &state.xer_ca);
    CheckOperand(PPCSTATE(xer_so_ov), &state.xer_so_ov);
    CheckOperand(PPCSTATE(xer_stringctrl), &state.xer_stringctrl);
  }

  void CheckArrayEndpoints()
  {
    const auto& state = GetPPCState();
    for (const size_t i : std::array<size_t, 2>{0, std::size(state.gpr) - 1})
      CheckOperand(PPCSTATE_GPR(i), &state.gpr[i]);
    for (const size_t i : std::array<size_t, 2>{0, std::size(state.cr.fields) - 1})
      CheckOperand(PPCSTATE_CR(i), &state.cr.fields[i]);
    for (const size_t i : std::array<size_t, 2>{0, std::size(state.sr) - 1})
      CheckOperand(PPCSTATE_SR(i), &state.sr[i]);
    for (const size_t i : std::array<size_t, 2>{0, std::size(state.spr) - 1})
      CheckOperand(PPCSTATE_SPR(i), &state.spr[i]);
    for (const size_t i : std::array<size_t, 2>{0, std::size(state.ps) - 1})
    {
      CheckOperand(PPCSTATE_PS0(i), &state.ps[i].ps0);
      CheckOperand(PPCSTATE_PS1(i), &state.ps[i].ps1);
    }
    CheckOperand(PPCSTATE_LR, &state.spr[SPR_LR]);
    CheckOperand(PPCSTATE_CTR, &state.spr[SPR_CTR]);
    CheckOperand(PPCSTATE_SRR0, &state.spr[SPR_SRR0]);
    CheckOperand(PPCSTATE_SRR1, &state.spr[SPR_SRR1]);
  }

private:
  const void* ResolveOperand(const OpArg& operand)
  {
    const auto resolve = reinterpret_cast<const void* (*)()>(AlignCode4());
    PUSH(RPPCSTATE);
    // The shipping dispatcher keeps the actual state base biased by 0x80 in RBP.
    MOV(64, R(RPPCSTATE), ImmPtr(reinterpret_cast<const u8*>(&GetPPCState()) + 0x80));
    LEA(64, RAX, operand);
    POP(RPPCSTATE);
    RET();
    return resolve();
  }

  void CheckOperand(OpArg operand, const void* expected)
  {
    SCOPED_TRACE(expected);
    EXPECT_EQ(ResolveOperand(operand), expected);
    operand.AddMemOffset(1);
    EXPECT_NE(ResolveOperand(operand), expected);
  }
};
}  // namespace

TEST(Jit64, StateOffsetsAddressTheLiveState)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_thread_guard([] { Core::UndeclareAsCPUThread(); });
  Jit64 jit(Core::System::GetInstance());
  StateOffsetEmitter emitter(jit);
  emitter.CheckScalars();
  emitter.CheckArrayEndpoints();
}
