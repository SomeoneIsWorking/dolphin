// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <compare>
#include <map>

#include "Common/CommonTypes.h"

class JitBase;

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;

namespace GcnPort
{
struct ImageIdentity
{
  std::array<u8, 32> sha256{};

  [[nodiscard]] bool IsAuthenticated() const;
  auto operator<=>(const ImageIdentity&) const = default;
};

struct ExecutionIdentity
{
  ImageIdentity image;
  u64 module_generation = 0;

  auto operator<=>(const ExecutionIdentity&) const = default;
};

struct HookKey
{
  ExecutionIdentity identity;
  u32 address = 0;

  [[nodiscard]] bool IsValid() const;
  auto operator<=>(const HookKey&) const = default;
};

enum class HookAction
{
  ReturnToCaller,
  ContinueAtAddress,
  RunOriginalOnce,
};

struct HookResult
{
  HookAction action = HookAction::ReturnToCaller;
  u32 continuation = 0;

  [[nodiscard]] static HookResult ReturnToCaller();
  [[nodiscard]] static HookResult ContinueAt(u32 address);
  [[nodiscard]] static HookResult RunOriginalOnce();
};

// Hooks run inside generated code and therefore may not unwind through the JIT ABI.
using NativeHook = HookResult (*)(void* context, PowerPCState& state) noexcept;

struct NativeHookBinding
{
  void* context = nullptr;
  NativeHook function = nullptr;
};

struct ExecutionCounters
{
  u64 jit_blocks_compiled = 0;
  u64 jit_block_executions = 0;
  u64 cold_block_executions = 0;
  u64 cache_hit_block_executions = 0;
  u64 hooks_executed = 0;
  u64 original_entries = 0;
  u64 invalidations = 0;
};

// This first embedding slice owns native-hook dispatch inside ordinary JIT blocks. A translated
// hooked instruction always contains a dynamic guard: handled hooks redispatch, while an explicit
// RunOriginalOnce result falls through to that invocation's ordinary translated instruction.
class RuntimeSession final
{
public:
  RuntimeSession(Core::System& system, ExecutionIdentity identity);
  RuntimeSession(const RuntimeSession&) = delete;
  RuntimeSession(RuntimeSession&&) = delete;
  RuntimeSession& operator=(const RuntimeSession&) = delete;
  RuntimeSession& operator=(RuntimeSession&&) = delete;
  ~RuntimeSession();

  // Construction, destruction, identity changes, and hook mutations require a stopped CPU safe
  // point. Generated hook callbacks run only on Dolphin's CPU thread.
  void SetExecutionIdentity(ExecutionIdentity identity);
  void InstallNativeHook(HookKey key, NativeHookBinding hook);
  [[nodiscard]] bool RemoveNativeHook(const HookKey& key);
  void InvalidateGuestCode(u32 address, u32 size);

  [[nodiscard]] bool HasNativeHook(u32 address) const;
  [[nodiscard]] const ExecutionIdentity& GetExecutionIdentity() const { return m_identity; }
  [[nodiscard]] const ExecutionCounters& GetExecutionCounters() const { return m_counters; }

  // Generated code calls this ABI boundary. true means fall through to the ordinary translated
  // instruction; false means the hook updated PC and the generated guard must redispatch.
  [[nodiscard]] static bool RunHookFromJit(RuntimeSession* session, u32 address) noexcept;
  static void RecordJitBlockExecutionFromJit(RuntimeSession* session, u32 address) noexcept;

  // Called by the backend only after the block has been published successfully.
  void RecordJitBlockCompiled(u32 address);

  void JitDestroyed() noexcept;

private:
  [[nodiscard]] bool RunHook(u32 address) noexcept;
  void RecordJitBlockExecution(u32 address) noexcept;
  void RequireIdentity(const ExecutionIdentity& identity) const noexcept;

  struct BlockKey
  {
    u32 address = 0;
    u32 feature_flags = 0;

    auto operator<=>(const BlockKey&) const = default;
  };

  Core::System& m_system;
  JitBase* m_jit = nullptr;
  ExecutionIdentity m_identity;
  std::map<HookKey, NativeHookBinding> m_hooks;
  std::map<BlockKey, bool> m_block_awaits_first_execution;
  ExecutionCounters m_counters;
};

}  // namespace GcnPort
}  // namespace PowerPC
