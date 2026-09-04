// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/GcnPortRuntime.h"

#include <algorithm>
#include <exception>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Core/PowerPC/Gekko.h"
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
}  // namespace

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

RuntimeSession::RuntimeSession(Core::System& system, ExecutionIdentity identity)
    : m_system(system), m_identity(identity)
{
  ValidateIdentity(identity);
  m_jit = dynamic_cast<JitBase*>(m_system.GetJitInterface().GetCore());
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
  const auto found = m_hooks.find(HookKey{.identity = m_identity, .address = address});
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

void RuntimeSession::RequireIdentity(const ExecutionIdentity& identity) const noexcept
{
  Require(identity == m_identity, "hook identity does not match the active image");
}

}  // namespace PowerPC::GcnPort
