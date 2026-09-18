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
// from the current Jit64/JitArm64 static fallback tables (see ClassifyFallbackReason); the other
// two remain reserved for the framework-level ExecuteRefusedBlock caller, which already supplies an
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

// The reason's name, owned here beside the reason itself. A fallback total on its own says only
// that the JIT declined something; the reason is what says whether that is an unimplemented opcode
// or a block the runtime refused to fetch, so any consumer reporting fallbacks needs this.
[[nodiscard]] const char* ToString(JitRefusalReason reason) noexcept;

// One guest address the JIT declined, with how often and why. A site keeps the reason it was first
// classified with; a second reason at the same address would mean the classifier is not a function
// of the instruction, which it is.
struct FallbackSite
{
  u64 events = 0;
  JitRefusalReason reason = JitRefusalReason::UnsupportedInstruction;
};

inline constexpr std::size_t kMaxTrackedFallbackSites = 256;

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
  // Fallbacks at addresses beyond kMaxTrackedFallbackSites distinct sites. Non-zero means
  // GetFallbackSites() is a truncated view, never that those fallbacks did not happen.
  u64 fallback_sites_not_tracked = 0;
  u64 original_tickets_armed = 0;
  // Distinct from original_entries (which also counts the ExecuteOriginalOnce ticket path and the
  // in-callback HookAction::RunOriginalOnce path): this counts only calls made through
  // CallOriginalSynchronously, i.e. a native hook calling the original guest body as a subroutine
  // and getting control back in the same callback frame.
  u64 synchronous_original_calls = 0;
  u64 synchronous_original_instructions = 0;
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

// The public batched execution result. A batch deliberately does NOT bound itself to one block: it
// lifts the one-block slice cap so the generated dispatcher chains direct-linked blocks natively,
// which is the only way this runtime reaches usable guest speed. Every block still reports itself
// through the JIT's own per-block callback, so the ExecutionCounters ledger stays exactly as
// complete as it is in one-block mode -- a batch trades per-call block granularity for speed, never
// measurability.
struct JitBatchOutcome
{
  // Blocks actually retired, which may exceed the requested minimum (a slice is not cut mid-block)
  // and falls short of it only when the batch stopped early, in which case `detail` says why.
  u64 blocks_executed = 0;
  // The PC the guest stopped at.
  u32 guest_pc = 0;
  bool backend_fault = false;
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
// caller-computed digest (ImageIdentity::IsAuthenticated()); this function does not itself
// recompute or verify the hash, matching the existing hook/original authentication convention in
// this file where authentication is an upstream, caller-attested fact.
//
// This is deliberately scoped to a raw in-memory image rather than a full BootParameters/DVD
// pipeline: the first embedding slice boots a small redistributable PPC test program, never a game
// disc (see docs/dolphin-embedding-contract.md). A title's real GameCube disc boot is a separate,
// later adapter that supplies its own validated image bytes through this same entry point.
//
// The process may boot at most one image at a time; a second call before ShutdownBootedImage
// returns a failed BootResult instead of silently reinitializing global Dolphin state.
//
// What a raw-image boot sets up around the image before it runs. Each field is independent, and a
// default-constructed value reproduces the original behaviour of booting nothing but the image.
struct GameCubeBootOptions
{
  // `apply_gamecube_os_init` selects the exact, title-neutral GameCube MSR/HID/BAT register setup
  // every retail title's real BS2/IPL establishes before jumping to a disc's DOL entry point (see
  // CBoot::SetupGameCubeBS2Registers, which reuses CBoot::EmulatedBS2_GC's own SetupMSR/SetupHID/
  // SetupBAT). It defaults to false so an existing caller booting a small synthetic PPC test
  // program that does not rely on effective-address translation is unaffected. A real GameCube
  // DOL's own code assumes this configuration is already in place: with MSR.DR/IR left at their
  // power-on-reset value of 0 (real mode), PowerPC treats an ordinary effective address like
  // 0x80xxxxxx as a physical address, landing far outside the console's 24 MiB of RAM instead of
  // translating back down into it, so a raw DOL boot without this flag reliably faults on its first
  // EA-dependent access.
  //
  bool apply_os_init = false;

  // `apply_gamecube_hardware_init` selects Dolphin's own maintained `HW::Init`/`HW::Shutdown`
  // (Source/Core/Core/HW/HW.cpp) in place of this function's own minimal Memory/CoreTiming/CPU
  // bring-up. `HW::Init` is what actually builds the `MMIO::Mapping` handler table (via
  // `MemoryManager::InitMMIO`, called at the end of `HW::Init`): every GameCube hardware register a
  // title's own `__init_hardware`-equivalent code touches (VideoInterface, ProcessorInterface,
  // SerialInterface, ExpansionInterface, AudioInterface, MemoryInterface, DSP, DVDInterface,
  // CommandProcessor, PixelEngine) is otherwise left with no registered read/write handler, so a
  // real title's ordinary hardware bring-up store or load into that physical range (e.g. GameCube
  // `ProcessorInterface` at physical 0x0C003000) calls through an uninitialized function pointer
  // and crashes; this is not a JIT bug, it is a missing hardware owner. It defaults to false so an
  // existing caller booting a small synthetic PPC test program that never touches hardware
  // registers is unaffected (`HW::Init` runs `system.GetMemory().Init()` again internally, so
  // calling it after this function's own Memory::Init would double-initialize and, worse, wipe an
  // already-copied image; when this flag is true the function calls `HW::Init` INSTEAD of its own
  // Memory/ CoreTiming/CPU calls, exactly once, before the image copy, matching Dolphin's own
  // EmuThread order).
  //
  // `HW::Init` on its own does not construct a host video backend or boot the DSP: those live in
  // separate calls Dolphin's own `EmuThread` makes AROUND `HW::Init`
  // (`g_video_backend->Initialize`, `DSPEmulator::Initialize`), and `apply_media_init` below
  // selects them. It does open no real input device by itself; this flag brings the
  // `ControllerInterface` up in its headless form because one of `HW::Init`'s own device owners
  // requires it. `AudioInterfaceManager::Init` (one of `HW::Init`'s own device owners)
  // unconditionally dereferences `system.GetSoundStream()`, so a `SoundStream` object must already
  // exist; this function calls `AudioCommon::InitSoundStream` itself for exactly that reason, with
  // the selected backend forced to Dolphin's own maintained "No Audio Output" (NullSound) backend
  // below, so this never opens a real host audio device. Two more of `HW::Init`'s device owners
  // have host side effects
  // that do not belong to a bare adapter boot with no configured title/user directory, so this flag
  // also forces safe, title-neutral defaults through Config before calling `HW::Init`: every
  // SerialInterface channel is forced to `SIDEVICE_NONE` (the default GameCube controller device
  // polls `Pad::GetStatus`, which indexes a `ControllerInterface` this function never initializes)
  // and both EXI memory card slots are forced to `EXIDeviceType::None` (the default
  // `MemoryCardFolder` device touches host disk under a per-title save path this function has no
  // way to derive from a raw image boot). Both are ordinary, real hardware states — no controller
  // plugged in, no memory card inserted — not a fabricated shortcut; a title consumer that wants
  // persistent input/storage devices attaches them itself afterward through the same Config surface
  // gcnport does not own.
  bool apply_hardware_init = false;

  // Filesystem path to the title's own disc image, so guest DVD commands read real data instead of
  // failing the way they do on a drive with no disc inserted. Empty mounts no disc, which is the
  // default and is itself an ordinary hardware state rather than an error.
  //
  // Only this consumer-supplied path crosses the boundary: gcnport never ships, embeds, or links a
  // game image, and never reads one except through a path its caller chose. Requires
  // apply_hardware_init, because DVDInterface and the DVD thread are among HW::Init's device
  // owners; asking for a disc without it is refused rather than silently ignored.
  std::string disc_image_path;

  // `apply_media_init` brings up the two periodic media devices Dolphin's own `EmuThread`
  // initializes around `HW::Init`: the video backend (pinned to Dolphin's maintained headless Null
  // backend) and the DSP emulator, followed by `Fifo::Prepare`.
  //
  // `HW::Init` and `SystemTimers::Init` already give a title working VI, DSP and audio-DMA
  // interrupts without this, so it is not needed to make time pass. What it supplies is a consumer
  // for those devices. Without a video backend the GP FIFO has no reader, so a title that writes
  // display lists eventually fills it and blocks; without `DSPEmulator::Initialize` the DSP object
  // `HW::Init` constructed never boots its ucode, so the SDK's DSP handshake never completes and
  // whichever thread performs it waits forever. Both failures look identical from outside: every
  // thread blocked while the scheduler idles and VI keeps ticking.
  //
  // It defaults to false so a caller running a synthetic PPC test program that touches neither is
  // unaffected, and it requires `apply_hardware_init`, whose devices it consumes. It also pins
  // single-core: the calling thread is declared as the GPU thread and `AsyncRequests` is put in
  // passthrough, because `ExecuteJitBlock`'s "exactly one observable block on the calling thread"
  // contract cannot hold if a separate GPU or DSP thread retires guest-visible work. A consumer
  // that owns a real renderer replaces the backend selection, but still needs an `AbstractGfx`
  // owner here, because that is what keeps the guest's GP writes draining.
  bool apply_media_init = false;

  // `run_apploader` runs the mounted disc's own apploader, which is what a real console does
  // between reading the disc header and entering a title. It is what loads the disc's file system
  // table and publishes its low-memory pointers; `disc_image_path` alone reads only the 0x20-byte
  // header, so without this a title's DVDConvertPathToEntrynum walks a null FST and every file
  // lookup fails.
  //
  // It defaults to false because a caller booting a synthetic image has no file system to load. It
  // requires a disc to run one from, and `apply_os_init`, because the apploader is guest code and
  // needs the address translation that flag establishes. The apploader also copies the title's
  // executable sections from the disc; the caller's authenticated image and entry point still win,
  // since both are applied after this runs.
  bool run_apploader = false;
};

[[nodiscard]] BootResult BootAuthenticatedImage(Core::System& system,
                                                const ExecutionIdentity& identity,
                                                std::span<const u8> image, u32 load_address,
                                                u32 entry_point,
                                                const GameCubeBootOptions& options = {});

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

  // Where the fallbacks happened, not just how many. A reason with a count says the JIT declined
  // something; only the guest address says which routine, and one hot loop and thousands of
  // distinct sites produce the same total. Bounded, because a diagnostic must not grow without
  // limit inside a long run: once kMaxTrackedFallbackSites distinct addresses are held, further
  // NEW addresses are counted in ExecutionCounters::fallback_sites_not_tracked rather than
  // silently dropped, so a truncated list can always be told from a complete one. Counts for
  // addresses already held keep accumulating.
  [[nodiscard]] const std::map<u32, FallbackSite>& GetFallbackSites() const
  {
    return m_fallback_sites;
  }

  // Executes exactly one observable guest basic block from the live PC through the ordinary
  // Jit64/JitArm64 dispatcher. One block per call comes from capping the CoreTiming SLICE at one
  // cycle (see BootAuthenticatedImage): the generated dispatcher returns to its caller at a slice
  // boundary, so a minimal slice cannot chain a second direct-linked block. It deliberately does
  // not touch the PowerPC downcount, which CoreTiming::Advance() reads to account for the time the
  // previous slice consumed. The block is compiled on a cache miss and reported as CacheHit on any
  // later entry with the same address and feature flags. An unavailable JIT is a
  // JitBlockKind::BackendFault, never a refusal.
  //
  // This granularity costs roughly a host round trip per block, so it is the right tool for
  // stepping, diagnostics and hook-ordered dispatch, and the wrong one for running a title. Use
  // ExecuteJitBlocks for that.
  [[nodiscard]] JitBlockOutcome ExecuteJitBlock();

  // Executes at least `minimum_blocks` guest basic blocks from the live PC, letting the dispatcher
  // chain direct-linked blocks within ordinary CoreTiming slices instead of returning after each
  // one. Scheduled hardware events still bound each slice exactly as they do during normal Dolphin
  // execution, so hardware timing is unaffected; only gcnport's own one-block cap is lifted.
  //
  // Refuses rather than running while a one-shot original ticket is armed: those tickets are
  // consumed by the per-dispatch driver loop, and a batch has no per-dispatch boundary at which to
  // consume one, so allowing it would silently drop the suppression.
  [[nodiscard]] JitBatchOutcome ExecuteJitBlocks(u64 minimum_blocks);

  // Executes the given already-refused PC through Dolphin's ordinary interpreter for at most
  // `maximum_instruction_count` guest instructions, then returns without resuming JIT dispatch
  // itself (the caller resumes dispatch by calling ExecuteJitBlock next, per the dispatch ordering
  // in docs/dolphin-embedding-contract.md).
  [[nodiscard]] InterpretedBlockResult ExecuteRefusedBlock(u32 guest_pc,
                                                           u32 maximum_instruction_count);

  // Executes through the plain interpreter for at most `maximum_instruction_count` instructions
  // without ever consulting the gameplay JIT selector. This is a distinct, explicitly diagnostic
  // entry point and must not be reachable from ExecuteJitBlock/ExecuteRefusedBlock.
  [[nodiscard]] InterpretedBlockResult
  ExecuteDiagnosticInterpreterBlock(u32 maximum_instruction_count);

  // Arms a one-shot ticket so the NEXT dispatch that reaches `key.address` under `key.identity`
  // falls through to the ordinary translated body instead of invoking any registered native hook,
  // without requiring the hook callback itself to return HookAction::RunOriginalOnce. Combined with
  // ExecuteJitBlock's one-block granularity, a native caller gets a real synchronous
  // native -> original -> native continuation: arm the ticket, call ExecuteJitBlock to run exactly
  // the original body once, then resume native code. Returns false if the key is invalid or has no
  // installed hook to suppress.
  [[nodiscard]] bool ExecuteOriginalOnce(const HookKey& key);

  // Runs the ORIGINAL guest body at key.address as a synchronous subroutine call, for use FROM
  // INSIDE a NativeHook callback (the classic "superCall": a native override that wants to run
  // native code, call through to the real guest function, then run more native code and decide the
  // outcome, all inside one callback invocation). ExecuteOriginalOnce/RunOriginalOnce cannot do
  // this: both only arm a one-shot suppression that the OUTER ExecuteJitBlock driver loop consumes
  // on its NEXT dispatch, so control never returns to the callback that requested it.
  //
  // This is safe to call reentrantly from inside a hook callback because it never re-enters the JIT
  // dispatcher or its generated-code call stack: it drives Dolphin's plain interpreter directly,
  // starting at key.address, until the guest body executes a control-flow instruction that returns
  // to the caller's current link register (an ordinary ABI `blr` epilogue) or
  // `maximum_instruction_count` is reached, whichever comes first. Exceeding the bound without
  // returning is a hard fault, not a silently truncated call -- a real callee that does not return
  // within the bound is a caller error, not an expected outcome to swallow.
  //
  // PC/NPC are restored to their pre-call values on return so the enclosing hook callback retains
  // full control over the final HookResult (e.g. ReturnToCaller, ContinueAt); every other guest
  // register and memory side effect made by the original body is real and observable, matching what
  // an ordinary guest-to-guest call would have produced.
  [[nodiscard]] InterpretedBlockResult CallOriginalSynchronously(const HookKey& key,
                                                                 u32 maximum_instruction_count);

  // Generated code calls this ABI boundary. true means fall through to the ordinary translated
  // instruction; false means the hook updated PC and the generated guard must redispatch.
  [[nodiscard]] static bool RunHookFromJit(RuntimeSession* session, u32 address) noexcept;
  static void RecordJitBlockExecutionFromJit(RuntimeSession* session, u32 address) noexcept;
  static void RecordFallbackFromJit(RuntimeSession* session, u32 address,
                                    u32 reason_value) noexcept;

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
  std::map<u32, FallbackSite> m_fallback_sites;
  JitRefusalReason m_last_fallback_reason = JitRefusalReason::UnsupportedInstruction;
};

}  // namespace GcnPort
}  // namespace PowerPC
