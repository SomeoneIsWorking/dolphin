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
#include "InputCommon/ControllerInterface/ControllerInterface.h"
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

// Proves BootAuthenticatedImage's apply_gamecube_os_init flag actually installs the exact retail
// GameCube MSR/BAT configuration CBoot::EmulatedBS2_GC applies before jumping to a disc's DOL entry
// point (see CBoot::SetupGameCubeBS2Registers), rather than merely compiling. Asserting the raw SPR
// contents matches this file's own convention of testing through the public one-block adapter
// surface, not a synthetic guest program: MSR/BAT registers are host-visible state the adapter is
// responsible for, independent of what guest code the caller boots.
void RunGameCubeOsInitScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x80007000;
  const std::vector<u8> program = BigEndianImage({BRANCH_TO_SELF});
  const auto identity = MakeIdentity(3);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      /*apply_gamecube_os_init=*/true);
  ASSERT_TRUE(booted.ok) << booted.detail;

  const auto& ppc_state = system.GetPPCState();
  EXPECT_EQ(ppc_state.msr.DR, 1u);
  EXPECT_EQ(ppc_state.msr.IR, 1u);
  EXPECT_EQ(ppc_state.msr.FP, 1u);
  EXPECT_EQ(ppc_state.msr.RI, 1u);
  // CBoot::SetupBAT's exact retail GameCube constants (Boot_BS2Emu.cpp): BAT0 maps the cached
  // 0x80000000 effective mirror and BAT1 maps the 0xC0000000 uncached mirror, both onto physical
  // RAM starting at 0. Without these (real mode, MSR.DR/IR == 0) an ordinary effective address like
  // PROGRAM_ADDRESS above would be used as a physical address instead, far outside GC RAM.
  EXPECT_EQ(ppc_state.spr[SPR_IBAT0U], 0x80001fffu);
  EXPECT_EQ(ppc_state.spr[SPR_IBAT0L], 0x00000002u);
  EXPECT_EQ(ppc_state.spr[SPR_DBAT0U], 0x80001fffu);
  EXPECT_EQ(ppc_state.spr[SPR_DBAT0L], 0x00000002u);
  EXPECT_EQ(ppc_state.spr[SPR_DBAT1U], 0xc0001fffu);
  EXPECT_EQ(ppc_state.spr[SPR_DBAT1L], 0x0000002au);

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, BootAuthenticatedImageAppliesGameCubeOsInitRegisters)
{
  std::thread cpu_thread(RunGameCubeOsInitScenario);
  cpu_thread.join();
}

// The default (apply_gamecube_os_init=false, the parameter's default value) must be unchanged: a
// caller booting a small synthetic PPC test program that never relies on effective-address
// translation keeps real-mode MSR/BAT state exactly as before this flag existed.
void RunGameCubeOsInitDefaultOffScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x80008000;
  const std::vector<u8> program = BigEndianImage({BRANCH_TO_SELF});
  const auto identity = MakeIdentity(4);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(system, identity, program,
                                                                 PROGRAM_ADDRESS, PROGRAM_ADDRESS);
  ASSERT_TRUE(booted.ok) << booted.detail;

  const auto& ppc_state = system.GetPPCState();
  EXPECT_EQ(ppc_state.msr.DR, 0u);
  EXPECT_EQ(ppc_state.msr.IR, 0u);
  EXPECT_EQ(ppc_state.spr[SPR_IBAT0U], 0u);
  EXPECT_EQ(ppc_state.spr[SPR_DBAT0U], 0u);

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, BootAuthenticatedImageDefaultsToNoGameCubeOsInit)
{
  std::thread cpu_thread(RunGameCubeOsInitDefaultOffScenario);
  cpu_thread.join();
}

// Regression falsifier for a timekeeping defect that made every scheduled CoreTiming event
// unreachable, and for the block-granularity contract that defect's first attempted fix broke.
//
// ExecuteJitBlock must run exactly one JIT BLOCK per call while leaving ppc_state.downcount alone.
// CoreTiming::Advance() -- which Dolphin's generated dispatcher calls on entry -- derives elapsed
// guest time as `slice_length - DowncountToCycles(downcount)`, so downcount on entry must still be
// the previous slice's natural remainder. Writing a sentinel of 1 into it while slice_length was
// also 1 (the steady state a one-block-at-a-time caller settles into) yielded 1 - 1 == 0 and froze
// the global timer permanently: no scheduled event could come due, so no hardware completion
// interrupt was ever raised and a title polling for one span forever. One block per call instead
// comes from the one-cycle slice cap BootAuthenticatedImage installs.
//
// The three assertions below are each an independent discriminator:
//   * ticks must strictly increase per dispatch          -- fails if downcount is overwritten;
//   * a block must hold more than one instruction        -- fails under MAIN_ENABLE_DEBUGGING, which
//                                                           also bounds per block but drives the
//                                                           analyzer into single-instruction blocks;
//   * r3 must advance by exactly one block's worth        -- fails if a call runs zero or several
//                                                           blocks.
void RunBlockBoundaryAndCoreTimingScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x80009000;
  // Three adds then an unconditional branch back to the first of them: a four-instruction loop in
  // which exactly three of every four instructions retired is an increment of r3. How many of those
  // iterations the analyzer folds into one block is its own decision -- it follows the unconditional
  // backward branch, and has been observed forming both a 4-instruction and a 12-instruction block
  // for this body -- so the assertions below are written against that ratio rather than any fixed
  // block size, which is not part of this API's contract.
  constexpr u32 BRANCH_BACK_THREE_INSTRUCTIONS = 0x4bfffff4;
  constexpr u32 ADDS_PER_ITERATION = 3;
  constexpr u32 INSTRUCTIONS_PER_ITERATION = 4;
  const std::vector<u8> program = BigEndianImage(
      {ADDI_R3_R3_1, ADDI_R3_R3_1, ADDI_R3_R3_1, BRANCH_BACK_THREE_INSTRUCTIONS});
  const auto identity = MakeIdentity(5);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      /*apply_gamecube_os_init=*/true);
  ASSERT_TRUE(booted.ok) << booted.detail;

  PowerPC::GcnPort::RuntimeSession runtime(system, identity);
  auto& core_timing = system.GetCoreTiming();
  auto& ppc_state = system.GetPPCState();
  ppc_state.gpr[3] = 0;

  constexpr u32 DISPATCHES = 16;
  u64 previous_ticks = core_timing.GetTicks();
  for (u32 dispatch = 0; dispatch < DISPATCHES; ++dispatch)
  {
    const u32 r3_before = ppc_state.gpr[3];
    const auto outcome = runtime.ExecuteJitBlock();

    // Blocks must stay block-sized. MAIN_ENABLE_DEBUGGING would also make the dispatcher exit once
    // per block, but it additionally drives the analyzer into single-instruction blocks; this is the
    // assertion that tells the two apart.
    EXPECT_GT(outcome.instruction_count, 1u)
        << "dispatch " << dispatch << " ran a single-instruction block, not a real one";

    // instruction_count is read from the block that STARTED at the pc this dispatch entered, so if
    // the dispatcher chained a second block the adds would scale while instruction_count would not.
    // Equating the two through the loop body's fixed 3-adds-per-4-instructions ratio therefore
    // pins "exactly one block ran" without hard-coding how large the analyzer made that block.
    const u32 adds = ppc_state.gpr[3] - r3_before;
    EXPECT_EQ(adds * INSTRUCTIONS_PER_ITERATION, outcome.instruction_count * ADDS_PER_ITERATION)
        << "dispatch " << dispatch << " retired " << adds << " adds against a block of "
        << outcome.instruction_count << " instructions, i.e. not exactly one block";

    const u64 ticks = core_timing.GetTicks();
    EXPECT_GT(ticks, previous_ticks) << "the CoreTiming global timer did not advance across "
                                        "dispatch "
                                     << dispatch << "; scheduled events can never come due";
    previous_ticks = ticks;
  }

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, ExecuteJitBlockAdvancesCoreTimingAndRunsExactlyOneBlock)
{
  std::thread cpu_thread(RunBlockBoundaryAndCoreTimingScenario);
  cpu_thread.join();
}

// SerialInterfaceManager's periodic poll calls g_controller_interface.UpdateInput() unconditionally,
// before and independently of asking any SI channel for data, and that call asserts on m_is_init.
// Forcing every channel to SIDEVICE_NONE is therefore not sufficient -- the poll still runs, because
// an SI poll with nothing plugged in is what real hardware does. A hardware-init boot must leave the
// interface initialized in its headless, no-devices-present form, and must hand it back on shutdown
// so a second boot in the same process starts from the same state.
void RunHeadlessControllerInterfaceScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x8000a000;
  const std::vector<u8> program = BigEndianImage({BRANCH_TO_SELF});
  const auto identity = MakeIdentity(6);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      /*apply_gamecube_os_init=*/true, /*apply_gamecube_hardware_init=*/true);
  ASSERT_TRUE(booted.ok) << booted.detail;

  EXPECT_TRUE(g_controller_interface.IsInit())
      << "a hardware-init boot left SerialInterfaceManager's poll without an initialized "
         "ControllerInterface to call UpdateInput() on";

  PowerPC::GcnPort::ShutdownBootedImage(system);

  EXPECT_FALSE(g_controller_interface.IsInit())
      << "ShutdownBootedImage leaked an initialized ControllerInterface into the next boot";

  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, HardwareInitBootOwnsHeadlessControllerInterface)
{
  std::thread cpu_thread(RunHeadlessControllerInterfaceScenario);
  cpu_thread.join();
}

// ExecuteJitBlocks exists because one block per host call costs a host round trip per block: against
// exact GMSE01 it measured ~180,000 blocks/second, i.e. ~740,000 guest instructions/second, far under
// GameCube speed. A batch lifts gcnport's one-block slice cap so the dispatcher chains direct-linked
// blocks natively.
//
// The contract this pins is that speed is bought without giving up measurability or correctness:
// every block still reports itself through the JIT's own per-block callback, so the counters advance
// by exactly the number of blocks the batch claims; a batch runs far more blocks per call than the
// one-block path; and the one-block path still works afterwards, proving the lifted cap was restored
// rather than leaked.
void RunBatchedExecutionScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x8000b000;
  constexpr u32 BRANCH_BACK_THREE_INSTRUCTIONS = 0x4bfffff4;
  const std::vector<u8> program = BigEndianImage(
      {ADDI_R3_R3_1, ADDI_R3_R3_1, ADDI_R3_R3_1, BRANCH_BACK_THREE_INSTRUCTIONS});
  const auto identity = MakeIdentity(7);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      /*apply_gamecube_os_init=*/true);
  ASSERT_TRUE(booted.ok) << booted.detail;

  PowerPC::GcnPort::RuntimeSession runtime(system, identity);

  // A batch must be asked for real work; zero is a caller error, not an empty success.
  const auto empty = runtime.ExecuteJitBlocks(0);
  EXPECT_TRUE(empty.backend_fault);
  EXPECT_EQ(empty.blocks_executed, 0u);

  constexpr u64 REQUESTED_BLOCKS = 5000;
  const u64 executions_before = runtime.GetExecutionCounters().jit_block_executions;
  const auto batch = runtime.ExecuteJitBlocks(REQUESTED_BLOCKS);

  EXPECT_FALSE(batch.backend_fault) << batch.detail;
  EXPECT_GE(batch.blocks_executed, REQUESTED_BLOCKS) << batch.detail;
  // The counters are the runtime's own ledger; a batch that ran blocks the ledger never saw would
  // buy its speed by going dark, which is the one trade this API must not make.
  EXPECT_EQ(runtime.GetExecutionCounters().jit_block_executions - executions_before,
            batch.blocks_executed);

  // The point of the API: one host call covered far more than the one block the stepping path does.
  EXPECT_GT(batch.blocks_executed, 1u);

  // The lifted slice cap must be restored, or every later one-block call would silently run a whole
  // slice instead.
  const u64 before_single = runtime.GetExecutionCounters().jit_block_executions;
  const auto single = runtime.ExecuteJitBlock();
  EXPECT_NE(single.kind, PowerPC::GcnPort::JitBlockKind::BackendFault) << single.detail;
  EXPECT_EQ(runtime.GetExecutionCounters().jit_block_executions - before_single, 1u)
      << "the one-block slice cap was not restored after a batch";

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, ExecuteJitBlocksChainsBlocksAndRestoresTheOneBlockCap)
{
  std::thread cpu_thread(RunBatchedExecutionScenario);
  cpu_thread.join();
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

// Proves BootAuthenticatedImage's apply_gamecube_hardware_init flag actually builds a working
// MMIO::Mapping handler table (HW::Init -> MemoryManager::InitMMIO), using a small synthetic program
// that stores to, then reads back, the real GameCube ProcessorInterface hardware register at
// physical/effective address 0x0C003004 (PI_INTERRUPT_MASK; see ProcessorInterface.cpp's
// RegisterMMIO) -- the exact register and address a real GMSE01 boot's own __init_hardware code
// faulted on before this flag existed (docs/dolphin-embedding-contract.md). Boots in real
// addressing mode (apply_gamecube_os_init left at its default false) so the effective address the
// program uses is also the physical address the MMIO table is registered under, independent of the
// BAT setup this file already covers in the os-init tests above.
void RunHardwareInitMmioScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x80009000;
  constexpr u32 LIS_R3_PI_BASE = 0x3C600C00;             // lis r3, 0x0C00
  constexpr u32 ADDI_R3_R3_PI_MASK_OFFSET = 0x38633004;  // addi r3, r3, 0x3004 (PI_INTERRUPT_MASK)
  constexpr u32 LIS_R4_TEST_VALUE_HI = 0x3C801234;       // lis r4, 0x1234
  constexpr u32 ORI_R4_R4_TEST_VALUE_LO = 0x60845678;    // ori r4, r4, 0x5678
  constexpr u32 STW_R4_0_R3 = 0x90830000;                // stw r4, 0(r3)
  constexpr u32 LWZ_R5_0_R3 = 0x80A30000;                // lwz r5, 0(r3)
  const std::vector<u8> program =
      BigEndianImage({LIS_R3_PI_BASE, ADDI_R3_R3_PI_MASK_OFFSET, LIS_R4_TEST_VALUE_HI,
                       ORI_R4_R4_TEST_VALUE_LO, STW_R4_0_R3, LWZ_R5_0_R3, BRANCH_TO_SELF});
  const auto identity = MakeIdentity(5);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      /*apply_gamecube_os_init=*/false, /*apply_gamecube_hardware_init=*/true);
  ASSERT_TRUE(booted.ok) << booted.detail;

  PowerPC::GcnPort::RuntimeSession runtime(system, identity);
  const auto outcome = runtime.ExecuteJitBlock();
  EXPECT_EQ(outcome.kind, PowerPC::GcnPort::JitBlockKind::Compiled);

  // The read-back value proves the store landed in ProcessorInterfaceManager's own
  // m_interrupt_mask (DirectRead/ComplexWrite<u32>) through a real registered MMIO handler, not
  // silently discarded, corrupted, or serviced by an uninitialized handler.
  EXPECT_EQ(system.GetPPCState().gpr[5], 0x12345678u);

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, BootAuthenticatedImageAppliesGameCubeHardwareInitMmio)
{
  std::thread cpu_thread(RunHardwareInitMmioScenario);
  cpu_thread.join();
}

// Negative control for the test above: without apply_gamecube_hardware_init (its default, false),
// the exact same hardware-register store must NOT silently succeed or no-op. It must reach the same
// real fault this issue diagnosed booting exact GMSE01 -- a SIGSEGV inside
// MMIO::WriteHandler<u32>::Write (Source/Core/Core/HW/MMIO.cpp) through an uninitialized handler
// function pointer, because without HW::Init/InitMMIO no MMIO::Mapping table exists at all.
// Asserting that the unfixed path actually crashes (not just that the fixed path works) is the
// falsifier that this change fixed a real fault instead of only exercising a happy path.
void RunNoHardwareInitMmioFaultScenario()
{
  constexpr u32 PROGRAM_ADDRESS = 0x8000A000;
  constexpr u32 LIS_R3_PI_BASE = 0x3C600C00;
  constexpr u32 ADDI_R3_R3_PI_MASK_OFFSET = 0x38633004;
  constexpr u32 LIS_R4_TEST_VALUE_HI = 0x3C801234;
  constexpr u32 ORI_R4_R4_TEST_VALUE_LO = 0x60845678;
  constexpr u32 STW_R4_0_R3 = 0x90830000;
  const std::vector<u8> program =
      BigEndianImage({LIS_R3_PI_BASE, ADDI_R3_R3_PI_MASK_OFFSET, LIS_R4_TEST_VALUE_HI,
                       ORI_R4_R4_TEST_VALUE_LO, STW_R4_0_R3, BRANCH_TO_SELF});
  const auto identity = MakeIdentity(6);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(system, identity, program,
                                                                PROGRAM_ADDRESS, PROGRAM_ADDRESS);
  ASSERT_TRUE(booted.ok) << booted.detail;

  PowerPC::GcnPort::RuntimeSession runtime(system, identity);
  // Deliberately unguarded: this must crash the process, proving the register write reaches an
  // uninitialized MMIO handler rather than silently succeeding.
  const auto outcome = runtime.ExecuteJitBlock();
  (void)outcome;
}

TEST(GcnPortRuntime, BootAuthenticatedImageWithoutHardwareInitFaultsOnMmioAccess)
{
  EXPECT_DEATH(
      {
        std::thread cpu_thread(RunNoHardwareInitMmioFaultScenario);
        cpu_thread.join();
      },
      "");
}
}  // namespace
