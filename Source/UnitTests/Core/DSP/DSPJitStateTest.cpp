// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>

#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSP/Jit/x64/DSPEmitter.h"
#include "Core/DSP/Jit/x64/DSPJitRegCache.h"

#include <gtest/gtest.h>

namespace
{
using namespace Gen;

const void* ResolveOperand(const DSP::SDSP& state, OpArg operand)
{
  X64CodeBlock code;
  code.AllocCodeSpace(4096);
  const auto resolve = reinterpret_cast<const void* (*)(const DSP::SDSP*)>(code.AlignCode4());
  code.PUSH(R15);
  code.MOV(64, R(R15), R(ABI_PARAM1));
  code.LEA(64, RAX, operand);
  code.POP(R15);
  code.RET();
  return resolve(&state);
}
}  // namespace

TEST(DSPJitState, RegisterOperandsAddressTheLiveState)
{
  DSP::DSPCore core;
  const auto& state = core.DSPState();
  const auto& r = state.r;
  // Independent address oracle in architectural register order, including composites.
  const std::array<const void*, 37> expected = {
      &r.ar[0],     &r.ar[1],   &r.ar[2],   &r.ar[3],   &r.ix[0],     &r.ix[1],     &r.ix[2],
      &r.ix[3],     &r.wr[0],   &r.wr[1],   &r.wr[2],   &r.wr[3],     &r.st[0],     &r.st[1],
      &r.st[2],     &r.st[3],   &r.ac[0].h, &r.ac[1].h, &r.cr,        &r.sr,        &r.prod.l,
      &r.prod.m,    &r.prod.h,  &r.prod.m2, &r.ax[0].l, &r.ax[1].l,   &r.ax[0].h,   &r.ax[1].h,
      &r.ac[0].l,   &r.ac[1].l, &r.ac[0].m, &r.ac[1].m, &r.ax[0].val, &r.ax[1].val, &r.ac[0].val,
      &r.ac[1].val, &r.prod.val};

  for (size_t reg = 0; reg < expected.size(); ++reg)
  {
    SCOPED_TRACE(reg);
    auto operand = DSP::JIT::x64::GetRegisterMemory(state, reg);
    EXPECT_EQ(ResolveOperand(state, operand), expected[reg]);
    // A neighboring displacement must not be accepted as the intended register.
    operand.AddMemOffset(sizeof(u16));
    EXPECT_NE(ResolveOperand(state, operand), expected[reg]);
  }
}

TEST(DSPJitState, ClearProductWritesOnlyTheProductRegister)
{
  DSP::DSPCore core;
  auto& state = core.DSPState();
  state.r.prod.val = ~u64{0};
  state.r.sr = 0x5a5a;
  state.r.ax[0].val = 0x12345678;
  DSP::JIT::x64::DSPEmitter emitter(core);
  const auto clear = reinterpret_cast<void (*)()>(emitter.AlignCode4());
  emitter.PUSH(R15);
  emitter.MOV(64, R(R15), ImmPtr(&state));
  emitter.clrp(0);
  emitter.POP(R15);
  emitter.RET();
  clear();
  EXPECT_EQ(state.r.prod.val, 0x001000fffff00000ULL);
  EXPECT_EQ(state.r.sr, 0x5a5a);
  EXPECT_EQ(state.r.ax[0].val, 0x12345678U);
}
