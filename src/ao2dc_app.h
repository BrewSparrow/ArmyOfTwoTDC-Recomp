// ao2dc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

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
  }

  // Marketplace content (DLC): LIVE packages placed in dlc\ are installed into user\ the way a
  // console installs them to its HDD (user\0000000000000000\454109AB\00000002\<package>\ plus
  // a .header with the package's license bits), so the game finds them through its normal
  // content enumeration. Only marketplace packages (content type 2) for this title are taken;
  // anything else in dlc\ is skipped with a log line. The packages themselves are not modified.
  void OnPreLaunchModule() override {
    namespace fs = std::filesystem;
    const fs::path dir = ProjectRoot() / "dlc";
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || !runtime() || !runtime()->kernel_state())
      return;
    auto* kernel = runtime()->kernel_state();
    auto* content = kernel->content_manager();
    if (!content)
      return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const auto ext = entry.path().extension().string();
      if (!entry.is_regular_file(ec) || ext == ".md" || ext == ".txt")
        continue;
      uint8_t header[0x364] = {};
      std::ifstream in(entry.path(), std::ios::binary);
      if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) {
        REXLOG_WARN("DLC: {} is not a LIVE package (too short); skipped", entry.path().filename().string());
        continue;
      }
      auto be32 = [&](size_t o) {
        return uint32_t(header[o]) << 24 | uint32_t(header[o + 1]) << 16 |
               uint32_t(header[o + 2]) << 8 | header[o + 3];
      };
      const std::string magic(reinterpret_cast<const char*>(header), 4);
      const uint32_t content_type = be32(0x344), title_id = be32(0x360);
      if ((magic != "LIVE" && magic != "PIRS" && magic != "CON ") || content_type != 2 ||
          title_id != kernel->title_id()) {
        REXLOG_WARN("DLC: {} skipped (magic '{}', content type {:#x}, title {:08X}; need a "
                    "marketplace package for {:08X})",
                    entry.path().filename().string(), magic, content_type, title_id,
                    kernel->title_id());
        continue;
      }
      const auto result = content->InstallContent(entry.path());
      REXLOG_INFO("DLC: installed {} -> result {:#x}", entry.path().filename().string(),
                  uint32_t(result));
    }
  }
};
