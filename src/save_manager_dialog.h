// save_manager_dialog.h - the SAVE MANAGER screen (F6): a full-window ImGui dialog, separate from
// the F4 settings window, in the same colours. It only shows and starts operations; the work is in
// save_store.h. Opening it does not save, restore or pause anything.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/input/input_driver.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "save_store.h"

namespace ao2dc {

// True while the screen is open: the input drivers ignore keyboard, mouse and pad meanwhile.
inline std::atomic<bool> g_save_manager_open{false};

// What the screen needs from the app.
struct SaveManagerHooks {
  std::function<saves::Context()> context;
  // Save-write coordination, null when the game runtime is not up. Begin returns an owner token
  // (0: none) only at a verified boundary (no save/profile package open or being changed), sets
  // *overlapped when another snapshot is active; End(token) releases the gate for its owner only.
  std::function<uint64_t(std::chrono::milliseconds, bool* overlapped)> begin_snapshot;
  std::function<bool(uint64_t)> end_snapshot;
  std::string shortcut_note;  // e.g. a key conflict found at startup
  std::string shortcut_key;   // current key of the F6 bind
};

// Background work of the screen outlives the screen itself (F6 closes it while a backup runs), so it
// is tracked per process: shutdown asks it to stop and waits for it before the game's runtime goes.
struct SaveManagerRuntime {
  std::mutex mutex;
  std::condition_variable done;
  int workers = 0;
  std::atomic<bool> stopping{false};
};
inline SaveManagerRuntime g_save_runtime;

// Asks running backups to stop at the next file and waits until they have ended (and released the
// snapshot gate). Returns false when they did not end within `wait`.
inline bool ShutdownSaveManager(std::chrono::milliseconds wait) {
  std::unique_lock lock(g_save_runtime.mutex);
  g_save_runtime.stopping = true;
  return g_save_runtime.done.wait_for(lock, wait, [] { return g_save_runtime.workers == 0; });
}

// Holds the snapshot gate for its lifetime; only the token it was given releases it.
class SnapshotLease {
 public:
  SnapshotLease(const SaveManagerHooks& hooks, std::chrono::milliseconds timeout) : hooks_(hooks) {
    if (hooks_.begin_snapshot)
      token_ = hooks_.begin_snapshot(timeout, &overlapped_);
  }
  ~SnapshotLease() {
    if (token_ && hooks_.end_snapshot)
      hooks_.end_snapshot(token_);
  }
  SnapshotLease(const SnapshotLease&) = delete;
  SnapshotLease& operator=(const SnapshotLease&) = delete;
  bool held() const { return token_ != 0; }
  bool overlapped() const { return overlapped_; }

 private:
  const SaveManagerHooks& hooks_;
  uint64_t token_ = 0;
  bool overlapped_ = false;
};

class SaveManagerDialog : public rex::ui::ImGuiDialog {
 public:
  // Opens the screen, or asks the open one to close.
  static void Toggle(rex::ui::ImGuiDrawer* drawer, const SaveManagerHooks& hooks) {
    if (instance_) {
      instance_->close_requested_ = true;
      return;
    }
    new SaveManagerDialog(drawer, hooks);  // deletes itself when it closes
  }

 private:
  struct Shared {
    std::mutex mutex;
    std::string status = "Ready.";
    bool status_ok = true;
    bool busy = false;
    bool dirty = true;  // list needs a refresh
    uint64_t changes = 0;  // bumped by every status change, so a refresh that raced one is repeated
    std::vector<saves::BackupInfo> backups;
    std::vector<saves::Pending> pending;
    std::vector<std::string> launch_lines;
    std::vector<std::string> profiles;
    std::string backups_root;
  };

  SaveManagerDialog(rex::ui::ImGuiDrawer* drawer, const SaveManagerHooks& hooks)
      : rex::ui::ImGuiDialog(drawer), hooks_(hooks), shared_(SharedState()) {
    instance_ = this;
    g_save_manager_open = true;
    // Mouse only: the game gets no pad or keyboard input while this screen is open, and the screen
    // itself uses neither pad nor keyboard navigation (only F6/Escape close it).
    rex::input::InputDriver::SetCaptured(true);
    ImGuiIO& io = GetIO();
    previous_nav_flags_ = io.ConfigFlags;
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    Refresh();
  }

  ~SaveManagerDialog() override {
    ImGuiIO& io = GetIO();
    io.ConfigFlags = previous_nav_flags_;
    rex::input::InputDriver::SetCaptured(false);
    g_save_manager_open = false;
    instance_ = nullptr;
  }

  // ---- background work -------------------------------------------------------------------

  // One state for the whole process: closing and reopening the screen during a backup shows the
  // same busy state and result.
  static std::shared_ptr<Shared> SharedState() {
    static std::shared_ptr<Shared> state = std::make_shared<Shared>();
    return state;
  }

  // Marks an operation as running; false when one already is (operations never overlap).
  static bool TryBegin(const std::shared_ptr<Shared>& s, const std::string& text) {
    std::lock_guard lock(s->mutex);
    if (s->busy)
      return false;
    s->status = text;
    s->status_ok = true;
    s->busy = true;
    s->dirty = true;
    ++s->changes;
    return true;
  }

  // Runs `body` on a tracked worker thread with a top-level handler: whatever happens the busy state
  // ends and the screen shows a result. Refused while the game is shutting down.
  template <typename F>
  static void RunWorker(const std::shared_ptr<Shared>& s, F body) {
    {
      std::lock_guard lock(g_save_runtime.mutex);
      if (g_save_runtime.stopping) {
        SetStatus(s, "The game is closing; nothing was started.", false);
        return;
      }
      ++g_save_runtime.workers;
    }
    std::thread([s, body = std::move(body)]() mutable {
      try {
        body();
      } catch (const std::exception& e) {
        SetStatus(s, std::string("The operation failed unexpectedly: ") + e.what(), false);
      } catch (...) {
        SetStatus(s, "The operation failed unexpectedly.", false);
      }
      std::lock_guard lock(g_save_runtime.mutex);
      --g_save_runtime.workers;
      g_save_runtime.done.notify_all();
    }).detach();
  }

  static void SetStatus(const std::shared_ptr<Shared>& s, const std::string& text, bool ok, bool busy = false) {
    std::lock_guard lock(s->mutex);
    s->status = text;
    s->status_ok = ok;
    s->busy = busy;
    s->dirty = true;
    ++s->changes;
  }

  void Refresh() {
    auto s = shared_;
    const saves::Context ctx = hooks_.context();
    uint64_t seen = 0;
    {
      std::lock_guard lock(s->mutex);
      seen = s->changes;
    }
    std::thread([s, ctx, seen] {
      auto backups = saves::ListBackups(ctx.backups_root);
      auto pending = saves::ListPending(ctx.user_root);
      auto lines = saves::LastLaunchTaskLines(ctx.user_root);
      auto profiles = saves::ListProfiles(ctx.user_root);
      std::lock_guard lock(s->mutex);
      s->backups = std::move(backups);
      s->pending = std::move(pending);
      s->launch_lines = std::move(lines);
      s->profiles = std::move(profiles);
      s->backups_root = ctx.backups_root.string();
      if (s->changes == seen)
        s->dirty = false;
    }).detach();
  }

  void StartBackup() {
    auto s = shared_;
    if (!TryBegin(s, "Backing up saved progress..."))
      return;
    const SaveManagerHooks hooks = hooks_;
    RunWorker(s, [s, hooks] {
      saves::Context ctx = hooks.context();
      ctx.cancel = &g_save_runtime.stopping;
      saves::Result result;
      bool queue = false, overlapped = false;
      {
        SnapshotLease lease(hooks, std::chrono::milliseconds(3000));
        if (lease.held()) {
          // No save or profile package is open and none can open until the lease ends: a complete save.
          result = saves::CreateBackup(ctx, "manual", "Save Manager (in game)");
        } else if (lease.overlapped()) {
          overlapped = true;
        } else {
          queue = true;
        }
      }  // the gate is released here, before anything is reported
      if (overlapped) {
        SetStatus(s, "Another backup is already running.", false);
      } else if (queue) {
        // The game kept a save package open (or the runtime is not up): do not copy files that may be
        // half written. Run the backup at the next launch, before the game starts.
        const saves::Result q = saves::QueueBackup(ctx);
        SetStatus(s,
                  q.ok ? "QUEUED - the game was still writing saved data. The backup will run the next time "
                         "the game starts, before it opens any save file, and appear here as Complete only "
                         "after it exists and has been verified."
                       : q.message,
                  q.ok);
      } else {
        SetStatus(s, result.ok ? result.message + " Verified." : result.message, result.ok);
      }
    });
  }

  void StartImport(const std::filesystem::path& folder) {
    auto s = shared_;
    if (!TryBegin(s, "Importing saved progress from " + folder.string() + "..."))
      return;
    saves::Context ctx = hooks_.context();
    ctx.cancel = &g_save_runtime.stopping;
    RunWorker(s, [s, ctx, folder] {
      const saves::Result r = saves::ImportFromInstallation(ctx, folder);
      SetStatus(s, r.ok ? "Imported as a new backup. " + r.message : r.message, r.ok);
    });
  }

  void QueueRestore(const std::string& id) {
    const saves::Result r = saves::QueueRestore(hooks_.context(), id);
    SetStatus(shared_, r.message, r.ok);
  }

  // ---- drawing -----------------------------------------------------------------------------

  static std::string LocalTime(const std::string& utc) {
    std::tm tm{};
    if (sscanf_s(utc.c_str(), "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                 &tm.tm_min, &tm.tm_sec) != 6)
      return utc.empty() ? "-" : utc;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const std::time_t t = _mkgmtime(&tm);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return buf;
  }

  static std::string Size(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1u << 20))
      snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / (1 << 20));
    else
      snprintf(buf, sizeof(buf), "%.1f KB", double(bytes) / 1024.0);
    return buf;
  }

  static bool AnyInputDown() {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
      if (ImGui::IsKeyDown(static_cast<ImGuiKey>(k)))
        return true;
    return false;
  }

  void OnDraw(ImGuiIO& io) override {
    constexpr ImVec4 kOrange(0.95f, 0.47f, 0.12f, 1.00f);
    constexpr ImVec4 kOrangeDim(0.62f, 0.29f, 0.07f, 1.00f);
    constexpr ImVec4 kText(0.90f, 0.88f, 0.84f, 1.00f);
    constexpr ImVec4 kTextDim(0.55f, 0.53f, 0.50f, 1.00f);
    constexpr ImVec4 kPanel(0.07f, 0.07f, 0.08f, 0.97f);
    constexpr ImVec4 kFrame(0.16f, 0.16f, 0.17f, 1.00f);
    constexpr ImVec4 kGood(0.45f, 0.80f, 0.45f, 1.00f);
    constexpr ImVec4 kBad(0.95f, 0.40f, 0.30f, 1.00f);

    // The same theme as the F4 settings window.
    int colors = 0;
    auto color = [&](ImGuiCol idx, const ImVec4& c) {
      ImGui::PushStyleColor(idx, c);
      ++colors;
    };
    color(ImGuiCol_WindowBg, kPanel);
    color(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    color(ImGuiCol_PopupBg, kPanel);
    color(ImGuiCol_Border, ImVec4(0.95f, 0.47f, 0.12f, 0.45f));
    color(ImGuiCol_Text, kText);
    color(ImGuiCol_TextDisabled, kTextDim);
    color(ImGuiCol_TitleBg, ImVec4(0.10f, 0.09f, 0.08f, 1.0f));
    color(ImGuiCol_TitleBgActive, ImVec4(0.18f, 0.10f, 0.04f, 1.0f));
    color(ImGuiCol_FrameBg, kFrame);
    color(ImGuiCol_Button, ImVec4(0.20f, 0.19f, 0.18f, 1.0f));
    color(ImGuiCol_ButtonHovered, kOrangeDim);
    color(ImGuiCol_ButtonActive, kOrange);
    color(ImGuiCol_Header, kOrangeDim);
    color(ImGuiCol_HeaderHovered, ImVec4(0.75f, 0.36f, 0.09f, 1.0f));
    color(ImGuiCol_HeaderActive, kOrange);
    color(ImGuiCol_Separator, ImVec4(0.95f, 0.47f, 0.12f, 0.35f));
    color(ImGuiCol_TableHeaderBg, ImVec4(0.14f, 0.13f, 0.12f, 1.0f));
    color(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.03f));
    color(ImGuiCol_TableBorderStrong, ImVec4(0.95f, 0.47f, 0.12f, 0.35f));
    color(ImGuiCol_TableBorderLight, ImVec4(0.95f, 0.47f, 0.12f, 0.15f));
    color(ImGuiCol_NavHighlight, kOrange);
    color(ImGuiCol_ScrollbarBg, ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
    color(ImGuiCol_ScrollbarGrab, kOrangeDim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));

    // Snapshot of the shared state.
    std::vector<saves::BackupInfo> backups;
    std::vector<saves::Pending> pending;
    std::vector<std::string> launch_lines, profiles;
    std::string status, root;
    bool status_ok, busy, dirty;
    {
      std::lock_guard lock(shared_->mutex);
      backups = shared_->backups;
      pending = shared_->pending;
      launch_lines = shared_->launch_lines;
      profiles = shared_->profiles;
      status = shared_->status;
      status_ok = shared_->status_ok;
      busy = shared_->busy;
      dirty = shared_->dirty;
      root = shared_->backups_root;
    }
    if (dirty && !refresh_in_flight_) {
      refresh_in_flight_ = true;
      Refresh();
    } else if (!dirty) {
      refresh_in_flight_ = false;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    if (ImGui::Begin("ARMY OF TWO: THE DEVIL'S CARTEL  |  SAVE MANAGER##ao2save", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::TextColored(kTextDim, "%s closes this screen, so does Esc.  Opening it saves and restores nothing.",
                         hooks_.shortcut_key.empty() ? "F6" : hooks_.shortcut_key.c_str());
      ImGui::Spacing();
      ImGui::TextColored(kOrange, "WHAT A BACKUP CONTAINS");
      ImGui::Separator();
      ImGui::TextWrapped(
          "A backup keeps your saved checkpoints and campaign progress (the files the game itself saved: "
          "profile, saved games and unlocks). It does not keep where your character stands right now, "
          "and it is not an emulator save state. The game may be saving at any moment; a backup is only "
          "taken when no save is being written, otherwise it is queued and runs the next time the game "
          "starts, before it opens any save file.");
      ImGui::TextWrapped(
          "Restoring replaces your current saved progress with the selected backup. It is never done "
          "while the game runs: you confirm, close the game, and it is applied when the game starts, "
          "after a recovery backup of what you have now.");
      ImGui::Spacing();

      ImGui::BeginDisabled(busy);
      if (ImGui::Button("  Back Up Saved Progress  "))
        StartBackup();
      ImGui::SameLine();
      const bool can_restore = selected_ >= 0 && selected_ < int(backups.size()) && backups[selected_].valid;
      ImGui::BeginDisabled(!can_restore);
      if (ImGui::Button("  Restore Backup...  "))
        open_confirm_ = true;
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("  Import From Older Release...  ")) {
        const std::filesystem::path folder =
            saves::PickFolder(L"Choose the older release folder (the one with ao2dc_manifest.toml) or its user folder");
        if (!folder.empty())
          StartImport(folder);
      }
      ImGui::SameLine();
      if (ImGui::Button("  Open Backup Folder  "))
        saves::OpenFolder(hooks_.context().backups_root);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("  Return to Game  "))
        close_requested_ = true;
      if (!can_restore)
        ImGui::TextColored(kTextDim, "Select a complete backup in the list to restore it.");

      ImGui::Spacing();
      ImGui::TextColored(status_ok ? (busy ? kOrange : kGood) : kBad, "%s", status.c_str());
      ImGui::TextColored(kTextDim, "Backups are stored in: %s", root.empty() ? "(Saved Games folder not found)" : root.c_str());
      if (!hooks_.shortcut_note.empty())
        ImGui::TextColored(kBad, "%s", hooks_.shortcut_note.c_str());
      for (const std::string& l : launch_lines)
        ImGui::TextColored(kTextDim, "At last start: %s", l.c_str());

      ImGui::Spacing();
      ImGui::TextColored(kOrange, "BACKUPS");
      ImGui::Separator();
      if (ImGui::BeginTable("##backups", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Resizable,
                            ImVec2(0, ImGui::GetContentRegionAvail().y - 4.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int pending_index = 0;
        for (const saves::Pending& p : pending) {
          ImGui::TableNextRow();
          ImGui::PushID(1000 + pending_index++);
          ImGui::TableSetColumnIndex(0);
          ImGui::TextColored(kOrange, "%s", LocalTime(p.requested).c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextColored(kOrange, "%s", p.action == "backup" ? "Queued backup" : "Queued restore");
          ImGui::TableSetColumnIndex(4);
          ImGui::TextColored(kOrange, "Queued - runs when the game next starts, before it opens save files%s",
                             p.action == "restore" ? (" (restore of " + p.backup + ")").c_str() : "");
          ImGui::SameLine();
          if (ImGui::SmallButton("Cancel")) {
            saves::CancelPending(p);
            SetStatus(shared_, "Queued item cancelled.", true);
          }
          ImGui::PopID();
        }
        int index = 0;
        for (const saves::BackupInfo& b : backups) {
          ImGui::TableNextRow();
          ImGui::PushID(index);
          ImGui::TableSetColumnIndex(0);
          const bool selected = selected_ == index;
          if (ImGui::Selectable(LocalTime(b.created).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            selected_ = index;
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(b.valid ? Size(b.bytes).c_str() : "-");
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(b.profiles.empty() ? "-" : b.profiles[0].c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(b.kind == "manual"    ? "Manual"
                                 : b.kind == "import"  ? "Imported"
                                 : b.kind == "recovery" ? "Recovery"
                                 : b.kind == "queued"  ? "Queued run"
                                                       : "-");
          ImGui::TableSetColumnIndex(4);
          ImGui::TextColored(b.valid ? kGood : kBad, "%s", b.status.c_str());
          ImGui::PopID();
          ++index;
        }
        if (backups.empty() && pending.empty()) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(4);
          ImGui::TextColored(kTextDim, "No backups yet.");
        }
        ImGui::EndTable();
      }

      // Restore confirmation: names the backup and the profile it will be applied to.
      if (open_confirm_) {
        ImGui::OpenPopup("Confirm restore##ao2save");
        open_confirm_ = false;
        confirm_open_ = true;
      }
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("Confirm restore##ao2save", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (selected_ >= 0 && selected_ < int(backups.size())) {
          const saves::BackupInfo& b = backups[selected_];
          ImGui::Text("Backup:   %s  (%s, %s, %zu files)", b.id.c_str(), LocalTime(b.created).c_str(),
                      Size(b.bytes).c_str(), b.file_count);
          std::string replaced;
          for (const std::string& p : b.profiles)
            replaced += (replaced.empty() ? "" : ", ") + p;
          ImGui::Text("Profiles replaced by this backup:  %s", replaced.empty() ? "-" : replaced.c_str());
          std::string current;
          for (const std::string& p : profiles)
            current += (current.empty() ? "" : ", ") + p;
          ImGui::Text("Profiles with saved progress here now:  %s", current.empty() ? "(none)" : current.c_str());
          for (const std::string& p : b.profiles) {
            if (std::find(profiles.begin(), profiles.end(), p) == profiles.end())
              ImGui::TextColored(kBad,
                                 "Warning: profile %s has no saved progress in this installation. It is restored under "
                                 "that profile as it is, not moved to another one; the game only sees it when it is "
                                 "signed in as that profile.", p.c_str());
          }
          ImGui::Spacing();
          ImGui::PushTextWrapPos(560.0f);
          ImGui::TextWrapped(
              "Your current saved progress for this profile will be replaced by this backup. A recovery "
              "backup of what you have now is created and verified first, and nothing is changed if that "
              "fails. The restore is applied the next time the game starts, before it opens any save "
              "file - not now. Close the game and start it again to apply it.");
          ImGui::PopTextWrapPos();
          ImGui::Spacing();
          if (ImGui::Button("  Queue restore  ")) {
            QueueRestore(b.id);
            ImGui::CloseCurrentPopup();
            confirm_open_ = false;
          }
          ImGui::SameLine();
          if (ImGui::Button("  Cancel  ") ||
              (ImGui::IsWindowFocused() && ImGui::IsKeyReleased(ImGuiKey_Escape))) {
            ImGui::CloseCurrentPopup();
            confirm_open_ = false;
          }
        } else {
          ImGui::CloseCurrentPopup();
          confirm_open_ = false;
        }
        ImGui::EndPopup();
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(colors);

    // Escape (on release, so the game never sees it) and F6/Return to Game close the screen, but only
    // once no key or mouse button is held: a button still down when input returns to the game would
    // fire there.
    if (!confirm_open_ && ImGui::IsKeyReleased(ImGuiKey_Escape))
      close_requested_ = true;
    if (close_requested_) {
      bool mouse_down = false;
      for (int b = 0; b < IM_ARRAYSIZE(io.MouseDown); ++b)
        mouse_down = mouse_down || io.MouseDown[b];
      if (!mouse_down && !AnyInputDown())
        Close();
    }
  }

  inline static SaveManagerDialog* instance_ = nullptr;
  SaveManagerHooks hooks_;
  std::shared_ptr<Shared> shared_;
  ImGuiConfigFlags previous_nav_flags_ = 0;
  int selected_ = -1;
  bool open_confirm_ = false;
  bool confirm_open_ = false;
  bool close_requested_ = false;
  bool refresh_in_flight_ = false;
};

}  // namespace ao2dc
