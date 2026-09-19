// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/LogManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/GcnPortRuntime.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoBackendBase.h"

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

std::vector<u8> BigEndianImage(std::span<const u32> words)
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

std::vector<u8> BigEndianImage(std::initializer_list<u32> words)
{
  return BigEndianImage(std::span<const u32>(words.begin(), words.size()));
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
  // ExecuteOriginalOnce proof. Dolphin's analyzer may merge several passes of a tight reflexive
  // loop into one compiled unit (observed empirically: a plain two-instruction self-loop's
  // originalSize came back as a multiple of 2, not exactly 2), and the native-hook guard is emitted
  // at the top of the generated code, so a merged loop body re-evaluates that guard once per
  // internal pass within a single ExecuteJitBlock() call. A one-shot ticket armed by
  // ExecuteOriginalOnce would then be consumed by the first internal pass and leak a real hook
  // invocation on the second. A block with no back-edge to itself has exactly one guard evaluation
  // per compiled-block execution, so the hook proof uses HOOK_ADDRESS ->
  // PROGRAM_LANDING_PAD_ADDRESS (itself a harmless branch-to-self stopping point, never hooked)
  // instead of looping back into the hooked address.
  std::vector<u8> program = BigEndianImage({ADDI_R3_R3_1, BRANCH_BACK_ONE_INSTRUCTION});
  program.resize(HOOK_ADDRESS - PROGRAM_ADDRESS, 0);
  // addi r3,r3,1 ; b PROGRAM_LANDING_PAD_ADDRESS (unconditional forward branch, no back edge).
  constexpr u32 HOOK_BRANCH_DISPLACEMENT =
      PROGRAM_LANDING_PAD_ADDRESS - (HOOK_ADDRESS + sizeof(u32));
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
  EXPECT_FALSE(PowerPC::GcnPort::BootAuthenticatedImage(system, identity, program, PROGRAM_ADDRESS,
                                                        PROGRAM_ADDRESS)
                   .ok);

  {
    PowerPC::GcnPort::RuntimeSession runtime(system, identity);
    auto& state = system.GetPPCState();
    state.gpr[3] = 0;

    const auto first = runtime.ExecuteJitBlock();
    EXPECT_EQ(first.kind, PowerPC::GcnPort::JitBlockKind::Compiled);
    EXPECT_EQ(first.guest_pc, PROGRAM_ADDRESS);
    // Dolphin's analyzer may merge more than one pass of a tight two-instruction reflexive loop
    // into a single compiled block; only a multiple of the 2-instruction body is guaranteed.
    EXPECT_GE(first.instruction_count, 2u);
    EXPECT_EQ(first.instruction_count % 2, 0u);
    EXPECT_GE(state.gpr[3], 1u);

    const auto after_first = runtime.GetExecutionCounters();
    EXPECT_EQ(after_first.jit_blocks_compiled, 1u);
    EXPECT_EQ(after_first.fallback_events, 0u);

    // A second entry at the same address/feature flags is a cache hit, not a recompile: the block
    // branches back to its own already-published start, so there is never a second,
    // not-yet-compiled successor address for this shape.
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
        // this minimal harness (ReturnToCaller would jump to it), and HOOK_ADDRESS's own body
        // already falls through toward that same landing pad on the unhooked path.
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

// Proves BootAuthenticatedImage's apply_os_init option actually installs the exact retail
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
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true});
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

  // The registers are only half of what BS2 leaves behind. The SDK reads these low-memory globals
  // back by fixed address, and derives OS_TIMER_CLOCK from the bus clock at 0x800000F8 -- so
  // leaving that one zero does not fail loudly, it silently corrupts every tick and time conversion
  // a title makes. CBoot::SetupGCMemory's own constants (Boot_BS2Emu.cpp) are the reference.
  auto& memory = system.GetMemory();
  EXPECT_EQ(memory.Read_U32(0x80000020), 0x0D15EA5Eu);              // booted from bootrom
  EXPECT_EQ(memory.Read_U32(0x80000028), memory.GetRamSizeReal());  // physical memory size
  EXPECT_EQ(memory.Read_U32(0x800000d0), 0x01000000u);              // ARAM size
  EXPECT_EQ(memory.Read_U32(0x800000F8), 0x09a7ec80u);              // bus clock speed
  EXPECT_EQ(memory.Read_U32(0x800000FC), 0x1cf7c580u);              // CPU clock speed
  EXPECT_EQ(memory.Read_U32(0x80000300), 0x4c000064u);              // default DSI handler: rfi
  EXPECT_EQ(memory.Read_U32(0x80000800), 0x4c000064u);              // default FPU handler: rfi
  EXPECT_EQ(memory.Read_U32(0x80000C00), 0x4c000064u);              // default syscall handler: rfi

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, BootAuthenticatedImageAppliesGameCubeOsInitRegisters)
{
  std::thread cpu_thread(RunGameCubeOsInitScenario);
  cpu_thread.join();
}

// The default (apply_os_init left false, the option's default value) must be unchanged: a
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
  // The low-memory OS globals are part of the same opt-in, so a caller that did not ask for
  // GameCube OS init must not find BS2's values in memory either -- otherwise the assertions above
  // would pass against a boot that quietly applied half of it.
  auto& memory = system.GetMemory();
  EXPECT_EQ(memory.Read_U32(0x80000020), 0u);
  EXPECT_EQ(memory.Read_U32(0x80000028), 0u);
  EXPECT_EQ(memory.Read_U32(0x800000F8), 0u);

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
//   * a block must hold more than one instruction        -- fails under MAIN_ENABLE_DEBUGGING,
//   which
//                                                           also bounds per block but drives the
//                                                           analyzer into single-instruction
//                                                           blocks;
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
  // iterations the analyzer folds into one block is its own decision -- it follows the
  // unconditional backward branch, and has been observed forming both a 4-instruction and a
  // 12-instruction block for this body -- so the assertions below are written against that ratio
  // rather than any fixed block size, which is not part of this API's contract.
  constexpr u32 BRANCH_BACK_THREE_INSTRUCTIONS = 0x4bfffff4;
  constexpr u32 ADDS_PER_ITERATION = 3;
  constexpr u32 INSTRUCTIONS_PER_ITERATION = 4;
  const std::vector<u8> program =
      BigEndianImage({ADDI_R3_R3_1, ADDI_R3_R3_1, ADDI_R3_R3_1, BRANCH_BACK_THREE_INSTRUCTIONS});
  const auto identity = MakeIdentity(5);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true});
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
    // per block, but it additionally drives the analyzer into single-instruction blocks; this is
    // the assertion that tells the two apart.
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

// Dolphin's subsystems reach some of their owners through raw singletons they never check, so a
// boot that leaves one absent does not fail -- it crashes later, in whichever subsystem touches it
// first. Two of those are covered here.
//
// The log manager is reached through a raw pointer by, among others,
// FileMonitor::FileLogger::Log, which DVDThread::ProcessReadRequest calls on every disc FILE read:
// measured as a SIGSEGV on the DVD thread with LogManager::IsEnabled's `this` at null, the first
// time a boot read a file rather than the raw disc header. It is required by every boot, which is
// why it is checked here on the minimal one that brings no hardware up at all.
//
// The controller interface is required only by a hardware-init boot: SerialInterfaceManager's
// periodic poll calls g_controller_interface.UpdateInput() unconditionally, before and
// independently of asking any SI channel for data, and that call asserts on m_is_init. Forcing
// every channel to SIDEVICE_NONE is not sufficient, because the poll still runs -- an SI poll with
// nothing plugged in is what real hardware does.
//
// Both must be handed back on shutdown, so a second boot in the same process starts from the same
// state rather than leaking or double-initializing.
void RunHeadlessSubsystemOwnershipScenario()
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

  // Even the minimal boot, which brings no hardware up, must leave the log manager present.
  const auto minimal = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true});
  ASSERT_TRUE(minimal.ok) << minimal.detail;
  EXPECT_NE(Common::Log::LogManager::GetInstance(), nullptr)
      << "a boot left Dolphin's unchecked log-manager singleton absent";
  PowerPC::GcnPort::ShutdownBootedImage(system);
  EXPECT_EQ(Common::Log::LogManager::GetInstance(), nullptr)
      << "ShutdownBootedImage leaked a log manager into the next boot";

  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true, .apply_hardware_init = true});
  ASSERT_TRUE(booted.ok) << booted.detail;

  EXPECT_TRUE(g_controller_interface.IsInit())
      << "a hardware-init boot left SerialInterfaceManager's poll without an initialized "
         "ControllerInterface to call UpdateInput() on";

  PowerPC::GcnPort::ShutdownBootedImage(system);

  EXPECT_FALSE(g_controller_interface.IsInit())
      << "ShutdownBootedImage leaked an initialized ControllerInterface into the next boot";

  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, BootOwnsTheSubsystemsDolphinDereferencesUnchecked)
{
  std::thread cpu_thread(RunHeadlessSubsystemOwnershipScenario);
  cpu_thread.join();
}

// ExecuteJitBlocks exists because one block per host call costs a host round trip per block:
// against exact GMSE01 it measured ~180,000 blocks/second, i.e. ~740,000 guest instructions/second,
// far under GameCube speed. A batch lifts gcnport's one-block slice cap so the dispatcher chains
// direct-linked blocks natively.
//
// The contract this pins is that speed is bought without giving up measurability or correctness:
// every block still reports itself through the JIT's own per-block callback, so the counters
// advance by exactly the number of blocks the batch claims; a batch runs far more blocks per call
// than the one-block path; and the one-block path still works afterwards, proving the lifted cap
// was restored rather than leaked.
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
  const std::vector<u8> program =
      BigEndianImage({ADDI_R3_R3_1, ADDI_R3_R3_1, ADDI_R3_R3_1, BRANCH_BACK_THREE_INSTRUCTIONS});
  const auto identity = MakeIdentity(7);

  Core::System& system = Core::System::GetInstance();
  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true});
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

// A disc is a consumer-supplied path, never anything gcnport ships, so the cases this can check
// without a game image are exactly the refusals -- and those are the ones that matter, because each
// one would otherwise be a boot that succeeds with the disc silently absent and fails much later
// inside the title's own disc-error path, where the cause is no longer visible.
void RunDiscRefusalScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x8000c000;
  const std::vector<u8> program = BigEndianImage({BRANCH_TO_SELF});
  const auto identity = MakeIdentity(8);
  Core::System& system = Core::System::GetInstance();

  // Asking for a disc without the hardware init that owns DVDInterface has no correct answer, so it
  // must be refused rather than quietly booting with no drive to mount into. Refused before any
  // global Dolphin state is touched, so a later boot in this same process still works.
  const auto no_hardware = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true,
                                            .disc_image_path = "/nonexistent.iso"});
  EXPECT_FALSE(no_hardware.ok);
  EXPECT_NE(no_hardware.detail.find("apply_hardware_init"), std::string::npos)
      << no_hardware.detail;

  // A path that is not a readable disc image is a failed boot, not a boot with no disc: the caller
  // named a disc and did not get one.
  const std::string missing = profile_path + "/not-a-disc.iso";
  const auto unreadable = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{
          .apply_os_init = true, .apply_hardware_init = true, .disc_image_path = missing});
  EXPECT_FALSE(unreadable.ok);
  EXPECT_NE(unreadable.detail.find(missing), std::string::npos) << unreadable.detail;

  // An apploader lives on a disc and is guest code, so asking for one without a disc to read it
  // from, or without the address translation it runs under, has no correct answer either. Both are
  // refused before any global state is touched, which is what lets the ordinary boot below succeed.
  const auto no_disc = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{
          .apply_os_init = true, .apply_hardware_init = true, .run_apploader = true});
  EXPECT_FALSE(no_disc.ok);
  EXPECT_NE(no_disc.detail.find("disc"), std::string::npos) << no_disc.detail;

  const auto no_os_init = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{
          .apply_hardware_init = true, .disc_image_path = missing, .run_apploader = true});
  EXPECT_FALSE(no_os_init.ok);
  EXPECT_NE(no_os_init.detail.find("apply_os_init"), std::string::npos) << no_os_init.detail;

  // That failed boot must have handed back every global it took, or this ordinary boot cannot run.
  const auto recovered = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true});
  ASSERT_TRUE(recovered.ok) << "a refused disc boot leaked global state: " << recovered.detail;

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

// Writes a minimal but genuine GameCube disc image: the 0x20-byte header a drive returns for
// DVDReadDiscID, carrying the game ID, plus the big-endian region code in bi2.bin at 0x458 that
// DiscIO::VolumeGC::GetRegion reads. No title data and no apploader, so it stays asset-free and
// CI-safe while still being a real disc as far as DiscIO::CreateDisc is concerned.
// The bi2.bin region codes the writer below stamps, in the order DiscIO::VolumeGC::GetRegion reads
// them. They belong beside the writer rather than inside one scenario, because more than one boot
// depends on which console a disc builds.
constexpr u32 REGION_CODE_NTSC_U = 1;
constexpr u32 REGION_CODE_PAL = 2;

bool WriteSyntheticGameCubeDisc(const std::string& path, const char (&game_id)[7], u32 region_code)
{
  constexpr u32 GAMECUBE_DISC_MAGIC = 0xc2339f3d;
  constexpr size_t REGION_CODE_OFFSET = 0x458;
  constexpr size_t DISC_HEADER_BYTES = REGION_CODE_OFFSET + sizeof(u32);

  std::vector<u8> image(DISC_HEADER_BYTES, 0);
  std::memcpy(image.data(), game_id, 6);
  const auto write_big_endian = [&image](size_t offset, u32 value) {
    image[offset] = static_cast<u8>(value >> 24);
    image[offset + 1] = static_cast<u8>(value >> 16);
    image[offset + 2] = static_cast<u8>(value >> 8);
    image[offset + 3] = static_cast<u8>(value);
  };
  write_big_endian(0x1c, GAMECUBE_DISC_MAGIC);
  write_big_endian(REGION_CODE_OFFSET, region_code);

  File::IOFile file(path, "wb");
  return file.WriteBytes(image.data(), image.size());
}

// A disc does not just supply data to a booted console -- it decides what console to build. Two
// things a frontend does before Core sees a title, and this adapter must do itself:
//
//   * the disc's region, which CBoot::SetupGCMemory publishes at 0x800000CC and EmulatedBS2_GC also
//     reads for the IPL font encoding. SConfig leaves it Unknown, which DiscIO::IsNTSC reads as
//     PAL. Measured with a US retail disc: the title built a PAL render mode (xfbHeight 530) while
//     its own framebuffer allocation was the NTSC-sized 640x528x2, so its display copy ran past the
//     end of that block and destroyed the live object the heap had placed immediately after it.
//
//   * Dolphin's shipped per-title settings in Sys/GameSettings, which carry the corrections whose
//     absence breaks specific titles the same way.
//
// Both must be in place before the video backend and the rest of the hardware come up, because
// Config::AddLayer's change notification reaches VideoCommon through CPUThreadConfigCallback, which
// a frontend's CPU thread pumps and this adapter's caller-driven dispatch does not.
void RunDiscConfiguresConsoleScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x8000e000;
  constexpr u32 GUEST_VIDEO_FORMAT = 0x800000cc;
  constexpr u32 VIDEO_FORMAT_NTSC = 0;
  constexpr u32 VIDEO_FORMAT_PAL = 1;
  const std::vector<u8> program = BigEndianImage({BRANCH_TO_SELF});
  Core::System& system = Core::System::GetInstance();

  // Two discs that differ only in their country code, booted the same way. One disc alone could not
  // tell "the region was taken from the disc" from "the region happened to already be right".
  const std::string ntsc_path = profile_path + "/synthetic-ntsc.iso";
  const std::string pal_path = profile_path + "/synthetic-pal.iso";
  ASSERT_TRUE(WriteSyntheticGameCubeDisc(ntsc_path, "GMSE01", REGION_CODE_NTSC_U));
  ASSERT_TRUE(WriteSyntheticGameCubeDisc(pal_path, "GMSP01", REGION_CODE_PAL));

  const auto ntsc = PowerPC::GcnPort::BootAuthenticatedImage(
      system, MakeIdentity(10), program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{
          .apply_os_init = true, .apply_hardware_init = true, .disc_image_path = ntsc_path});
  ASSERT_TRUE(ntsc.ok) << ntsc.detail;

  EXPECT_EQ(SConfig::GetInstance().m_region, DiscIO::Region::NTSC_U);
  EXPECT_EQ(system.GetMemory().Read_U32(GUEST_VIDEO_FORMAT), VIDEO_FORMAT_NTSC);

  // The shipped per-title layer, asserted by its presence rather than by any one setting inside it,
  // so this stays a test of the mechanism and not of whatever Dolphin currently ships for one ID.
  EXPECT_NE(Config::GetLayer(Config::LayerType::GlobalGame), nullptr);
  // The user's own per-title INI is deliberately never applied: an embedded boot must not depend on
  // what the person running it last set in Dolphin.
  EXPECT_EQ(Config::GetLayer(Config::LayerType::LocalGame), nullptr);

  PowerPC::GcnPort::ShutdownBootedImage(system);
  EXPECT_EQ(Config::GetLayer(Config::LayerType::GlobalGame), nullptr);

  const auto pal = PowerPC::GcnPort::BootAuthenticatedImage(
      system, MakeIdentity(11), program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{
          .apply_os_init = true, .apply_hardware_init = true, .disc_image_path = pal_path});
  ASSERT_TRUE(pal.ok) << pal.detail;

  EXPECT_EQ(SConfig::GetInstance().m_region, DiscIO::Region::PAL);
  EXPECT_EQ(system.GetMemory().Read_U32(GUEST_VIDEO_FORMAT), VIDEO_FORMAT_PAL);

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

// A memory card is storage policy, which the empty-slot default deliberately does not own. Three
// things are worth proving without a game image: that naming one attaches a real card whose file
// the device creates and formats, and that each way of naming one the boot cannot honour is
// refused rather than booted with the slot silently still empty -- which a title reports much
// later, in its own words, as there being no memory card inserted.
void RunMemoryCardScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }
  UICommon::SetUserDirectory(profile_path);

  constexpr u32 PROGRAM_ADDRESS = 0x8000c000;
  const std::vector<u8> program = BigEndianImage({BRANCH_TO_SELF});
  const auto identity = MakeIdentity(8);
  Core::System& system = Core::System::GetInstance();

  const std::string card_directory = profile_path + "/cards";
  File::CreateFullPath(card_directory + "/");
  const std::string card_path = card_directory + "/gamecube.raw";
  const std::string disc_path = profile_path + "/ntsc-u.gcm";
  ASSERT_TRUE(WriteSyntheticGameCubeDisc(disc_path, "GMSE01", REGION_CODE_NTSC_U));

  const auto no_hardware = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true,
                                            .disc_image_path = disc_path,
                                            .memory_card_slot_a_path = card_path});
  EXPECT_FALSE(no_hardware.ok);
  EXPECT_NE(no_hardware.detail.find("apply_hardware_init"), std::string::npos)
      << no_hardware.detail;

  // Without a disc there is no region, and Dolphin names a card file by the region: a boot that
  // attached one anyway would reach GetDirectoryForRegion's unreachable default.
  const auto no_disc = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true,
                                            .apply_hardware_init = true,
                                            .memory_card_slot_a_path = card_path});
  EXPECT_FALSE(no_disc.ok);
  EXPECT_NE(no_disc.detail.find("region"), std::string::npos) << no_disc.detail;

  EXPECT_EQ(File::ScanDirectoryTree(card_directory, false).children.size(), 0u)
      << "a refused boot created a card file for a slot it never attached";

  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true,
                                            .apply_hardware_init = true,
                                            .disc_image_path = disc_path,
                                            .memory_card_slot_a_path = card_path});
  ASSERT_TRUE(booted.ok) << booted.detail;
  EXPECT_EQ(Config::Get(Config::MAIN_SLOT_A), ExpansionInterface::EXIDeviceType::MemoryCard);
  // The negative that makes the positive mean something: slot B was named by nobody, so it must
  // still be the empty slot the default forces rather than a second card attached along with it.
  EXPECT_EQ(Config::Get(Config::MAIN_SLOT_B), ExpansionInterface::EXIDeviceType::None);

  PowerPC::GcnPort::ShutdownBootedImage(system);

  // The device writes its card back on the way out, so exactly one card now exists in the
  // directory the caller named, carrying a formatted card rather than an empty placeholder. Its
  // exact filename is Dolphin's to choose -- GetMemcardPath appends the region and the card's free
  // block count -- so this asserts what the caller can actually rely on: one file, of a real
  // card's size, where the caller asked for it.
  const File::FSTEntry cards = File::ScanDirectoryTree(card_directory, false);
  ASSERT_EQ(cards.children.size(), 1u);
  // The smallest card the GameCube has is 4 Mbit, which is 512 KiB on disk; anything shorter is a
  // placeholder rather than a card.
  constexpr u64 SMALLEST_GAMECUBE_CARD_BYTES = 512 * 1024;
  EXPECT_GE(cards.children.front().size, SMALLEST_GAMECUBE_CARD_BYTES);
  EXPECT_TRUE(cards.children.front().virtualName.starts_with("gamecube."))
      << cards.children.front().virtualName;

  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, MemoryCardAttachesOnlyWhenTheConsumerNamesOne)
{
  std::thread cpu_thread(RunMemoryCardScenario);
  cpu_thread.join();
}

TEST(GcnPortRuntime, DiscImageRefusalsNeverBootSilentlyWithoutTheDisc)
{
  std::thread cpu_thread(RunDiscRefusalScenario);
  cpu_thread.join();
}

TEST(GcnPortRuntime, DiscRegionAndShippedSettingsConfigureTheConsole)
{
  std::thread cpu_thread(RunDiscConfiguresConsoleScenario);
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
// A fallback total says the JIT declined something; it cannot say whether that was one instruction
// in a hot loop or thousands of distinct routines, and those want opposite work. The session
// therefore keeps per-address counts -- bounded, because a diagnostic must not grow without limit
// inside a long run, and audited, because a list that silently stopped growing would be
// indistinguishable from one that was complete.
//
// Driven through the shipping path: Jit64::FallBackToInterpreter is what emits the record, so the
// program is a run of real opcode-31 fallback instructions and the counts come from executing it.
void RunFallbackSiteAccountingScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x8000f000;
  constexpr u32 MFSR_R4_0 = 0x7c8004a6;  // opcode 31, subop 595: on Jit64's fallback list.
  // More distinct fallback addresses than the session will track, so the truncation path is the
  // one under test rather than a case that happens to fit.
  constexpr std::size_t FALLBACK_INSTRUCTIONS = PowerPC::GcnPort::kMaxTrackedFallbackSites + 8;

  std::vector<u32> instructions(FALLBACK_INSTRUCTIONS, MFSR_R4_0);
  instructions.push_back(BRANCH_TO_SELF);
  const std::vector<u8> program = BigEndianImage(instructions);
  const auto identity = MakeIdentity(12);
  Core::System& system = Core::System::GetInstance();

  ASSERT_TRUE(PowerPC::GcnPort::BootAuthenticatedImage(system, identity, program, PROGRAM_ADDRESS,
                                                       PROGRAM_ADDRESS)
                  .ok);
  {
    PowerPC::GcnPort::RuntimeSession runtime(system, identity);
    constexpr u64 BLOCK_BUDGET = 4096;
    const auto batch = runtime.ExecuteJitBlocks(BLOCK_BUDGET);
    ASSERT_FALSE(batch.backend_fault) << batch.detail;
    EXPECT_GT(batch.blocks_executed, 0u);

    const auto& counters = runtime.GetExecutionCounters();
    const auto& sites = runtime.GetFallbackSites();

    ASSERT_GT(counters.fallback_events, 0u);
    // mfsr is supervisor-only, so every one of these is a PrivilegedInstruction refusal -- a
    // non-default reason, which is what makes "counted under the right reason" a real assertion
    // rather than one the zero-initialised enum would satisfy on its own.
    EXPECT_EQ(counters.fallback_events_by_reason[static_cast<std::size_t>(
                  PowerPC::GcnPort::JitRefusalReason::PrivilegedInstruction)],
              counters.fallback_events);
    EXPECT_EQ(counters.fallback_events_by_reason[static_cast<std::size_t>(
                  PowerPC::GcnPort::JitRefusalReason::UnsupportedInstruction)],
              0u);

    // Bounded, and honest about it: the list stops at the cap and the overflow is counted rather
    // than dropped, so a truncated view can always be told from a complete one.
    EXPECT_EQ(sites.size(), PowerPC::GcnPort::kMaxTrackedFallbackSites);
    EXPECT_GT(counters.fallback_sites_not_tracked, 0u);

    // Every event landed somewhere. This is what makes the per-site view a measurement rather than
    // a sample: tracked counts plus untracked events must reconstruct the total exactly.
    u64 tracked_events = 0;
    for (const auto& [address, site] : sites)
    {
      tracked_events += site.events;
      EXPECT_EQ(site.reason, PowerPC::GcnPort::JitRefusalReason::PrivilegedInstruction);
      EXPECT_GE(address, PROGRAM_ADDRESS);
      EXPECT_LT(address, PROGRAM_ADDRESS + program.size());
    }
    EXPECT_EQ(tracked_events + counters.fallback_sites_not_tracked, counters.fallback_events);

    // The reason names are owned beside the reason, so a consumer reporting fallbacks never has to
    // restate them.
    EXPECT_STREQ(
        PowerPC::GcnPort::ToString(PowerPC::GcnPort::JitRefusalReason::PrivilegedInstruction),
        "privileged_instruction");
  }

  PowerPC::GcnPort::ShutdownBootedImage(system);
  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, FallbackAccountingNamesItsSitesAndReportsItsOwnTruncation)
{
  std::thread cpu_thread(RunFallbackSiteAccountingScenario);
  cpu_thread.join();
}

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

// Proves BootAuthenticatedImage's apply_hardware_init option actually builds a working
// MMIO::Mapping handler table (HW::Init -> MemoryManager::InitMMIO), using a small synthetic
// program that stores to, then reads back, the real GameCube ProcessorInterface hardware register
// at physical/effective address 0x0C003004 (PI_INTERRUPT_MASK; see ProcessorInterface.cpp's
// RegisterMMIO) -- the exact register and address a real GMSE01 boot's own __init_hardware code
// faulted on before this flag existed (docs/dolphin-embedding-contract.md). Boots in real
// addressing mode (apply_os_init left at its default false) so the effective address the
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
      PowerPC::GcnPort::GameCubeBootOptions{.apply_hardware_init = true});
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

// Negative control for the test above: without apply_hardware_init (its default, false),
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

// HW::Init builds the MMIO table and SystemTimers::Init schedules the periodic events, but neither
// gives those devices a consumer: the GP FIFO has no reader and the DSP object is constructed
// without booting its ucode. Against exact GMSE01 that combination is invisible from outside -- the
// title reaches the SDK's idle loop with every thread blocked while VI interrupts keep arriving at
// 60 Hz, which looks identical to a title that is simply waiting on disc data.
//
// The contract this pins is that apply_media_init brings those consumers up headlessly, that guest
// execution still works with a GPU thread declared on the calling thread, that shutdown hands them
// back so a second boot in the same process starts clean, and that asking for media without the
// hardware that owns it is refused rather than half-applied.
void RunHeadlessMediaDevicesScenario()
{
  const std::string profile_path = File::CreateTempDir();
  if (profile_path.empty())
  {
    ADD_FAILURE() << "failed to create an isolated Dolphin user directory";
    return;
  }

  constexpr u32 PROGRAM_ADDRESS = 0x8000d000;
  constexpr u32 BRANCH_BACK_THREE_INSTRUCTIONS = 0x4bfffff4;
  const std::vector<u8> program =
      BigEndianImage({ADDI_R3_R3_1, ADDI_R3_R3_1, ADDI_R3_R3_1, BRANCH_BACK_THREE_INSTRUCTIONS});
  const auto identity = MakeIdentity(9);

  Core::System& system = Core::System::GetInstance();

  const auto without_hardware = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true, .apply_media_init = true});
  EXPECT_FALSE(without_hardware.ok)
      << "media init was accepted without the hardware devices it drives";
  EXPECT_EQ(g_gfx, nullptr) << "a refused boot still brought a video backend up";

  const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
      system, identity, program, PROGRAM_ADDRESS, PROGRAM_ADDRESS,
      PowerPC::GcnPort::GameCubeBootOptions{
          .apply_os_init = true, .apply_hardware_init = true, .apply_media_init = true});
  ASSERT_TRUE(booted.ok) << booted.detail;

  EXPECT_NE(g_video_backend, nullptr) << "no video backend was selected";
  EXPECT_NE(g_gfx, nullptr) << "the GP FIFO was left without a consumer";
  EXPECT_NE(g_presenter, nullptr) << "the video backend was selected but never initialized";

  // Execution has to keep working with the calling thread declared as the GPU thread; a batch that
  // retires nothing would mean the media bring-up broke dispatch rather than completing it.
  PowerPC::GcnPort::RuntimeSession runtime(system, identity);
  constexpr u64 REQUESTED_BLOCKS = 1000;
  const auto batch = runtime.ExecuteJitBlocks(REQUESTED_BLOCKS);
  EXPECT_FALSE(batch.backend_fault) << batch.detail;
  EXPECT_GE(batch.blocks_executed, REQUESTED_BLOCKS)
      << "guest execution stopped once a video backend owned the FIFO";

  PowerPC::GcnPort::ShutdownBootedImage(system);

  EXPECT_EQ(g_gfx, nullptr) << "ShutdownBootedImage leaked a video backend into the next boot";

  File::DeleteDirRecursively(profile_path);
}

TEST(GcnPortRuntime, MediaInitOwnsHeadlessVideoAndDspOrRefuses)
{
  std::thread cpu_thread(RunHeadlessMediaDevicesScenario);
  cpu_thread.join();
}
