// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <OptionParser.h>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <Windows.h>
#endif

#include "Common/HookableEvent.h"
#include "Common/ScopeGuard.h"
#include "Common/SunbrightHooks.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Common/FileUtil.h"
#include "Core/Host.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/VideoEvents.h"

#include "UICommon/CommandLineParse.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif
#include "UICommon/UICommon.h"

static std::unique_ptr<Platform> s_platform;

static void signal_handler(int)
{
  constexpr char message[] = "A signal was received. A second signal will force Dolphin to stop.\n";
#ifdef _WIN32
  puts(message);
#else
  if (write(STDERR_FILENO, message, sizeof(message)) < 0)
  {
  }
#endif

  s_platform->RequestShutdown();
}

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(const HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_platform->Stop();
}

void Host_UpdateTitle(const std::string& title)
{
  s_platform->SetTitle(title);
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return s_platform->IsWindowFocused();
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return s_platform->IsWindowFullscreen();
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateClientID(client_id);
#endif
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  return Discord::UpdateDiscordPresenceRaw(details, state, large_image_key, large_image_text,
                                           small_image_key, small_image_text, start_timestamp,
                                           end_timestamp, party_size, party_max);
#else
  return false;
#endif
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

static std::unique_ptr<Platform> GetPlatform(const optparse::Values& options)
{
  std::string platform_name = static_cast<const char*>(options.get("platform"));

#if HAVE_X11
  if (platform_name == "x11" || platform_name.empty())
    return Platform::CreateX11Platform();
#endif

#ifdef __linux__
  if (platform_name == "fbdev" || platform_name.empty())
    return Platform::CreateFBDevPlatform();
#endif

#ifdef _WIN32
  if (platform_name == "win32" || platform_name.empty())
    return Platform::CreateWin32Platform();
#endif
#ifdef __APPLE__
  if (platform_name == "macos" || platform_name.empty())
    return Platform::CreateMacOSPlatform();
#endif

  if (platform_name == "headless" || platform_name.empty())
    return Platform::CreateHeadlessPlatform();

  return nullptr;
}

#ifdef _WIN32
#define main app_main
#endif

int main(const int argc, char* argv[])
{
  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
  parser->add_option("-p", "--platform")
      .action("store")
      .help("Window platform to use [%choices]")
      .choices({"headless"
#ifdef __linux__
                ,
                "fbdev"
#endif
#if HAVE_X11
                ,
                "x11"
#endif
#ifdef _WIN32
                ,
                "win32"
#endif
#ifdef __APPLE__
                ,
                "macos"
#endif
      });

  // Sunbright: headless FIFO recording. Drives Dolphin's own FifoRecorder from
  // the NoGUI frontend so a .dff can be captured with no Qt window — replaces
  // the xdrive.py GUI-driving hack. Boot is deterministic under a fixed
  // EmulationSpeed, so a frame count reliably lands on a target scene.
  parser->add_option("--fifo-record")
      .action("store")
      .help("Headless: record a FIFO log (.dff) to this path, then exit");
  parser->add_option("--fifo-record-after")
      .action("store")
      .type("int")
      .set_default("3400")
      .help("VI fields to wait after boot before recording starts [%default]");
  parser->add_option("--fifo-record-frames")
      .action("store")
      .type("int")
      .set_default("3")
      .help("Number of frames to record into the .dff [%default]");
  parser->add_option("--pad-start-at")
      .action("store")
      .type("int")
      .set_default("-1")
      .help("Headless oracle: VI field to begin holding GC START, to reach input-gated "
            "screens like file-select (-1 = off) [%default]");
  parser->add_option("--pad-start-frames")
      .action("store")
      .type("int")
      .set_default("6")
      .help("VI fields to hold the injected START press [%default]");
  parser->add_option("--save-state-at")
      .action("store")
      .type("int")
      .set_default("-1")
      .help("Headless oracle: save a Dolphin save state at this VI field, then exit. Pairs with "
            "--pad-start-at to reach a settled scene, giving a REPRODUCIBLE matched-state oracle "
            "(reload with --save_state) (-1 = off) [%default]");
  parser->add_option("--save-state-path")
      .action("store")
      .set_default("scratch/oracle/state/fsel.sav")
      .help("Path for --save-state-at to write the save state [%default]");

  optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  std::vector<std::string> args = parser->args();

  std::optional<std::string> save_state_path;
  if (options.is_set("save_state"))
  {
    save_state_path = static_cast<const char*>(options.get("save_state"));
  }

  std::unique_ptr<BootParameters> boot;
  bool game_specified = false;
  if (options.is_set("exec"))
  {
    const std::list<std::string> paths_list = options.all("exec");
    const std::vector<std::string> paths{std::make_move_iterator(std::begin(paths_list)),
                                         std::make_move_iterator(std::end(paths_list))};
    boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    const std::string hex_string = static_cast<const char*>(options.get("nand_title"));
    if (hex_string.length() != 16)
    {
      fprintf(stderr, "Invalid title ID\n");
      parser->print_help();
      return 1;
    }
    const u64 title_id = std::stoull(hex_string, nullptr, 16);
    boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }
  else if (args.size())
  {
    boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    args.erase(args.begin());
    game_specified = true;
  }
  else
  {
    parser->print_help();
    return 0;
  }

  std::string user_directory;
  if (options.is_set("user"))
    user_directory = static_cast<const char*>(options.get("user"));

  s_platform = GetPlatform(options);
  if (!s_platform || !s_platform->Init())
  {
    fprintf(stderr, "No platform found, or failed to initialize.\n");
    return 1;
  }

  const WindowSystemInfo wsi = s_platform->GetWindowSystemInfo();

  UICommon::SetUserDirectory(user_directory);
  UICommon::Init();
  UICommon::InitControllers(wsi);

  Common::ScopeGuard ui_common_guard([] {
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  });

  if (save_state_path && !game_specified)
  {
    fprintf(stderr, "A save state cannot be loaded without specifying a game to launch.\n");
    return 1;
  }

  auto core_state_changed_hook = Core::AddOnStateChangedCallback([](const Core::State state) {
    if (state == Core::State::Uninitialized)
      s_platform->Stop();
  });

#ifdef _WIN32
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#else
  // Shut down cleanly on SIGINT and SIGTERM
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif

  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
  {
    fprintf(stderr, "Could not boot the specified file\n");
    return 1;
  }

  // Sunbright: arm headless FIFO recording. On each VI field end (deterministic
  // under FIFO/fixed speed): once `after` fields have elapsed, StartRecording;
  // once the recorder reports done, Save the .dff and RequestShutdown so the
  // process exits with the file written. The EventHook is kept alive in a static
  // for the whole run.
  static Common::EventHook s_fifo_record_hook;
  if (options.is_set("fifo_record"))
  {
    // options.get() returns an optparse::Value by value; its operator const char*
    // points into that temporary's std::string, so the string must be copied out
    // within the same full-expression (matching the platform/user option handling
    // above) — capturing a const char* first would dangle.
    static std::string s_path = static_cast<const char*>(options.get("fifo_record"));
    static int s_after = options.get("fifo_record_after");
    static int s_frames = options.get("fifo_record_frames");
    static int s_field = 0;
    static bool s_started = false;
    static bool s_finished = false;  // set by the recorder's finished callback
    static bool s_done = false;
    // Headless scripted START (oracle harness): reach an input-gated screen before recording.
    sb_pad_start_at = options.get("pad_start_at");
    sb_pad_start_dur = options.get("pad_start_frames");
    auto& system = Core::System::GetInstance();
    fprintf(stderr, "[sb-fifo] armed: record %d frames to '%s' after %d fields (pad START @%d for %d)\n",
            s_frames, s_path.c_str(), s_after, sb_pad_start_at, sb_pad_start_dur);
    s_fifo_record_hook = system.GetVideoEvents().vi_end_field_event.Register([&system] {
      if (s_done)
        return;
      ++s_field;
      sb_pad_cur_field = s_field;  // drive the scripted-pad field window
      FifoRecorder& rec = system.GetFifoRecorder();
      if (!s_started && s_field >= s_after)
      {
        s_started = true;
        // The finished callback is the only signal that all s_frames were captured:
        // it fires (FifoRecorder::WriteGPCommand) after the final frame's AddFrame once
        // the requested frame count is reached. IsRecordingDone() is NOT that signal —
        // it flips true on the first (skipped) setup EndFrame, so polling it saves a
        // 0-frame file.
        rec.StartRecording(s_frames, [] { s_finished = true; });
        fprintf(stderr, "[sb-fifo] StartRecording at field %d\n", s_field);
      }
      if (s_started && s_finished)
      {
        FifoDataFile* file = rec.GetRecordedFile();
        const bool ok = file != nullptr && file->Save(s_path);
        fprintf(stderr, "[sb-fifo] recorded %d frame(s) -> '%s' (save %s)\n",
                file != nullptr ? file->GetFrameCount() : 0, s_path.c_str(),
                ok ? "OK" : "FAILED");
        s_done = true;
        s_platform->RequestShutdown();
      }
    });
  }

  // Sunbright: headless SAVE-STATE-AT-FIELD (matched-state oracle tooling). At a
  // chosen VI field (after --pad-start-at settles a scene), write a Dolphin save
  // state, then exit. Reloading it with --save_state gives a REPRODUCIBLE frozen
  // oracle frame (framedump or FIFO-record from the same state) — the only sound
  // way to compare native vs oracle when the scene animates (camera pan / Mario
  // idle). Mutually exclusive with --fifo-record (both drive the field counter).
  static Common::EventHook s_save_state_hook;
  if (options.is_set("save_state_at") && static_cast<int>(options.get("save_state_at")) >= 0 &&
      !options.is_set("fifo_record"))
  {
    static int s_ss_at = options.get("save_state_at");
    static std::string s_ss_path = static_cast<const char*>(options.get("save_state_path"));
    static int s_ss_field = 0;
    static bool s_ss_saved = false;
    static int s_ss_saved_field = 0;
    static bool s_ss_done = false;
    // Reuse the scripted-START window so save-state-at can reach input-gated screens.
    sb_pad_start_at = options.get("pad_start_at");
    sb_pad_start_dur = options.get("pad_start_frames");
    auto& system = Core::System::GetInstance();
    // Ensure the destination directory exists (State::SaveAs does not mkdir).
    File::CreateFullPath(s_ss_path);
    fprintf(stderr, "[sb-state] armed: save state at field %d -> '%s' (pad START @%d for %d)\n",
            s_ss_at, s_ss_path.c_str(), sb_pad_start_at, sb_pad_start_dur);
    s_save_state_hook = system.GetVideoEvents().vi_end_field_event.Register([&system] {
      if (s_ss_done)
        return;
      ++s_ss_field;
      sb_pad_cur_field = s_ss_field;  // drive the scripted-pad field window
      if (!s_ss_saved && s_ss_field >= s_ss_at)
      {
        s_ss_saved = true;
        s_ss_saved_field = s_ss_field;
        State::SaveAs(system, s_ss_path);
        fprintf(stderr, "[sb-state] SaveAs queued at field %d\n", s_ss_field);
      }
      // Let the async CPU-thread save + compress/dump worker complete before we
      // tear down (SaveAs is not synchronous). ~30 fields of emulation is ample;
      // Core::Shutdown's State flush is the backstop if it somehow isn't.
      if (s_ss_saved && s_ss_field >= s_ss_saved_field + 30)
      {
        s_ss_done = true;
        fprintf(stderr, "[sb-state] state written -> '%s'; shutting down\n", s_ss_path.c_str());
        s_platform->RequestShutdown();
      }
    });
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif

  s_platform->MainLoop();
  Core::Stop(Core::System::GetInstance());

  Core::Shutdown(Core::System::GetInstance());
  s_platform.reset();

  return 0;
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> args = Common::CommandLineToUtf8Argv(GetCommandLineW());
  const int argc = static_cast<int>(args.size());
  std::vector<char*> argv(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].data();

  return main(argc, argv.data());
}

#undef main
#endif
