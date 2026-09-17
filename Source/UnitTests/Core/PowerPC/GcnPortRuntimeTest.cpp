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
#include "Core/PowerPC/Gekko.h"
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

std::vector<u8> BigEndianImage(std::initializer_list<u32> words)
{
  std::vector<u8> bytes;
  bytes.reserve(words.size() * sizeof(u32));
  for (const u32 word : words)
  {
    bytes.push_back(static_cast<u8>(word >> 24));
    bytes.push_back(static_cast<u8>(word >> 16));
    bytes.push_back(static_cast<u8>(word >> 8));
    bytes.push_back(static_cast<u8>(word));
  }
  return bytes;
}

// Proves the public one-block adapter surface (BootAuthenticatedImage, ExecuteJitBlock,
// ExecuteOriginalOnce, ExecuteRefusedBlock, ExecuteDiagnosticInterpreterBlock, and typed fallback
// counters) rather than the raw Memory::Write_U32 + PowerPC::SingleStep harness the original
// ShippingJit test uses. This is still Dolphin's own gtest binary; the outside-library proof lives
// in gcnport's own dolphin_backend test.
void RunPublicAdapterScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x80004000;
  constexpr u32 HOOK_ADDRESS = 0x80005000;
  constexpr u32 PROGRAM_LANDING_PAD_ADDRESS = 0x80006000;
  // PROGRAM_ADDRESS is the same two-instruction self-loop as RunShippingJitScenario's ENTRY_ADDRESS
  // body, used only to prove the plain Compiled/CacheHit ExecuteJitBlock contract: a block that
  // branches back to its own start never needs a second, not-yet-compiled successor block, so it is
  // the only synthetic shape this slice can bound to exactly one *guest-body* execution per call.
  //
  // HOOK_ADDRESS is a separate, straight-line (non-looping) block used only for the hook/
  // ExecuteOriginalOnce proof. Dolphin's analyzer may merge several passes of a tight reflexive loop
  // into one compiled unit (observed empirically: a plain two-instruction self-loop's originalSize
  // came back as a multiple of 2, not exactly 2), and the native-hook guard is emitted at the top of
  // the generated code, so a merged loop body re-evaluates that guard once per internal pass within a
  // single ExecuteJitBlock() call. A one-shot ticket armed by ExecuteOriginalOnce would then be
  // consumed by the first internal pass and leak a real hook invocation on the second. A block with no
  // back-edge to itself has exactly one guard evaluation per compiled-block execution, so the hook
  // proof uses HOOK_ADDRESS -> PROGRAM_LANDING_PAD_ADDRESS (itself a harmless branch-to-self stopping
  // point, never hooked) instead of looping back into the hooked address.
  std::vector<u8> program = BigEndianImage({ADDI_R3_R3_1, BRANCH_BACK_ONE_INSTRUCTION});
  program.resize(HOOK_ADDRESS - PROGRAM_ADDRESS, 0);
  // addi r3,r3,1 ; b PROGRAM_LANDING_PAD_ADDRESS (unconditional forward branch, no back edge).
  constexpr u32 HOOK_BRANCH_DISPLACEMENT = PROGRAM_LANDING_PAD_ADDRESS - (HOOK_ADDRESS + sizeof(u32));
  constexpr u32 BRANCH_TO_LANDING_PAD = 0x48000000 | (HOOK_BRANCH_DISPLACEMENT & 0x03FFFFFCu);
  static_assert((HOOK_BRANCH_DISPLACEMENT & ~0x03FFFFFCu) == 0,
                "landing pad displacement must fit in the B-form 24-bit field");
  const std::vector<u8> hook_body_bytes = BigEndianImage({ADDI_R3_R3_1, BRANCH_TO_LANDING_PAD});
  program.insert(program.end(), hook_body_bytes.begin(), hook_body_bytes.end());
  program.resize(PROGRAM_LANDING_PAD_ADDRESS - PROGRAM_ADDRESS, 0);
  const std::vector<u8> landing_pad_bytes = BigEndianImage({BRANCH_TO_SELF});
  program.insert(program.end(), landing_pad_bytes.begin(), landing_pad_bytes.end());
  const auto identity = MakeIdentity(2);

  Core::System& system = Core::System::GetInstance();

  const PowerPC::GcnPort::ExecutionIdentity unauthenticated;
  EXPECT_FALSE(PowerPC::GcnPort::BootAuthenticatedImage(system, unauthenticated, program,
                                                         PROGRAM_ADDRESS, PROGRAM_ADDRESS)
                   .ok);

  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(system, identity, program,
                                                                 PROGRAM_ADDRESS, PROGRAM_ADDRESS);
  ASSERT_TRUE(booted.ok) << booted.detail;

  // The process may boot at most one image at a time; a second call must fail rather than
  // silently reinitializing state the first boot still owns.
  EXPECT_FALSE(PowerPC::GcnPort::BootAuthenticatedImage(system, identity, program,
                                                         PROGRAM_ADDRESS, PROGRAM_ADDRESS)
                   .ok);

  {
    PowerPC::GcnPort::RuntimeSession runtime(system, identity);
    auto& state = system.GetPPCState();
    state.gpr[3] = 0;

    const auto first = runtime.ExecuteJitBlock();
    EXPECT_EQ(first.kind, PowerPC::GcnPort::JitBlockKind::Compiled);
    EXPECT_EQ(first.guest_pc, PROGRAM_ADDRESS);
    // Dolphin's analyzer may merge more than one pass of a tight two-instruction reflexive loop into
    // a single compiled block; only a multiple of the 2-instruction body is guaranteed.
    EXPECT_GE(first.instruction_count, 2u);
    EXPECT_EQ(first.instruction_count % 2, 0u);
    EXPECT_GE(state.gpr[3], 1u);

    const auto after_first = runtime.GetExecutionCounters();
    EXPECT_EQ(after_first.jit_blocks_compiled, 1u);
    EXPECT_EQ(after_first.fallback_events, 0u);

    // A second entry at the same address/feature flags is a cache hit, not a recompile: the block
    // branches back to its own already-published start, so there is never a second, not-yet-compiled
    // successor address for this shape.
    state.pc = PROGRAM_ADDRESS;
    state.npc = PROGRAM_ADDRESS;
    state.gpr[3] = 0;
    const auto second = runtime.ExecuteJitBlock();
    EXPECT_EQ(second.kind, PowerPC::GcnPort::JitBlockKind::CacheHit);
    EXPECT_EQ(runtime.GetExecutionCounters().jit_blocks_compiled, after_first.jit_blocks_compiled);

    struct CountingHook
    {
      u32 calls = 0;
      static PowerPC::GcnPort::HookResult Run(void* context, PowerPC::PowerPCState&) noexcept
      {
        ++static_cast<CountingHook*>(context)->calls;
        // Redirect to a separate, unhooked branch-to-self landing pad. LR is never initialized in
        // this minimal harness (ReturnToCaller would jump to it), and HOOK_ADDRESS's own body already
        // falls through toward that same landing pad on the unhooked path.
        return PowerPC::GcnPort::HookResult::ContinueAt(PROGRAM_LANDING_PAD_ADDRESS);
      }
    };
    CountingHook hook;
    const PowerPC::GcnPort::HookKey key{identity, HOOK_ADDRESS};
    runtime.InstallNativeHook(key, {.context = &hook, .function = &CountingHook::Run});

    // ExecuteOriginalOnce arms a ticket that must run the ordinary body once WITHOUT ever invoking
    // the hook callback, proving a real synchronous native -> original -> native continuation
    // distinct from a hook returning HookAction::RunOriginalOnce from inside its own callback.
    EXPECT_TRUE(runtime.ExecuteOriginalOnce(key));
    state.pc = HOOK_ADDRESS;
    state.npc = HOOK_ADDRESS;
    state.gpr[3] = 0;
    const auto ticketed = runtime.ExecuteJitBlock();
    (void)ticketed;
    EXPECT_EQ(hook.calls, 0u);
    EXPECT_EQ(state.gpr[3], 1u);
    EXPECT_EQ(runtime.GetExecutionCounters().original_tickets_armed, 1u);
    EXPECT_GE(runtime.GetExecutionCounters().original_entries, 1u);

    // The ticket is single-use: the next dispatch must invoke the hook body normally.
    state.pc = HOOK_ADDRESS;
    state.npc = HOOK_ADDRESS;
    const u64 hooks_executed_before = runtime.GetExecutionCounters().hooks_executed;
    const auto after_ticket = runtime.ExecuteJitBlock();
    (void)after_ticket;
    EXPECT_EQ(hook.calls, 1u);
    EXPECT_GT(runtime.GetExecutionCounters().hooks_executed, hooks_executed_before);

    // Explicit bounded interpreter entry points, independent of JIT dispatch and each other.
    const auto refused = runtime.ExecuteRefusedBlock(PROGRAM_ADDRESS, 1);
    EXPECT_EQ(refused.guest_pc, PROGRAM_ADDRESS);
    EXPECT_EQ(refused.instruction_count, 1u);

    state.pc = PROGRAM_ADDRESS;
    state.npc = PROGRAM_ADDRESS;
    const auto diagnostic = runtime.ExecuteDiagnosticInterpreterBlock(1);
    EXPECT_EQ(diagnostic.guest_pc, PROGRAM_ADDRESS);
    EXPECT_EQ(diagnostic.instruction_count, 1u);
  }

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, PublicAdapterBootExecuteOriginalAndTypedFallback)
{
  std::thread cpu_thread(RunPublicAdapterScenario);
  cpu_thread.join();
}

// Proves CallOriginalSynchronously's "superCall" contract: a native hook callback does native work,
// calls through to the ORIGINAL guest body as a subroutine, gets control back in the SAME callback
// invocation once the guest body returns, does more native work, and only then decides the final
// HookResult. This is distinct from ExecuteOriginalOnce/HookAction::RunOriginalOnce, both of which
// only arm a suppression the OUTER ExecuteJitBlock driver consumes on its next dispatch and never
// hand control back to the requesting callback.
void RunSynchronousOriginalCallScenario()
{
  constexpr u32 SUPER_CALL_HOOK_ADDRESS = 0x80003000;
  constexpr u32 SUPER_CALL_RETURN_ADDRESS = 0x80004000;
  constexpr u32 ADDI_R3_R3_10 = 0x3863000A;
  constexpr u32 BLR = 0x4e800020;

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

  // The ORIGINAL guest body: addi r3,r3,10 ; blr. This is what the hook calls through to.
  system.GetMemory().Write_U32(ADDI_R3_R3_10, SUPER_CALL_HOOK_ADDRESS);
  system.GetMemory().Write_U32(BLR, SUPER_CALL_HOOK_ADDRESS + sizeof(u32));
  // Landing pad the hook's ReturnToCaller result resumes at; a harmless branch-to-self stopping
  // point, matching the same pattern the other scenarios in this file use.
  system.GetMemory().Write_U32(BRANCH_TO_SELF, SUPER_CALL_RETURN_ADDRESS);

  auto& state = system.GetPPCState();
  auto& power_pc = system.GetPowerPC();
  const auto identity = MakeIdentity(4);
  PowerPC::GcnPort::RuntimeSession runtime(system, identity);

  struct SuperCallHook
  {
    PowerPC::GcnPort::RuntimeSession* session = nullptr;
    PowerPC::GcnPort::HookKey key;
    u32 gpr3_before_call = 0;
    u32 gpr3_after_call = 0;
    u32 original_instruction_count = 0;
    bool native_before_ran = false;
    bool native_after_ran = false;

    static PowerPC::GcnPort::HookResult Run(void* context, PowerPC::PowerPCState& state) noexcept
    {
      auto& hook = *static_cast<SuperCallHook*>(context);
      // Native work BEFORE calling through to the original guest body.
      hook.native_before_ran = true;
      state.gpr[3] += 1;
      hook.gpr3_before_call = state.gpr[3];

      const auto result = hook.session->CallOriginalSynchronously(hook.key, 4);
      hook.original_instruction_count = result.instruction_count;
      hook.gpr3_after_call = state.gpr[3];

      // Native work AFTER the original body returned control to this same callback invocation.
      state.gpr[3] += 1000;
      hook.native_after_ran = true;
      return PowerPC::GcnPort::HookResult::ReturnToCaller();
    }
  };

  SuperCallHook hook;
  hook.session = &runtime;
  hook.key = PowerPC::GcnPort::HookKey{identity, SUPER_CALL_HOOK_ADDRESS};
  runtime.InstallNativeHook(hook.key, {.context = &hook, .function = &SuperCallHook::Run});

  state.gpr[3] = 100;
  state.spr[SPR_LR] = SUPER_CALL_RETURN_ADDRESS;
  state.pc = SUPER_CALL_HOOK_ADDRESS;
  state.npc = SUPER_CALL_HOOK_ADDRESS;
  power_pc.SingleStep();

  EXPECT_TRUE(hook.native_before_ran);
  EXPECT_TRUE(hook.native_after_ran);
  // 100 (initial) + 1 (native work before the call) == 101, observed by the callback right before
  // calling through.
  EXPECT_EQ(hook.gpr3_before_call, 101u);
  // 101 + 10 (the ORIGINAL guest body's own addi) == 111, observed by the callback right after the
  // call returns -- this is the assertion that the guest body's side effect genuinely happened
  // BETWEEN the two halves of native work, inside the same callback invocation.
  EXPECT_EQ(hook.gpr3_after_call, 111u);
  EXPECT_EQ(hook.original_instruction_count, 2u);
  // 111 + 1000 (native work after the call) == 1111, the final guest-visible state.
  EXPECT_EQ(state.gpr[3], 1111u);
  EXPECT_EQ(state.pc, SUPER_CALL_RETURN_ADDRESS);

  const auto counters = runtime.GetExecutionCounters();
  EXPECT_EQ(counters.hooks_executed, 1u);
  EXPECT_EQ(counters.synchronous_original_calls, 1u);
  EXPECT_EQ(counters.synchronous_original_instructions, 2u);
  EXPECT_GE(counters.original_entries, 1u);

  system.GetCPU().Shutdown();
  system.GetCoreTiming().Shutdown();
  system.GetMemory().Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
  Core::UndeclareAsCPUThread();
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, HookCallsOriginalSynchronouslyThenResumesNativeWork)
{
  std::thread cpu_thread(RunSynchronousOriginalCallScenario);
  cpu_thread.join();
}

// ClassifyFallbackReason is a pure host-side function (no guest execution), so it is verified
// directly against the real Jit64_Tables.cpp / JitArm64_Tables.cpp opcode-31 fallback lists rather
// than by provoking a live JIT/interpreter exception path.
TEST(GcnPortRuntime, ClassifyFallbackReasonMatchesStaticOpcodeTables)
{
  using PowerPC::GcnPort::ClassifyFallbackReason;
  using PowerPC::GcnPort::JitRefusalReason;

  // mfsr r4,0: opcode 31, subop 595 -- supervisor-only segment-register access.
  EXPECT_EQ(ClassifyFallbackReason(0x7C8004A6), JitRefusalReason::PrivilegedInstruction);
  // tlbie r5: opcode 31, subop 306.
  EXPECT_EQ(ClassifyFallbackReason(0x7C002A64), JitRefusalReason::PrivilegedInstruction);
  // icbi 0,r5: opcode 31, subop 982 -- an ordinary user-mode instruction the JIT never lowers.
  EXPECT_EQ(ClassifyFallbackReason(0x7C002FAC), JitRefusalReason::UnsupportedInstruction);
  // addi r3,r3,1: opcode 14, not an opcode-31 fallback at all; must not be misclassified.
  EXPECT_EQ(ClassifyFallbackReason(ADDI_R3_R3_1), JitRefusalReason::UnsupportedInstruction);
}
}  // namespace
