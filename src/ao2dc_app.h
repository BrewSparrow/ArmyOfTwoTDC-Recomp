// ao2dc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

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
    paths.config_path = root / "settings" / "hardware.toml";
  }
};
