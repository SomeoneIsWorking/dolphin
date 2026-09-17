// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/GcnPortRuntime.h"

#include <algorithm>
#include <exception>
#include <string_view>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace PowerPC::GcnPort
{
namespace
{
void Require(bool condition, std::string_view reason) noexcept
{
  if (!condition)
  {
    ERROR_LOG_FMT(DYNA_REC, "gcnport runtime hard fault: {}", reason);
    std::terminate();
  }
}

void ValidateIdentity(const ExecutionIdentity& identity) noexcept
{
  Require(identity.image.IsAuthenticated(), "image identity is not authenticated");
}

void ValidateKey(const HookKey& key) noexcept
{
  ValidateIdentity(key.identity);
  Require(key.address != 0 && key.address % sizeof(u32) == 0,
          "hook address is zero or is not instruction-aligned");
}

// Effective-address base of the GameCube's mapped main RAM mirror; matches the addressing already
// used by ImageIdentity/HookKey guest addresses (e.g. the existing gtest's 0x8000xxxx entries).
constexpr u32 EFFECTIVE_RAM_BASE = 0x80000000;

// At most one authenticated image may be booted per process: Config/SConfig are process-global
// singletons, not owned per Core::System, so a second concurrent boot would silently reinitialize
// state a live session still depends on.
bool g_image_booted = false;
}  // namespace

JitRefusalReason ClassifyFallbackReason(u32 instruction_hex) noexcept
{
  // Table entries pulled from Jit64_Tables.cpp / JitArm64_Tables.cpp: every opcode-31 extended
  // instruction that both backends unconditionally route to FallBackToInterpreter. mtsr/mtsrin/
  // mfsr/mfsrin/tlbie manipulate supervisor-only segment/TLB state; the rest (lock-reservation,
  // string, and external-control loads/stores, plus icbi) are ordinary user-mode instructions the
  // JIT simply never lowers, which is unsupported-instruction lowering rather than a privilege
  // boundary.
  constexpr std::array<u32, 5> kPrivilegedSubops = {210, 242, 595, 659, 306};
  const u32 opcode = (instruction_hex >> 26) & 0x3F;
  if (opcode == 31)
  {
    const u32 subop = (instruction_hex >> 1) & 0x3FF;
    if (std::ranges::find(kPrivilegedSubops, subop) != kPrivilegedSubops.end())
      return JitRefusalReason::PrivilegedInstruction;
  }
  return JitRefusalReason::UnsupportedInstruction;
}

bool ImageIdentity::IsAuthenticated() const
{
  return std::ranges::any_of(sha256, [](u8 byte) { return byte != 0; });
}

bool HookKey::IsValid() const
{
  return identity.image.IsAuthenticated() && address != 0 && address % sizeof(u32) == 0;
}

HookResult HookResult::ReturnToCaller()
{
  return {.action = HookAction::ReturnToCaller};
}

HookResult HookResult::ContinueAt(u32 address)
{
  Require(address != 0 && address % sizeof(u32) == 0,
          "continuation address is zero or is not instruction-aligned");
  return {.action = HookAction::ContinueAtAddress, .continuation = address};
}

HookResult HookResult::RunOriginalOnce()
{
  return {.action = HookAction::RunOriginalOnce};
}

BootResult BootAuthenticatedImage(Core::System& system, const ExecutionIdentity& identity,
                                   std::span<const u8> image, u32 load_address, u32 entry_point)
{
  if (!identity.image.IsAuthenticated())
    return {.ok = false, .detail = "image identity is not authenticated"};
  if (g_image_booted)
    return {.ok = false, .detail = "a booted image is already active for this process"};
  if (image.empty())
    return {.ok = false, .detail = "image is empty"};
  if (load_address % sizeof(u32) != 0 || entry_point % sizeof(u32) != 0)
    return {.ok = false, .detail = "load address or entry point is not instruction-aligned"};
  if (entry_point < load_address || entry_point - load_address >= image.size())
    return {.ok = false, .detail = "entry point is outside the loaded image"};

  Core::DeclareAsCPUThread();
  Config::Init();
  SConfig::Init();
  system.GetMemory().Init();

  const u32 ram_size = system.GetMemory().GetRamSizeReal();
  if (load_address < EFFECTIVE_RAM_BASE ||
      static_cast<u64>(load_address - EFFECTIVE_RAM_BASE) + image.size() > ram_size)
  {
    system.GetMemory().Shutdown();
    SConfig::Shutdown();
    Config::Shutdown();
    Core::UndeclareAsCPUThread();
    return {.ok = false, .detail = "image does not fit inside mapped GameCube RAM"};
  }

  system.GetCoreTiming().Init();
  system.GetCPU().Init(PowerPC::DefaultCPUCore());
  system.GetMemory().CopyToEmu(load_address, image.data(), image.size());

  auto& state = system.GetPPCState();
  state.pc = entry_point;
  state.npc = entry_point;

  g_image_booted = true;
  return {.ok = true, .detail = ""};
}

void ShutdownBootedImage(Core::System& system) noexcept
{
  if (!g_image_booted)
    return;
  system.GetCPU().Shutdown();
  system.GetCoreTiming().Shutdown();
  system.GetMemory().Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
  Core::UndeclareAsCPUThread();
  g_image_booted = false;
}

RuntimeSession::RuntimeSession(Core::System& system, ExecutionIdentity identity)
    : m_system(system), m_identity(identity)
{
  ValidateIdentity(identity);
  m_jit = m_system.GetJitInterface().GetCore();
  Require(m_jit != nullptr, "initialized host JIT backend is unavailable");
  m_jit->AttachGcnPortRuntime(*this);
}

RuntimeSession::~RuntimeSession()
{
  if (m_jit)
    m_jit->DetachGcnPortRuntime(*this);
}

void RuntimeSession::SetExecutionIdentity(ExecutionIdentity identity)
{
  ValidateIdentity(identity);
  Require(m_jit != nullptr, "host JIT backend was destroyed");
  if (identity == m_identity)
    return;
  m_identity = identity;
  m_block_awaits_first_execution.clear();
  m_jit->ClearCache();
  ++m_counters.invalidations;
}

void RuntimeSession::InstallNativeHook(HookKey key, NativeHookBinding hook)
{
  ValidateKey(key);
  RequireIdentity(key.identity);
  Require(hook.function != nullptr, "native hook callback is null");
  m_hooks.insert_or_assign(key, hook);
  InvalidateGuestCode(key.address, sizeof(u32));
}

bool RuntimeSession::RemoveNativeHook(const HookKey& key)
{
  ValidateKey(key);
  RequireIdentity(key.identity);
  if (m_hooks.erase(key) == 0)
    return false;
  InvalidateGuestCode(key.address, sizeof(u32));
  return true;
}

void RuntimeSession::InvalidateGuestCode(u32 address, u32 size)
{
  Require(m_jit != nullptr, "host JIT backend was destroyed");
  Require(size != 0, "invalidation size is zero");
  m_system.GetJitInterface().InvalidateICache(address, size, true);
  ++m_counters.invalidations;
}

bool RuntimeSession::HasNativeHook(u32 address) const
{
  return m_hooks.contains(HookKey{.identity = m_identity, .address = address});
}

JitBlockOutcome RuntimeSession::ExecuteJitBlock()
{
  auto& state = m_system.GetPPCState();
  const u32 guest_pc = state.pc;

  if (!m_jit)
    return {.kind = JitBlockKind::BackendFault,
            .guest_pc = guest_pc,
            .detail = "host JIT backend is unavailable"};

  const u64 compiled_before = m_counters.jit_blocks_compiled;
  const u64 fallback_before = m_counters.fallback_events;

  // A downcount of 1 guarantees the generated dispatcher returns to this call after completing
  // exactly the first block: every executed block decrements downcount by at least its estimated
  // cycle cost before the dispatcher rechecks it, so it cannot chain a second direct-linked block.
  state.downcount = 1;
  m_system.GetPowerPC().SingleStep();

  JitBlockOutcome outcome;
  outcome.guest_pc = guest_pc;
  if (JitBaseBlockCache* const cache = m_jit->GetBlockCache())
  {
    if (const JitBlock* const block =
            cache->GetBlockFromStartAddress(guest_pc, state.feature_flags))
    {
      outcome.instruction_count = block->originalSize;
    }
  }

  if (m_counters.fallback_events > fallback_before)
  {
    // Dolphin's JIT always compiles a whole block; an unsupported instruction inside it is executed
    // through an embedded interpreter call rather than refusing the block outright. This is reported
    // as a refusal so the framework-level fallback ledger still sees and budgets it.
    outcome.kind = JitBlockKind::Refused;
    outcome.refusal_reason = m_last_fallback_reason;
  }
  else
  {
    outcome.kind = m_counters.jit_blocks_compiled > compiled_before ? JitBlockKind::Compiled
                                                                     : JitBlockKind::CacheHit;
  }
  return outcome;
}

InterpretedBlockResult RuntimeSession::ExecuteRefusedBlock(u32 guest_pc,
                                                            u32 maximum_instruction_count)
{
  Require(maximum_instruction_count != 0, "refused-block instruction bound is zero");
  auto& state = m_system.GetPPCState();
  state.pc = guest_pc;
  state.npc = guest_pc;

  Interpreter& interpreter = m_system.GetInterpreter();
  for (u32 executed = 0; executed < maximum_instruction_count; ++executed)
    interpreter.SingleStep();

  return {.guest_pc = guest_pc, .instruction_count = maximum_instruction_count};
}

InterpretedBlockResult RuntimeSession::ExecuteDiagnosticInterpreterBlock(u32 maximum_instruction_count)
{
  Require(maximum_instruction_count != 0, "diagnostic instruction bound is zero");
  const u32 guest_pc = m_system.GetPPCState().pc;

  Interpreter& interpreter = m_system.GetInterpreter();
  for (u32 executed = 0; executed < maximum_instruction_count; ++executed)
    interpreter.SingleStep();

  return {.guest_pc = guest_pc, .instruction_count = maximum_instruction_count};
}

bool RuntimeSession::ExecuteOriginalOnce(const HookKey& key)
{
  if (!key.IsValid() || key.identity != m_identity)
    return false;
  if (!m_hooks.contains(key))
    return false;
  m_pending_original_tickets.insert(key);
  ++m_counters.original_tickets_armed;
  return true;
}

InterpretedBlockResult RuntimeSession::CallOriginalSynchronously(const HookKey& key,
                                                                  u32 maximum_instruction_count)
{
  Require(maximum_instruction_count != 0, "synchronous original-call instruction bound is zero");
  Require(key.IsValid() && key.identity == m_identity,
          "synchronous original call used a stale or mismatched identity/key");
  Require(m_hooks.contains(key),
          "synchronous original call has no active hook installed at its own key");

  PowerPCState& state = m_system.GetPPCState();
  const u32 saved_pc = state.pc;
  const u32 saved_npc = state.npc;
  const u32 return_address = state.spr[SPR_LR];

  state.pc = key.address;
  state.npc = key.address;

  Interpreter& interpreter = m_system.GetInterpreter();
  u32 executed = 0;
  bool returned = false;
  while (executed < maximum_instruction_count)
  {
    interpreter.SingleStep();
    ++executed;
    if (state.pc == return_address)
    {
      returned = true;
      break;
    }
  }
  Require(returned,
          "synchronous original call exceeded its bounded instruction budget without returning");

  state.pc = saved_pc;
  state.npc = saved_npc;

  ++m_counters.original_entries;
  ++m_counters.synchronous_original_calls;
  m_counters.synchronous_original_instructions += executed;
  return {.guest_pc = key.address, .instruction_count = executed};
}

bool RuntimeSession::RunHookFromJit(RuntimeSession* session, u32 address) noexcept
{
  if (!session)
    Require(false, "generated hook guard received a null runtime session");
  return session->RunHook(address);
}

void RuntimeSession::RecordJitBlockExecutionFromJit(RuntimeSession* session, u32 address) noexcept
{
  if (!session)
    Require(false, "generated block counter received a null runtime session");
  session->RecordJitBlockExecution(address);
}

void RuntimeSession::RecordFallbackFromJit(RuntimeSession* session, u32 address,
                                            u32 reason_value) noexcept
{
  if (!session)
    Require(false, "generated fallback counter received a null runtime session");
  session->RecordFallback(address, static_cast<JitRefusalReason>(reason_value));
}

void RuntimeSession::RecordJitBlockCompiled(u32 address)
{
  Require(m_jit != nullptr, "host JIT backend was destroyed");
  const u32 feature_flags = m_system.GetPPCState().feature_flags;
  m_block_awaits_first_execution.insert_or_assign(BlockKey{address, feature_flags}, true);
  ++m_counters.jit_blocks_compiled;
}

void RuntimeSession::JitDestroyed() noexcept
{
  m_jit = nullptr;
}

bool RuntimeSession::RunHook(u32 address) noexcept
{
  const HookKey key{.identity = m_identity, .address = address};

  // A ticket armed by ExecuteOriginalOnce suppresses the registered hook for exactly this entry,
  // without ever invoking the hook callback. Combined with ExecuteJitBlock's one-block granularity,
  // this gives a native caller a real synchronous native -> original -> native continuation.
  if (m_pending_original_tickets.erase(key) != 0)
  {
    ++m_counters.original_entries;
    return true;
  }

  const auto found = m_hooks.find(key);
  if (found == m_hooks.end())
    Require(false, "generated hook guard has no matching active hook");

  PowerPCState& state = m_system.GetPPCState();
  const HookResult result = found->second.function(found->second.context, state);
  ++m_counters.hooks_executed;
  switch (result.action)
  {
  case HookAction::ReturnToCaller:
    state.pc = state.spr[SPR_LR];
    state.npc = state.pc;
    return false;
  case HookAction::ContinueAtAddress:
    if (result.continuation == 0 || result.continuation % sizeof(u32) != 0)
      Require(false, "native hook returned an invalid continuation address");
    state.pc = result.continuation;
    state.npc = state.pc;
    return false;
  case HookAction::RunOriginalOnce:
    ++m_counters.original_entries;
    return true;
  }
  Require(false, "native hook returned an unknown action");
}

void RuntimeSession::RecordJitBlockExecution(u32 address) noexcept
{
  const BlockKey key{address, m_system.GetPPCState().feature_flags};
  const auto found = m_block_awaits_first_execution.find(key);
  if (found == m_block_awaits_first_execution.end())
    Require(false, "generated block counter has no published block record");

  ++m_counters.jit_block_executions;
  if (found->second)
  {
    found->second = false;
    ++m_counters.cold_block_executions;
  }
  else
  {
    ++m_counters.cache_hit_block_executions;
  }
}

void RuntimeSession::RecordFallback(u32 address, JitRefusalReason reason) noexcept
{
  (void)address;
  ++m_counters.fallback_events;
  ++m_counters.fallback_events_by_reason[static_cast<std::size_t>(reason)];
  m_last_fallback_reason = reason;
}

void RuntimeSession::RequireIdentity(const ExecutionIdentity& identity) const noexcept
{
  Require(identity == m_identity, "hook identity does not match the active image");
}

}  // namespace PowerPC::GcnPort
