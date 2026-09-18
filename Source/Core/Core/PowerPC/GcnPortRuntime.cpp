// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/GcnPortRuntime.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <string_view>

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/MemTools.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/Volume.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/VideoBackendBase.h"

namespace PowerPC::GcnPort
{
namespace
{
// Config::AddLayer builds a layer from a loader, so an empty Base layer needs a loader that reads
// and writes nothing. Config::Init() creates only the CurrentRun layer, and several Core owners
// write through Base -- Common::Log::LogManager's constructor does -- where Config::Layer::Set
// dereferences a null layer if none exists.
//
// This is deliberately not ConfigLoaders::GenerateBaseConfigLoader(), which is a frontend's: it
// reads and writes the user's Dolphin.ini, so an embedded boot's behaviour would depend on whatever
// that person last set in Dolphin, which is exactly what a deterministic adapter must not do.
// Forced runtime selections in this file stay on Config::SetCurrent, which outranks Base either
// way; Base exists here only so that writes have somewhere to land.
class EmptyBaseConfigLoader final : public Config::ConfigLayerLoader
{
public:
  EmptyBaseConfigLoader() : ConfigLayerLoader(Config::LayerType::Base) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

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

// Tracks which shutdown sequence matches the boot that set g_image_booted: HW::Shutdown() must be
// paired with HW::Init(), and the original minimal Memory/CoreTiming/CPU shutdown must be paired
// with this function's own minimal bring-up, or shutdown either double-frees state HW::Init never
// touched or leaves HW::Init's device owners (DVD thread, ARAM allocation, EXI channels) alive.
bool g_hardware_initialized = false;

// A no-op CoreTiming event, kept permanently one cycle in the future, whose only purpose is to cap
// the length of every slice so Dolphin's generated dispatcher hands control back after a single
// block (see the comment at its registration in BootAuthenticatedImage). File scope because
// CoreTiming::TimedCallback is a plain function pointer, so the callback that reschedules it cannot
// capture. CoreTiming::Init() clears the event registry, so this is registered per boot.
CoreTiming::EventType* g_block_bound_event = nullptr;

void RescheduleBlockBound(Core::System& system, u64, s64)
{
  system.GetCoreTiming().ScheduleEvent(1, g_block_bound_event);
}

// Lifts the one-block slice cap for the duration of a batch and restores it afterwards. Removing
// the bounding event outright, rather than rescheduling it further out, is deliberate: CoreTiming
// already sizes a slice from the next genuinely scheduled event and caps it at its own
// MAX_SLICE_LENGTH, so with our event gone the batch runs on exactly the slice lengths ordinary
// Dolphin execution would use. Choosing some larger interval here instead would invent a second,
// competing slice policy. Nothing can re-arm the event while it is removed, because only its own
// callback reschedules it.
class LiftedBlockBound final
{
public:
  explicit LiftedBlockBound(Core::System& system) : m_system(system)
  {
    m_system.GetCoreTiming().RemoveEvent(g_block_bound_event);
  }

  ~LiftedBlockBound()
  {
    auto& core_timing = m_system.GetCoreTiming();
    core_timing.RemoveEvent(g_block_bound_event);
    core_timing.ScheduleEvent(1, g_block_bound_event);
  }

  LiftedBlockBound(const LiftedBlockBound&) = delete;
  LiftedBlockBound& operator=(const LiftedBlockBound&) = delete;
  LiftedBlockBound(LiftedBlockBound&&) = delete;
  LiftedBlockBound& operator=(LiftedBlockBound&&) = delete;

private:
  Core::System& m_system;
};

// Whether this process's SIGSEGV/SIGBUS fastmem handler is currently installed. Tracked separately
// from g_hardware_initialized because EMM::IsExceptionHandlerSupported() can be false on a host
// without this backend, in which case nothing was actually installed and Shutdown must not try to
// uninstall it.
bool g_exception_handler_installed = false;

// Tracks whether the media bring-up ran, so shutdown tears down exactly what boot brought up.
// Kept separate from g_hardware_initialized because apply_media_init is independently selectable.
bool g_media_initialized = false;

// A bare in-memory-image boot has no configured title identity or user directory and does not own
// input, persistent storage, or audio output (see docs/dolphin-embedding-contract.md). Force real,
// title-neutral "no device attached" hardware states before HW::Init() attaches its Config-selected
// defaults, so bringing up hardware registers never touches host disk (the default EXI slot A
// device is a MemoryCardFolder that scans/creates a save directory for whatever the current game ID
// happens to be), never indexes an uninitialized host ControllerInterface (the default SI channel 0
// device is a live GameCube controller that polls Pad::GetStatus), and never opens a real host
// audio device (AudioInterfaceManager::Init() unconditionally dereferences system.GetSoundStream(),
// so a SoundStream must already exist; the config-selected default backend, e.g. Cubeb, would
// otherwise open a real device). All three are ordinary real hardware/software states, not a
// fabricated shortcut: a real console can boot with its controller unplugged and no memory card
// inserted, and Dolphin's own maintained NullSound backend is exactly the "no audio output" state
// its UI already exposes, not a gcnport-invented stub. A title consumer that wants persistent
// input/storage/audio devices attaches them afterward through this same Config/SoundStream surface;
// gcnport does not own that policy.
void ForceNoHostBackedGameCubeDevices()
{
  for (int channel = 0; channel < SerialInterface::MAX_SI_CHANNELS; ++channel)
    Config::SetCurrent(Config::GetInfoForSIDevice(channel), SerialInterface::SIDEVICE_NONE);
  Config::SetCurrent(Config::MAIN_SLOT_A, ExpansionInterface::EXIDeviceType::None);
  Config::SetCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::None);
  Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
}
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

// Reverses everything BootAuthenticatedImage brought up, for a boot that fails after bring-up but
// before it publishes itself as booted. ShutdownBootedImage cannot serve here: it keys off
// g_image_booted, which a failed boot never sets, so calling it would silently do nothing and leave
// HW::Init's device owners and CoreTiming's event registry live -- which the NEXT boot then hits as
// "CoreTiming Event is already registered", far from the boot that actually leaked it.
// Brings up the periodic media devices Dolphin's own EmuThread initializes AROUND HW::Init().
//
// HW::Init() builds the MMIO handler table and SystemTimers::Init() schedules the VI, DSP and audio
// DMA events, so a title's interrupts already arrive without this. What it does not do is give
// those devices anything to talk to. Measured against a retail title: with no FIFO consumer, a
// GXDrawDone
// -- which writes a draw-done token and then sleeps until the PixelEngine finish interrupt reports
// the GPU has drained past it -- never wakes, because no interrupt is ever raised for a token
// nothing consumed. Every other thread was idle on its own work queue, so the whole title sat in
// the SDK's scheduler idle loop while VI interrupts kept arriving at 60 Hz. DSPManager::Init() has
// the matching gap: it constructs a DSPEmulator without booting its ucode, so a title's DSP
// handshake never completes and whichever thread performs it waits the same way. Both are separate
// calls in Core.cpp's EmuThread (GetInitializedVideoGuard, then GetDSPEmulator()->Initialize),
// which a bare adapter boot never runs.
//
// The video backend is pinned to Null. WindowSystemInfo defaults to WindowSystemType::Headless, and
// Null implements the AbstractGfx interface without opening a host device or a window -- the same
// shape as the NullSound and no-memory-card states this file already forces. A consumer that owns a
// real renderer replaces the backend selection; it still needs SOME AbstractGfx owner here, because
// the FIFO's consumer is what keeps the guest's GP writes draining.
//
// Single-core is the only mode this adapter supports: ExecuteJitBlock's contract is "run exactly
// one observable block on the calling thread", which a separate GPU thread would make unobservable.
// So the calling thread is declared as the GPU thread and AsyncRequests is put in passthrough,
// exactly as Core.cpp does when IsDualCoreMode() is false.
bool InitializeMediaDevices(Core::System& system)
{
  // SetCurrent, matching how ForceNoHostBackedGameCubeDevices above pins its own device selections:
  // this is a selection this boot forces for its own lifetime, not a default to persist into the
  // consumer's configuration, and CurrentRun outranks the Base layer.
  Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("Null"));

  const WindowSystemInfo headless_wsi{};
  VideoBackendBase::PopulateBackendInfo(headless_wsi);
  if (g_video_backend == nullptr)
  {
    return false;
  }

  WindowSystemInfo prepared_wsi(headless_wsi);
  g_video_backend->PrepareWindow(prepared_wsi);

  Core::DeclareAsGPUThread();
  AsyncRequests::GetInstance()->SetPassthrough(true);
  if (!g_video_backend->Initialize(prepared_wsi))
  {
    return false;
  }

  // The DSP thread is refused for the same reason the GPU thread is: it would retire guest-visible
  // work outside the calling thread's control.
  if (!system.GetDSP().GetDSPEmulator()->Initialize(system.IsWii(), false))
  {
    g_video_backend->Shutdown();
    return false;
  }

  AudioCommon::PostInitSoundStream(system);
  system.GetFifo().Prepare();
  return true;
}

void ShutdownMediaDevices(Core::System& system)
{
  system.GetFifo().Shutdown();
  g_video_backend->Shutdown();
  Core::UndeclareAsGPUThread();
}

void TearDownIncompleteBringUp(Core::System& system, bool hardware_initialized)
{
  if (hardware_initialized)
  {
    if (g_exception_handler_installed)
    {
      EMM::UninstallExceptionHandler();
      g_exception_handler_installed = false;
    }
    if (g_media_initialized)
    {
      ShutdownMediaDevices(system);
      g_media_initialized = false;
    }
    HW::Shutdown(system);
    AudioCommon::ShutdownSoundStream(system);
    g_controller_interface.Shutdown();
  }
  else
  {
    system.GetCPU().Shutdown();
    system.GetCoreTiming().Shutdown();
    system.GetMemory().Shutdown();
  }
  Common::Log::LogManager::Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
  Core::UndeclareAsCPUThread();
}

BootResult BootAuthenticatedImage(Core::System& system, const ExecutionIdentity& identity,
                                  std::span<const u8> image, u32 load_address, u32 entry_point,
                                  const GameCubeBootOptions& options)
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
  // DVDInterface and the DVD thread are among HW::Init's device owners, so there is nothing to
  // mount a disc into without it. Refuse here, before any global Dolphin state is touched, rather
  // than booting successfully with the disc silently absent.
  if (options.run_apploader && options.disc_image_path.empty())
  {
    return {.ok = false, .detail = "running an apploader requires the disc it lives on"};
  }

  if (options.run_apploader && !options.apply_os_init)
  {
    return {.ok = false,
            .detail = "an apploader is guest code, so it requires apply_os_init's address "
                      "translation setup"};
  }

  if (options.apply_media_init && !options.apply_hardware_init)
  {
    return {.ok = false,
            .detail = "media init requires apply_hardware_init, which owns the devices it drives"};
  }

  if (!options.disc_image_path.empty() && !options.apply_hardware_init)
  {
    return {.ok = false,
            .detail = "a disc image requires apply_hardware_init, which owns DVDInterface"};
  }

  Core::DeclareAsCPUThread();
  Config::Init();
  Config::AddLayer(std::make_unique<EmptyBaseConfigLoader>());
  SConfig::Init();

  // Dolphin's subsystems reach the log manager through a raw singleton pointer and do not check it.
  // FileMonitor::FileLogger::Log, which DVDThread::ProcessReadRequest calls on every disc FILE
  // read, dereferences it unconditionally -- measured as a SIGSEGV on the DVD thread with
  // LogManager::IsEnabled's `this` at null, the first time a boot read a file rather than the raw
  // disc header. UICommon::Init is where a frontend brings this up, after Config and SConfig, and
  // this embedding does not call that. Bring up the one piece of it Core requires, in the same
  // order, rather than the whole frontend, whose config layers this file deliberately does not
  // want.
  Common::Log::LogManager::Init();

  // The disc is opened before any hardware exists because what it is decides how the hardware must
  // behave. Dolphin ships per-title correctness settings in Sys/GameSettings, and for titles that
  // need them the defaults are wrong: Super Mario Sunshine's GMS.ini sets EFBToTextureEnable=False,
  // and without it Dolphin's default "keep EFB copies in VRAM" hack zeroes the copy's destination
  // in guest RAM (TextureCacheBase::UninitializeEFBMemory). Measured against that title, the zeroed
  // range ran past the framebuffer it had allocated and wiped the live object immediately after it,
  // and the title branched through the resulting null vtable on its first frame. These layers are
  // shipped Dolphin data, in the same category as the IPL fonts and the DSP ROM this boot already
  // resolves through File::GetSysDirectory(), so they are applied whenever a disc is present.
  //
  // Deliberately only the global layer. ConfigLoaders::GenerateLocalGameConfigLoader() reads the
  // *user's* per-game INI, which would make an embedded boot depend on whatever that person last
  // set in Dolphin -- the same reason the Base layer above is empty rather than the frontend's.
  //
  // Opening it here is also the only ordering that works: Config::AddLayer's change notification
  // reaches VideoCommon through CPUThreadConfigCallback, which a frontend's CPU thread pumps and
  // this adapter's caller-driven block dispatch does not, so a layer added after the video backend
  // came up would compile into g_ActiveConfig only by luck.
  std::unique_ptr<DiscIO::VolumeDisc> disc;
  if (!options.disc_image_path.empty())
  {
    disc = DiscIO::CreateDisc(options.disc_image_path);
    if (!disc)
    {
      Common::Log::LogManager::Shutdown();
      SConfig::Shutdown();
      Config::Shutdown();
      return {.ok = false,
              .detail = "could not open a GameCube/Wii disc image at " + options.disc_image_path};
    }
    Config::AddLayer(ConfigLoaders::GenerateGlobalGameConfigLoader(
        disc->GetGameID(), disc->GetRevision().value_or(0)));

    // The console has to be the one this disc expects. CBoot::SetupGCMemory publishes the video
    // format at 0x800000CC from SConfig's region, EmulatedBS2_GC picks the IPL font encoding and
    // the BS2 region settings from it, and SConfig leaves it Unknown -- which DiscIO::IsNTSC reads
    // as PAL. Measured with a US disc: the title built a PAL render mode (xfbHeight 530) while its
    // own framebuffer allocation was the NTSC-sized 0xa5000 = 640x528x2, so the display copy ran
    // two lines past the end of that block and over the live object the heap had placed directly
    // after it. SConfig::SetPathsAndGameMetadata is where a frontend takes this from the volume;
    // this adapter takes it from the same place, and leaves the frontend's fallback-region setting
    // alone because a disc that states its own region does not need one.
    SConfig::GetInstance().m_region = disc->GetRegion();
  }

  // HW::Init() performs its own system.GetMemory().Init() internally (it must: MemoryManager::
  // InitMMIO(), which builds the MMIO::Mapping handler table, depends on the rest of HW::Init()'s
  // device construction). Calling this function's own minimal Memory::Init() first as well would
  // reallocate the physical memory arena a second time, leaking the first one; the two bring-up
  // paths are therefore mutually exclusive, not additive.
  //
  // AudioInterfaceManager::Init() (called from inside HW::Init()) unconditionally dereferences
  // system.GetSoundStream(), so a SoundStream object must already exist before HW::Init() runs.
  // AudioCommon::InitSoundStream() is the only owner of that construction; ForceNoHostBackedGame-
  // CubeDevices() already pinned the selected backend to NullSound so this never opens a real host
  // audio device.
  //
  // A hardware register such as GameCube ProcessorInterface has no fastmem-backed page (only RAM/
  // L1/fake-VMEM/EXRAM physical regions are mapped, see MemoryManager::Init's physical_regions
  // table), so a JIT-generated fastmem load/store that targets one deliberately raises SIGSEGV to
  // reach the safe MMU/MMIO path. Dolphin's own maintained CpuThread() installs the handler for
  // exactly this reason (Core.cpp, "The JIT need to be able to intercept faults, both for fastmem
  // and for the BLR optimization"); a bare adapter boot never runs that function, so it must
  // install the same handler itself once real hardware registers are reachable, or an ordinary
  // fastmem- optimized access to one crashes the process outright instead of reaching the
  // registered MMIO handler.
  if (options.apply_hardware_init)
  {
    ForceNoHostBackedGameCubeDevices();
    AudioCommon::InitSoundStream(system);

    // SerialInterfaceManager's periodic poll calls g_controller_interface.UpdateInput()
    // unconditionally -- before, and independently of, asking any SI channel for data -- and that
    // call asserts on m_is_init. Forcing every channel to SIDEVICE_NONE above is therefore not
    // sufficient: the poll still runs, because an SI poll with nothing plugged in is exactly what
    // real hardware does. Bring the interface up in its headless form so that poll has a real,
    // initialized owner. WindowSystemInfo defaults to WindowSystemType::Headless and every host
    // input backend is compile-time gated (CIFACE_USE_*), so this constructs the "no input devices
    // present" state rather than opening a host device -- the same shape as the NullSound and
    // no-memory-card states forced above. A title consumer that owns real input attaches its own
    // devices through this same interface afterward.
    g_controller_interface.Initialize(WindowSystemInfo{});

    HW::Init(system, nullptr);
    if (EMM::IsExceptionHandlerSupported())
    {
      EMM::InstallExceptionHandler();
      g_exception_handler_installed = true;
    }
    if (options.apply_media_init)
    {
      if (!InitializeMediaDevices(system))
      {
        TearDownIncompleteBringUp(system, true);
        return {.ok = false, .detail = "could not bring up the headless video backend or the DSP"};
      }
      g_media_initialized = true;
    }
  }
  else
  {
    system.GetMemory().Init();
    system.GetCoreTiming().Init();
    system.GetCPU().Init(PowerPC::DefaultCPUCore());
  }

  // ExecuteJitBlock's contract is "run exactly one observable JIT block, then return". Dolphin's
  // generated dispatcher only returns to its caller at a slice boundary (its `do_timing` path,
  // taken when the CPU state is not Running), so bounding a call to one block means bounding the
  // SLICE to one block. CoreTiming already sizes every slice to end exactly at the next scheduled
  // event (CoreTimingManager::Advance: `slice_length = min(next_event.time - global_timer, ...)`),
  // so an event kept permanently one cycle ahead keeps every slice minimal.
  //
  // Bounding this way is what lets ExecuteJitBlock leave `ppc_state.downcount` alone, which is
  // required for correct timekeeping -- see the long comment there. MAIN_ENABLE_DEBUGGING would
  // also produce a per-block dispatcher exit, but it is the wrong tool: it additionally drives the
  // block analyzer into single-instruction blocks (JitBase::RefreshConfig ->
  // analyzer.SetDebuggingEnabled), destroying exactly the block-level granularity this API exposes.
  auto& core_timing = system.GetCoreTiming();
  g_block_bound_event = core_timing.RegisterEvent("GcnPortBlockBound", RescheduleBlockBound);
  core_timing.ScheduleEvent(1, g_block_bound_event);

  const u32 ram_size = system.GetMemory().GetRamSizeReal();
  if (load_address < EFFECTIVE_RAM_BASE ||
      static_cast<u64>(load_address - EFFECTIVE_RAM_BASE) + image.size() > ram_size)
  {
    TearDownIncompleteBringUp(system, options.apply_hardware_init);
    return {.ok = false, .detail = "image does not fit inside mapped GameCube RAM"};
  }

  system.GetMemory().CopyToEmu(load_address, image.data(), image.size());

  if (options.apply_os_init)
  {
    CBoot::SetupGameCubeBS2Registers(system);

    // The registers alone are not what BS2 leaves behind. The SDK reads fixed low-memory globals
    // back by address -- __OSPhysicalMemSize at 0x80000028, __OSBusClock at 0x800000F8, the ARAM
    // size, console type, and the default rfi exception handlers -- and derives OS_TIMER_CLOCK from
    // the bus clock, so leaving these zero silently corrupts every tick and time conversion a title
    // makes. Reuse Dolphin's own maintained writer rather than restating the constants here.
    //
    // Must follow the register setup: these are effective-address writes, and without the BATs and
    // MSR.DR that SetupGameCubeBS2Registers installs there is no translation for 0x800000xx at all.
    // It touches only the OS globals below 0x80003000, never the region a raw image is loaded into.
    const Core::CPUThreadGuard guard(system);
    CBoot::SetupGCMemory(system, guard);
  }

  if (disc)
  {
    // What BS2 does with a disc before handing control to the title, in the order EmulatedBS2_GC
    // does it: read the 0x20-byte disc header to physical 0, which is also what moves the drive out
    // of its DiscIdNotRead state, then leave the volume mounted for the title's own reads. Without
    // the header a title cannot identify the disc it is running from; without the mount its first
    // real read fails and the SDK falls into its disc-error screen.
    if (!CBoot::DVDReadDiscID(system, *disc, 0x00000000))
    {
      TearDownIncompleteBringUp(system, options.apply_hardware_init);
      return {.ok = false,
              .detail = "could not read the disc header from " + options.disc_image_path};
    }
    DiscIO::VolumeDisc& volume = *disc;
    system.GetDVDInterface().SetDisc(std::move(disc), {});

    // The apploader is the disc's own code, and running it is what loads the title's file system
    // table and publishes its low-memory pointers. DVDReadDiscID above reads only the 0x20-byte
    // header, so without this a title's DVDConvertPathToEntrynum walks a null FST, every file
    // lookup it makes fails, and it proceeds on null pointers -- measured against a retail title as
    // reads from address 0x00000008 inside the SDK's own path-to-entry conversion.
    //
    // It also copies the title's executable sections from the disc, which for a caller booting that
    // same title's pre-extracted image writes the same bytes over the same addresses. The caller's
    // entry point still wins: the apploader leaves the disc's entry in pc and this function
    // overwrites it below with the one the caller authenticated, so a caller booting something
    // other than this disc's executable gets what it asked for rather than the disc's.
    //
    // DVDInterface owns the volume after the move, so the reference above stays valid; the
    // apploader reads through it directly rather than through the drive.
    if (options.run_apploader)
    {
      const Core::CPUThreadGuard guard(system);
      if (!CBoot::LoadGameCubeDiscViaApploader(system, guard, volume, {}))
      {
        TearDownIncompleteBringUp(system, options.apply_hardware_init);
        return {.ok = false,
                .detail =
                    "the apploader on " + options.disc_image_path + " did not run to completion"};
      }
    }
  }

  auto& state = system.GetPPCState();
  state.pc = entry_point;
  state.npc = entry_point;

  g_image_booted = true;
  g_hardware_initialized = options.apply_hardware_init;
  return {.ok = true, .detail = ""};
}

void ShutdownBootedImage(Core::System& system) noexcept
{
  if (!g_image_booted)
    return;
  if (g_hardware_initialized)
  {
    if (g_exception_handler_installed)
    {
      EMM::UninstallExceptionHandler();
      g_exception_handler_installed = false;
    }
    if (g_media_initialized)
    {
      ShutdownMediaDevices(system);
      g_media_initialized = false;
    }
    HW::Shutdown(system);
    AudioCommon::ShutdownSoundStream(system);
    g_controller_interface.Shutdown();
  }
  else
  {
    system.GetCPU().Shutdown();
    system.GetCoreTiming().Shutdown();
    system.GetMemory().Shutdown();
  }
  Common::Log::LogManager::Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
  Core::UndeclareAsCPUThread();
  g_image_booted = false;
  g_hardware_initialized = false;
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

  // Deliberately does NOT write ppc_state.downcount. CoreTiming::Advance() -- which Dolphin's own
  // generated dispatcher calls on entry -- derives elapsed guest time from exactly that leftover:
  //
  //     cyclesExecuted = slice_length - DowncountToCycles(downcount)
  //
  // so on entry `downcount` must still be the natural remainder of the previous slice (normally
  // negative: the amount by which the last block overran it). Overwriting it with a sentinel makes
  // Advance() attribute the wrong cycle count to the slice that just ran. Writing 1 while
  // slice_length was also 1 -- the steady state a one-block-at-a-time caller settles into --
  // yielded 1 - 1 == 0 and froze the global timer permanently, so no scheduled CoreTiming event
  // could ever come due and no hardware completion interrupt was ever raised. Measured against
  // exact GMSE01: the timer stuck at 30,891 ticks across 16,384 consecutive dispatches while the
  // title spun forever inside __OSInitAudioSystem waiting on the ARAM DMA completion interrupt
  // (INT_ARAM, DSP_CONTROL bit 0x20) that DSPManager::Do_ARAM_DMA had scheduled just 246 ticks
  // ahead.
  //
  // The sentinel never bounded anything either: Advance() reassigns downcount from the event queue
  // before the first block runs. One block per call comes from the one-cycle slice cap installed in
  // BootAuthenticatedImage.
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
    // through an embedded interpreter call rather than refusing the block outright. This is
    // reported as a refusal so the framework-level fallback ledger still sees and budgets it.
    outcome.kind = JitBlockKind::Refused;
    outcome.refusal_reason = m_last_fallback_reason;
  }
  else
  {
    outcome.kind = m_counters.jit_blocks_compiled > compiled_before ? JitBlockKind::Compiled :
                                                                      JitBlockKind::CacheHit;
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

InterpretedBlockResult
RuntimeSession::ExecuteDiagnosticInterpreterBlock(u32 maximum_instruction_count)
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

JitBatchOutcome RuntimeSession::ExecuteJitBlocks(u64 minimum_blocks)
{
  JitBatchOutcome outcome;
  outcome.guest_pc = m_system.GetPPCState().pc;

  if (!m_jit)
  {
    outcome.backend_fault = true;
    outcome.detail = "host JIT backend is unavailable";
    return outcome;
  }
  if (minimum_blocks == 0)
  {
    outcome.backend_fault = true;
    outcome.detail = "a batch must be asked for at least one block";
    return outcome;
  }
  if (!m_pending_original_tickets.empty())
  {
    outcome.backend_fault = true;
    outcome.detail = "a one-shot original ticket is armed; consume it with ExecuteJitBlock first";
    return outcome;
  }

  const u64 executions_before = m_counters.jit_block_executions;
  {
    const LiftedBlockBound lifted(m_system);
    while (m_counters.jit_block_executions - executions_before < minimum_blocks)
    {
      const u64 executions_before_slice = m_counters.jit_block_executions;
      m_system.GetPowerPC().SingleStep();
      if (m_counters.jit_block_executions == executions_before_slice)
      {
        // A whole slice that retired no block at all will not start retiring them by being
        // repeated, and looping on it would hang the caller instead of reporting the condition.
        // Stop and say so; blocks_executed below reports exactly how far the batch actually got.
        outcome.detail = "a full slice retired no guest block";
        break;
      }
    }
  }

  outcome.blocks_executed = m_counters.jit_block_executions - executions_before;
  outcome.guest_pc = m_system.GetPPCState().pc;
  return outcome;
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
