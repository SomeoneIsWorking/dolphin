// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/GcnPortRuntime.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

namespace
{
constexpr u32 ENTRY_ADDRESS = 0x80001000;
constexpr u32 CONTINUATION_ADDRESS = 0x80002000;
constexpr u32 ADDI_R3_R3_1 = 0x38630001;
constexpr u32 BRANCH_BACK_ONE_INSTRUCTION = 0x4bfffffc;
constexpr u32 BRANCH_TO_SELF = 0x48000000;

PowerPC::GcnPort::ExecutionIdentity MakeIdentity(u8 discriminator, u64 generation = 1)
{
  PowerPC::GcnPort::ExecutionIdentity identity;
  identity.image.sha256.front() = discriminator;
  identity.module_generation = generation;
  return identity;
}

struct OriginalThenContinueHook
{
  u32 calls = 0;

  static PowerPC::GcnPort::HookResult Run(void* context, PowerPC::PowerPCState&) noexcept
  {
    auto& hook = *static_cast<OriginalThenContinueHook*>(context);
    ++hook.calls;
    if (hook.calls == 1)
      return {.action = PowerPC::GcnPort::HookAction::RunOriginalOnce};
    return {.action = PowerPC::GcnPort::HookAction::ContinueAtAddress,
            .continuation = CONTINUATION_ADDRESS};
  }
};

void RunShippingJitScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  Core::DeclareAsCPUThread();
  UICommon::SetUserDirectory(profile_path);
  Config::Init();
  SConfig::Init();

  Core::System& system = Core::System::GetInstance();
  system.GetMemory().Init();
  system.GetCoreTiming().Init();
  system.GetCPU().Init(PowerPC::DefaultCPUCore());

  system.GetMemory().Write_U32(ADDI_R3_R3_1, ENTRY_ADDRESS);
  system.GetMemory().Write_U32(BRANCH_BACK_ONE_INSTRUCTION, ENTRY_ADDRESS + sizeof(u32));
  system.GetMemory().Write_U32(BRANCH_TO_SELF, CONTINUATION_ADDRESS);

  auto& state = system.GetPPCState();
  auto& power_pc = system.GetPowerPC();
  const auto identity = MakeIdentity(1);
  PowerPC::GcnPort::RuntimeSession runtime(system, identity);

  state.gpr[3] = 0;
  state.pc = ENTRY_ADDRESS;
  state.npc = ENTRY_ADDRESS;
  power_pc.SingleStep();

  const auto unhooked = runtime.GetExecutionCounters();
  EXPECT_GE(state.gpr[3], 2u);
  EXPECT_GE(unhooked.jit_blocks_compiled, 1u);
  EXPECT_GE(unhooked.cold_block_executions, 1u);
  EXPECT_GE(unhooked.cache_hit_block_executions, 1u);
  EXPECT_EQ(unhooked.jit_block_executions,
            unhooked.cold_block_executions + unhooked.cache_hit_block_executions);

  OriginalThenContinueHook hook;
  const PowerPC::GcnPort::HookKey key{identity, ENTRY_ADDRESS};
  runtime.InstallNativeHook(key, {.context = &hook, .function = &OriginalThenContinueHook::Run});
  const u64 invalidations_after_install = runtime.GetExecutionCounters().invalidations;
  EXPECT_GE(invalidations_after_install, 1u);

  state.gpr[3] = 0;
  state.pc = ENTRY_ADDRESS;
  state.npc = ENTRY_ADDRESS;
  power_pc.SingleStep();

  const auto hooked = runtime.GetExecutionCounters();
  EXPECT_EQ(hook.calls, 2u);
  EXPECT_EQ(state.gpr[3], 1u);
  EXPECT_EQ(hooked.hooks_executed, 2u);
  EXPECT_EQ(hooked.original_entries, 1u);
  EXPECT_GT(hooked.jit_blocks_compiled, unhooked.jit_blocks_compiled);

  runtime.SetExecutionIdentity(MakeIdentity(1, 2));
  hook.calls = 0;
  state.gpr[3] = 0;
  state.pc = ENTRY_ADDRESS;
  state.npc = ENTRY_ADDRESS;
  power_pc.SingleStep();

  EXPECT_EQ(hook.calls, 0u);
  EXPECT_GE(state.gpr[3], 2u);
  EXPECT_GT(runtime.GetExecutionCounters().invalidations, invalidations_after_install);

  system.GetCPU().Shutdown();
  system.GetCoreTiming().Shutdown();
  system.GetMemory().Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
  Core::UndeclareAsCPUThread();
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, ShippingJitCacheHookOriginalAndInvalidation)
{
  // Dolphin's BLR optimization installs a guard in the current CPU thread's stack. A dedicated
  // thread gives the test the same fully mapped stack contract as Dolphin's shipping CPU thread.
  std::thread cpu_thread(RunShippingJitScenario);
  cpu_thread.join();
}
}  // namespace
