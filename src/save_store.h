// save_store.h - backup / restore of saved progress for Army of Two: The Devil's Cartel.
//
// No dependency on the game runtime (std::filesystem, BCrypt and the shell only), so it can be
// tested on disposable folders.
//
// What counts as saved progress ("the scope"), relative to the user data root (user\):
//   <xuid>/454109AB/**      every file of every per-user content type of this title: saved games
//                           (00000001) with their .header records under Headers\, profile data,
//                           whatever else the game stores per user. <xuid> is any 16-digit hex
//                           folder except 0000000000000000, which holds the common content (HD
//                           pack, DLC) and is never included.
//   achievements/454109AB.toml   the unlock state.
// Not included: DLC, the HD pack, title updates, game assets, shader cache, logs, settings.
//
// Backups live in <Saved Games>\BrewSparrow\ArmyOfTwoTDC\Backups, one self-contained folder each
// (manifest.toml + files\), published by renaming a ".tmp-" folder only after every file was
// copied and re-read successfully. Restores are staged, journalled and rolled back when
// interrupted; they run only before the game starts (RunLaunchTasks).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ao2dc::saves {

namespace fs = std::filesystem;

inline constexpr const char* kTitle = "454109AB";
inline constexpr int kManifestVersion = 1;

struct FileEntry {
  std::string rel;  // relative to the user data root, forward slashes
  uint64_t size = 0;
  std::string sha256;
};

struct Result {
  bool ok = false;
  std::string message;
  std::string id;  // backup folder name created or used
  bool unresolved = false;  // a failed rollback left old saves in a recovery folder (UnresolvedRecovery)
};

struct BackupInfo {
  std::string id;  // folder name
  fs::path dir;
  std::string created;  // UTC, "2026-11-18T14:50:03Z"
  std::string kind;     // manual | import | recovery | queued
  std::vector<std::string> profiles;
  std::string source;
  std::string build;
  uint64_t bytes = 0;
  size_t file_count = 0;
  bool valid = false;
  std::string status;  // "Complete" or why not
};

struct Pending {
  std::string action;  // backup | restore
  std::string backup;  // restore: backup folder name
  std::string requested;
  fs::path file;
};

// Called with the name of each step ("copy", "verify", "stage", "commit:<n>", "rollback" ...) so
// tests can fail or "crash" an operation there. Throwing std::exception is a failure the code
// recovers from; throwing anything else simulates the process dying (nothing is cleaned up).
using FaultHook = std::function<void(const std::string&)>;
using Log = std::function<void(const std::string&)>;

struct Context {
  fs::path user_root;
  fs::path backups_root;
  std::string build;  // build information stored in manifests
  Log log;
  FaultHook fault;
  const std::atomic<bool>* cancel = nullptr;  // set by the app at shutdown: backups stop between files
};

// <Saved Games>\BrewSparrow\ArmyOfTwoTDC\Backups, empty if Windows cannot resolve the folder.
fs::path DefaultBackupsRoot();

// Profile folders (xuids) under user_root that hold saved progress for this title.
std::vector<std::string> ListProfiles(const fs::path& user_root);

// The scope under user_root as relative, forward-slash paths.
std::vector<std::string> ScopeFiles(const fs::path& user_root);

// True when `rel` is a path a backup may contain (checked before anything is written or replaced).
bool IsAllowedRelativePath(const std::string& rel);

Result CreateBackup(const Context& ctx, const std::string& kind, const std::string& source_note);

// Copies the saved progress of an older installation (its project folder or its user folder) into
// a new backup. The installation's own files are only read.
Result ImportFromInstallation(const Context& ctx, const fs::path& older);

// Backups in ctx.backups_root, newest first; every file is re-hashed.
std::vector<BackupInfo> ListBackups(const fs::path& backups_root);

// Restores a backup into ctx.user_root. Only call before the game has opened any save file.
Result RestoreBackup(const Context& ctx, const std::string& backup_id);

// Work queued from the Save Manager, per installation (user_root\.ao2dc\pending).
Result QueueBackup(const Context& ctx);
Result QueueRestore(const Context& ctx, const std::string& backup_id);
std::vector<Pending> ListPending(const fs::path& user_root);
// Cancels a queued item (deletes its file).
bool CancelPending(const Pending& pending);

// Launch-time tasks, before the guest exists: finish or roll back an interrupted restore, then run
// the queued items. Returns one status line per step (also stored for the Save Manager).
std::vector<std::string> RunLaunchTasks(const Context& ctx);

// Restore work folders (user_root\.ao2dc\restore-*) that are still there: a restore or its
// rollback could not finish and its saved files are kept in them. While any exists no restore or
// queued item runs and the game must not start (it would write saves over an unresolved state).
// Every launch retries the rollback.
std::vector<fs::path> UnresolvedRecovery(const fs::path& user_root);

// Status lines from the last RunLaunchTasks, for display.
std::vector<std::string> LastLaunchTaskLines(const fs::path& user_root);

// Exclusive lock on a file, held for the life of the process (not tied to a thread; the OS drops it
// when the process dies). One per installation: the game running with these saves.
class InstanceLock {
 public:
  explicit InstanceLock(const fs::path& user_root);
  ~InstanceLock();
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;
  bool acquired() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

// One lock per installation (named by its user folder): "another instance runs with these saves"
// and "an operation is running". Held for the object's lifetime.
class NamedLock {
 public:
  NamedLock(const char* kind, const fs::path& user_root);
  ~NamedLock();
  NamedLock(const NamedLock&) = delete;
  NamedLock& operator=(const NamedLock&) = delete;
  bool acquired() const { return acquired_; }

 private:
  void* handle_ = nullptr;
  bool acquired_ = false;
};

// Shows the Windows folder picker; returns an empty path when cancelled.
fs::path PickFolder(const wchar_t* title);

// Opens a folder in Explorer (creating it first).
void OpenFolder(const fs::path& dir);

std::string Sha256OfFile(const fs::path& file, uint64_t* size_out);

}  // namespace ao2dc::saves
