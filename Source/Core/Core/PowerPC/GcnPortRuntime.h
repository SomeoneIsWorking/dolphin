// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <compare>
#include <map>
#include <set>
#include <span>
#include <string>

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

// Reasons the fork's own generated JIT lowering declines to translate an instruction. This mirrors
// gcnport::JitRefusalReason (include/gcnport/execution_types.h) so the small outside adapter can
// convert without inventing a second policy vocabulary. Only the first two values are reachable
// from the current Jit64/JitArm64 static fallback tables (see ClassifyFallbackReason); the other two
// remain reserved for the framework-level ExecuteRefusedBlock caller, which already supplies an
// explicit reason from outside this file.
enum class JitRefusalReason : u8
{
  UnsupportedInstruction,
  UnsafeInstructionFetch,
  UnsafeHostExecution,
  PrivilegedInstruction,
};

inline constexpr std::size_t kJitRefusalReasonCount = 4;

// Classifies a raw PPC instruction word using only its static opcode/extended-opcode fields, so
// code generation can select a typed reason without depending on Gekko.h's UGeckoInstruction union
// (kept out of this header to avoid coupling the runtime facade to the instruction decoder).
[[nodiscard]] JitRefusalReason ClassifyFallbackReason(u32 instruction_hex) noexcept;

struct ExecutionCounters
{
  u64 jit_blocks_compiled = 0;
  u64 jit_block_executions = 0;
  u64 cold_block_executions = 0;
  u64 cache_hit_block_executions = 0;
  u64 hooks_executed = 0;
  u64 original_entries = 0;
  u64 invalidations = 0;
  u64 fallback_events = 0;
  std::array<u64, kJitRefusalReasonCount> fallback_events_by_reason{};
  u64 original_tickets_armed = 0;
};

enum class JitBlockKind
{
  Compiled,
  CacheHit,
  Refused,
  BackendFault,
};

// The public one-block execution result. Exactly one call to RuntimeSession::ExecuteJitBlock
// corresponds to exactly one observable guest basic block (see ExecuteJitBlock for the mechanism).
struct JitBlockOutcome
{
  JitBlockKind kind = JitBlockKind::Compiled;
  u32 guest_pc = 0;
  u32 instruction_count = 0;
  JitRefusalReason refusal_reason = JitRefusalReason::UnsupportedInstruction;
  std::string detail;
};

struct InterpretedBlockResult
{
  u32 guest_pc = 0;
  u32 instruction_count = 0;
};

struct BootResult
{
  bool ok = false;
  std::string detail;
};

// Boots the minimal Dolphin subsystems needed to execute guest code (config, memory, core timing,
// the CPU/JIT core) and loads a caller-authenticated redistributable image at `load_address`,
// setting the initial program counter to `entry_point`. `identity` must already carry a
// caller-computed digest (ImageIdentity::IsAuthenticated()); this function does not itself recompute
// or verify the hash, matching the existing hook/original authentication convention in this file
// where authentication is an upstream, caller-attested fact.
//
// This is deliberately scoped to a raw in-memory image rather than a full BootParameters/DVD
// pipeline: the first embedding slice boots a small redistributable PPC test program, never a game
// disc (see docs/dolphin-embedding-contract.md). A title's real GameCube disc boot is a separate,
// later adapter that supplies its own validated image bytes through this same entry point.
//
// The process may boot at most one image at a time; a second call before ShutdownBootedImage
// returns a failed BootResult instead of silently reinitializing global Dolphin state.
[[nodiscard]] BootResult BootAuthenticatedImage(Core::System& system,
                                                 const ExecutionIdentity& identity,
                                                 std::span<const u8> image, u32 load_address,
                                                 u32 entry_point);

// Reverses BootAuthenticatedImage. Must be called before the process may boot another image.
void ShutdownBootedImage(Core::System& system) noexcept;

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

  // Executes exactly one observable guest basic block from the live PC through the ordinary
  // Jit64/JitArm64 dispatcher: it forces the PowerPC downcount to a value smaller than any real
  // block's cycle cost immediately before entering the dispatcher, so the generated dispatch loop
  // returns to this call after completing the first block instead of chaining further direct-linked
  // blocks. The block is compiled on a cache miss and reported as CacheHit on any later entry with
  // the same address and feature flags. An unavailable JIT is a JitBlockKind::BackendFault, never a
  // refusal.
  [[nodiscard]] JitBlockOutcome ExecuteJitBlock();

  // Executes the given already-refused PC through Dolphin's ordinary interpreter for at most
  // `maximum_instruction_count` guest instructions, then returns without resuming JIT dispatch
  // itself (the caller resumes dispatch by calling ExecuteJitBlock next, per the dispatch ordering
  // in docs/dolphin-embedding-contract.md).
  [[nodiscard]] InterpretedBlockResult ExecuteRefusedBlock(u32 guest_pc,
                                                            u32 maximum_instruction_count);

  // Executes through the plain interpreter for at most `maximum_instruction_count` instructions
  // without ever consulting the gameplay JIT selector. This is a distinct, explicitly diagnostic
  // entry point and must not be reachable from ExecuteJitBlock/ExecuteRefusedBlock.
  [[nodiscard]] InterpretedBlockResult ExecuteDiagnosticInterpreterBlock(u32 maximum_instruction_count);

  // Arms a one-shot ticket so the NEXT dispatch that reaches `key.address` under `key.identity`
  // falls through to the ordinary translated body instead of invoking any registered native hook,
  // without requiring the hook callback itself to return HookAction::RunOriginalOnce. Combined with
  // ExecuteJitBlock's one-block granularity, a native caller gets a real synchronous
  // native -> original -> native continuation: arm the ticket, call ExecuteJitBlock to run exactly
  // the original body once, then resume native code. Returns false if the key is invalid or has no
  // installed hook to suppress.
  [[nodiscard]] bool ExecuteOriginalOnce(const HookKey& key);

  // Generated code calls this ABI boundary. true means fall through to the ordinary translated
  // instruction; false means the hook updated PC and the generated guard must redispatch.
  [[nodiscard]] static bool RunHookFromJit(RuntimeSession* session, u32 address) noexcept;
  static void RecordJitBlockExecutionFromJit(RuntimeSession* session, u32 address) noexcept;
  static void RecordFallbackFromJit(RuntimeSession* session, u32 address, u32 reason_value) noexcept;

  // Called by the backend only after the block has been published successfully.
  void RecordJitBlockCompiled(u32 address);

  void JitDestroyed() noexcept;

private:
  [[nodiscard]] bool RunHook(u32 address) noexcept;
  void RecordJitBlockExecution(u32 address) noexcept;
  void RecordFallback(u32 address, JitRefusalReason reason) noexcept;
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
  std::set<HookKey> m_pending_original_tickets;
  ExecutionCounters m_counters;
  JitRefusalReason m_last_fallback_reason = JitRefusalReason::UnsupportedInstruction;
};

}  // namespace GcnPort
}  // namespace PowerPC
