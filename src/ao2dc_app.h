// ao2dc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/string.h>
#include <rex/rex_app.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/ui/keybinds.h>

#include "save_manager_dialog.h"
#include "save_store.h"

using namespace ao2dc;

class Ao2dcApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ao2dcApp>(new Ao2dcApp(ctx, "ArmyOfTwoTDC X360 Recomp",
        PPCImageConfig));
  }

  // The project root: the first folder up from the executable that holds the manifest.
  static std::filesystem::path ProjectRoot() {
    namespace fs = std::filesystem;
    fs::path dir = rex::filesystem::GetExecutableFolder();
    for (int i = 0; i < 6 && !dir.empty() && dir.has_parent_path(); ++i) {
      if (fs::exists(dir / "ao2dc_manifest.toml"))
        return dir;
      dir = dir.parent_path();
    }
    return rex::filesystem::GetExecutableFolder();
  }

  // Game data comes from assets\ (the user's own disc dump, read-only); everything the game
  // or runtime writes goes to user\, cache\ and settings\ in the project instead. The SDK
  // fills in defaults (user and cache under Documents\ao2dc) before this runs, so the project
  // paths replace them unless a --user_data_root / --cache_root override was given.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::filesystem::path root = ProjectRoot();
    if (paths.game_data_root.empty())
      paths.game_data_root = root / "assets";
    if (REXCVAR_GET(user_data_root).empty())
      paths.user_data_root = root / "user";
    if (REXCVAR_GET(cache_root).empty())
      paths.cache_root = root / "cache";
    // Title update, mounted as update: (the game reads its patched data and package.mft there): the
    // package as copied from the console in title_update\ (the build checks it is Title Update 1),
    // or an already extracted update\ folder.
    if (paths.update_data_root.empty()) {
      std::error_code ec;
      if (std::filesystem::is_directory(root / "title_update", ec)) {
        for (const auto& e : std::filesystem::directory_iterator(root / "title_update", ec)) {
          const auto ext = e.path().extension().string();
          if (e.is_regular_file(ec) && ext != ".md" && ext != ".txt") {
            paths.update_data_root = e.path();
            break;
          }
        }
      }
      if (paths.update_data_root.empty() && std::filesystem::exists(root / "update", ec))
        paths.update_data_root = root / "update";
    }
    paths.config_path = root / "settings" / "hardware.toml";
    RunSaveLaunchTasks(paths);
  }

  // ---- Save Manager ---------------------------------------------------------------------

  static std::string BuildInfo() {
    return std::string("AO2DC Recomp, built ") + __DATE__ + " " + __TIME__;
  }

  saves::Context SaveContext() const {
    saves::Context ctx;
    ctx.user_root = save_user_root_;
    ctx.backups_root = saves::DefaultBackupsRoot();
    ctx.build = BuildInfo() + (save_title_update_ ? "; Title Update 1" : "; disc version");
    ctx.log = [](const std::string& line) { REXLOG_INFO("{}", line); };
    return ctx;
  }

  // Before the runtime (and so the guest) exists: finish or undo an interrupted restore, then run
  // the backups and restores queued from the Save Manager. Nothing can have a save file open here.
  void RunSaveLaunchTasks(const rex::PathConfig& paths) {
    save_user_root_ = paths.user_data_root;
    save_title_update_ = !paths.update_data_root.empty();
    if (save_user_root_.empty())
      return;
    save_run_lock_ = std::make_unique<saves::NamedLock>("RUN", save_user_root_);
    if (!save_run_lock_->acquired()) {
      save_launch_log_.push_back(
          "Another instance of the game is using these saves: queued backups and restores wait for it to close.");
      return;
    }
    saves::Context ctx = SaveContext();
    ctx.log = nullptr;  // the log is not up yet; the returned lines are printed in OnPreLaunchModule
    for (const std::string& line : saves::RunLaunchTasks(ctx))
      save_launch_log_.push_back(line);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ui::RegisterBind("bind_save_manager", "F6", "Toggle the Save Manager screen", [this, drawer] {
      saves::Context ctx = SaveContext();
      SaveManagerHooks hooks;
      hooks.context = [this] { return SaveContext(); };
      hooks.begin_snapshot = [this](std::chrono::milliseconds timeout) {
        auto* cm = runtime() && runtime()->kernel_state() ? runtime()->kernel_state()->content_manager() : nullptr;
        return cm && cm->BeginSaveSnapshot(timeout);
      };
      hooks.end_snapshot = [this] {
        if (auto* cm = runtime() && runtime()->kernel_state() ? runtime()->kernel_state()->content_manager() : nullptr)
          cm->EndSaveSnapshot();
      };
      hooks.shortcut_key = rex::cvar::GetFlagByName("bind_save_manager");
      hooks.shortcut_note = SaveManagerKeyConflict(hooks.shortcut_key);
      SaveManagerDialog::Toggle(drawer, hooks);
    });
    const std::string key = rex::cvar::GetFlagByName("bind_save_manager");
    const std::string conflict = SaveManagerKeyConflict(key);
    if (conflict.empty())
      REXLOG_INFO("Save Manager: shortcut {} is not used by any other binding", key);
    else
      REXLOG_WARN("Save Manager: {}", conflict);
  }

  // A message when another bind (overlay shortcuts or game controls) uses the Save Manager's key.
  static std::string SaveManagerKeyConflict(const std::string& key) {
    if (key.empty())
      return {};
    for (auto& entry : rex::cvar::GetRegistry()) {
      const std::string name = entry.name;
      if (name == "bind_save_manager" || (name.rfind("bind_", 0) != 0 && name.rfind("keybind_", 0) != 0))
        continue;
      const std::string value = entry.getter();
      size_t pos = 0;
      while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        if (comma == std::string::npos)
          comma = value.size();
        std::string token = value.substr(pos, comma - pos);
        token.erase(0, token.find_first_not_of(' '));
        token.erase(token.find_last_not_of(' ') + 1);
        if (token == key)
          return "The Save Manager shortcut " + key + " is also bound to " + name +
                 ". Change one of them (bind_save_manager in settings\\hardware.toml).";
        pos = comma + 1;
      }
    }
    return {};
  }

  // Runs `work` on a worker thread while this (window) thread keeps handling window messages, so the
  // window does not show "Not Responding" during the first-start HD install (about 1.4 GB, tens of
  // seconds from a hard disk). One progress line every 10 s. A close request made meanwhile is
  // re-posted when the work is done.
  template <typename F>
  static auto RunPumped(F&& work) {
    using Result = decltype(work());
    Result result{};
    std::atomic<bool> done{false};
    std::thread worker([&] {
      result = work();
      done = true;
    });
    const auto start = std::chrono::steady_clock::now();
    auto next_log = start + std::chrono::seconds(10);
    bool quit = false;
    int quit_code = 0;
    while (!done) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          quit = true;
          quit_code = int(msg.wParam);
          continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
      if (std::chrono::steady_clock::now() >= next_log) {
        next_log += std::chrono::seconds(10);
        REXLOG_INFO("HD content: still working ({} s)",
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count());
      }
    }
    worker.join();
    if (quit)
      PostQuitMessage(quit_code);
    return result;
  }

  // Why a package cannot be installed for this title, or "" when its header is a marketplace
  // package (content type 2) for it.
  static std::string PackageProblem(const std::filesystem::path& path, uint32_t title_id) {
    uint8_t header[0x3A0] = {};
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)))
      return "not a LIVE package (too short)";
    auto be32 = [&](size_t o) {
      return uint32_t(header[o]) << 24 | uint32_t(header[o + 1]) << 16 | uint32_t(header[o + 2]) << 8 |
             header[o + 3];
    };
    const std::string magic(reinterpret_cast<const char*>(header), 4);
    if ((magic != "LIVE" && magic != "PIRS" && magic != "CON ") || be32(0x344) != 2 ||
        be32(0x360) != title_id)
      return fmt::format("magic '{}', content type {:#x}, title {:08X}; need a marketplace package "
                         "for {:08X}", magic, be32(0x344), be32(0x360), title_id);
    // The STFS reader aborts the game on a package whose data is cut short, so refuse one that is
    // smaller than its own header says: the allocated blocks plus their level-0 hash blocks, at
    // least (the real size is a little larger: header and higher hash levels).
    const uint64_t blocks = be32(0x379 + 0x1C);
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (!ec && size < (blocks + blocks / 0xAA) * 0x1000)
      return fmt::format("incomplete copy: the header lists {} blocks (at least {} bytes) but the "
                         "file has {} bytes", blocks, (blocks + blocks / 0xAA) * 0x1000, size);
    return {};
  }

  // First file of `root`'s package that is missing from `installed` or has the wrong size ("" when
  // every file is there at its full size); `files` and `bytes` total the package.
  static std::string FirstIncompleteFile(rex::filesystem::Entry* root,
                                         const std::filesystem::path& installed, uint64_t* files,
                                         uint64_t* bytes) {
    std::string bad;
    std::vector<rex::filesystem::Entry*> stack{root};
    while (!stack.empty()) {
      rex::filesystem::Entry* entry = stack.back();
      stack.pop_back();
      for (const auto& child : entry->children())
        stack.push_back(child.get());
      if (entry->attributes() & rex::filesystem::kFileAttributeDirectory)
        continue;
      ++*files;
      *bytes += entry->size();
      const std::filesystem::path file =
          installed / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));
      std::error_code ec;
      if (bad.empty() && (!std::filesystem::is_regular_file(file, ec) ||
                          std::filesystem::file_size(file, ec) != entry->size()))
        bad = entry->path();
    }
    return bad;
  }

  // The HD texture pack ("Army of TWO TDC HD Content", 1.4 GB: Data\Xenon\HD.sb and HD.toc) is
  // part of the disc dump: assets\Content\0000000000000000\454109AB\00000002\454109AB00000000.
  // Install it from there (no copy into dlc\ needed) with the same installer the DLC uses, into
  // user\ where the game's content enumeration finds it. A complete earlier install (every file at
  // its full size, plus the .header) is reused, so only the first start extracts. Nothing under
  // assets\ or dlc\ is touched; a missing or unusable package only logs why the HD textures are
  // not available. Returns the package file name when it is installed and complete.
  std::string InstallHdContent(rex::system::KernelState* kernel,
                               rex::system::xam::ContentManager* content) {
    namespace fs = std::filesystem;
    const std::string title = fmt::format("{:08X}", kernel->title_id());
    const std::string name = title + "00000000";
    // One installer at a time: a second instance started meanwhile would write the same files.
    struct Lock {
      HANDLE handle = CreateMutexW(nullptr, TRUE, L"Local\\AO2DC_HD_CONTENT_INSTALL");
      bool busy = GetLastError() == ERROR_ALREADY_EXISTS;
      ~Lock() {
        if (handle)
          CloseHandle(handle);
      }
    } lock;
    if (lock.busy) {
      REXLOG_WARN("HD content: another instance of the game is installing it; skipped in this one");
      return {};
    }
    const fs::path source = runtime()->game_data_root() / "Content" / "0000000000000000" / title /
                            "00000002" / name;
    std::error_code ec;
    const fs::path user = runtime()->user_data_root() / "0000000000000000" / title;
    const fs::path installed = user / "00000002" / name;
    const fs::path header = user / "Headers" / "00000002" / (name + ".header");
    if (!fs::is_regular_file(source, ec)) {
      if (fs::is_regular_file(header, ec))
        REXLOG_WARN("HD content: no package at {}; an earlier install in {} is used as it is, not "
                    "checked", rex::path_to_utf8(source), rex::path_to_utf8(installed));
      else
        REXLOG_INFO("HD content: no package at {} (optional: it comes from your own disc dump); the "
                    "game runs without the HD textures", rex::path_to_utf8(source));
      return {};
    }
    if (const std::string problem = PackageProblem(source, kernel->title_id()); !problem.empty()) {
      REXLOG_WARN("HD content: {} skipped ({})", rex::path_to_utf8(source), problem);
      return {};
    }
    rex::filesystem::StfsContainerDevice device("", source);
    auto* root = device.Initialize() ? device.ResolvePath("") : nullptr;
    if (!root) {
      REXLOG_WARN("HD content: {} cannot be read as an STFS package (incomplete or damaged copy?); "
                  "skipped", rex::path_to_utf8(source));
      return {};
    }
    uint64_t files = 0, bytes = 0;
    std::string bad = FirstIncompleteFile(root, installed, &files, &bytes);
    if (bad.empty() && !fs::is_regular_file(header, ec))
      bad = "(no install record)";
    if (bad.empty()) {
      REXLOG_INFO("HD content: already installed ({} files, {} bytes); reused", files, bytes);
      return name;
    }

    const auto space = fs::space(runtime()->user_data_root(), ec);
    if (!ec && space.available < bytes + (64ull << 20)) {
      REXLOG_ERROR("HD content: not enough free space for {} (need {} bytes, {} free); not installed",
                   rex::path_to_utf8(runtime()->user_data_root()), bytes, uint64_t(space.available));
      return {};
    }
    REXLOG_INFO("HD content: installing {} ({} files, {} bytes; reason: {}); the first start takes "
                "a moment", rex::path_to_utf8(source), files, bytes, bad);
    const auto result = content->InstallContent(source);
    // InstallContent writes the .header last and does not check what it extracted: verify, and drop
    // the record of a damaged install so the game does not enumerate it (the next start retries).
    uint64_t files_after = 0, bytes_after = 0;
    const std::string after = FirstIncompleteFile(root, installed, &files_after, &bytes_after);
    if (uint32_t(result) != 0 || !after.empty()) {
      fs::remove(header, ec);
      REXLOG_ERROR("HD content: install FAILED (result {:#x}{}); the HD textures are off and the next "
                   "start retries. Check the free disk space.", uint32_t(result),
                   after.empty() ? "" : ", " + after + " is incomplete");
      return {};
    }
    REXLOG_INFO("HD content: installed {} -> result {:#x}", name, uint32_t(result));
    return name;
  }

  // Marketplace content (DLC): LIVE packages placed in dlc\ are installed into user\ the way a
  // console installs them to its HDD (user\0000000000000000\454109AB\00000002\<package>\ plus
  // a .header with the package's license bits), so the game finds them through its normal
  // content enumeration. Only marketplace packages (content type 2) for this title are taken;
  // anything else in dlc\ is skipped with a log line. The packages themselves are not modified.
  void OnPreLaunchModule() override {
    namespace fs = std::filesystem;
    for (const std::string& line : save_launch_log_)
      REXLOG_INFO("Save Manager: {}", line);
    save_launch_log_.clear();
    // While the Save Manager is open the keyboard, mouse and pad belong to it, not the game. (The
    // SDK's own callback only knows its overlays.)
    if (runtime() && runtime()->input_system() && imgui_drawer()) {
      auto* drawer = imgui_drawer();
      static_cast<rex::input::InputSystem*>(runtime()->input_system())->SetActiveCallback([drawer] {
        if (g_save_manager_open)
          return false;
        return !drawer->GetIO().WantCaptureMouse;
      });
    }
    const fs::path dir = ProjectRoot() / "dlc";
    std::error_code ec;
    if (!runtime() || !runtime()->kernel_state())
      return;
    auto* kernel = runtime()->kernel_state();
    const std::string hd_installed =
        kernel->content_manager()
            ? RunPumped([&] { return InstallHdContent(kernel, kernel->content_manager()); })
            : "";
    if (!fs::is_directory(dir, ec))
      return;
    auto* content = kernel->content_manager();
    if (!content)
      return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const auto ext = entry.path().extension().string();
      if (!entry.is_regular_file(ec) || ext == ".md" || ext == ".txt")
        continue;
      if (entry.path().filename().string() == hd_installed) {
        REXLOG_INFO("DLC: {} is the HD pack already installed from assets\\; skipped", hd_installed);
        continue;
      }
      if (const std::string problem = PackageProblem(entry.path(), kernel->title_id());
          !problem.empty()) {
        REXLOG_WARN("DLC: {} skipped ({})", entry.path().filename().string(), problem);
        continue;
      }
      const auto result = content->InstallContent(entry.path());
      REXLOG_INFO("DLC: installed {} -> result {:#x}", entry.path().filename().string(),
                  uint32_t(result));
    }
  }

 private:
  std::filesystem::path save_user_root_;
  bool save_title_update_ = false;
  std::unique_ptr<saves::NamedLock> save_run_lock_;  // held for the life of the process
  std::vector<std::string> save_launch_log_;
};
