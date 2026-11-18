// save_store_test.cpp - exercises src/save_store.cpp on disposable folders.
// Build and run (from the project folder, with clang on PATH):
//   clang++ -std=c++20 -O1 tests/save_store_test.cpp src/save_store.cpp -o save_store_test.exe
//   save_store_test.exe
#include "../src/save_store.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

using namespace ao2dc::saves;

static int g_failed = 0, g_passed = 0;
static void Check(bool ok, const std::string& what) {
  if (ok) {
    ++g_passed;
    std::printf("  PASS  %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("  FAIL  %s\n", what.c_str());
  }
}

static void Write(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << data;
}
static std::string Slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static std::string Blob(char seed, size_t n) {
  std::string s(n, seed);
  for (size_t i = 0; i < n; ++i)
    s[i] = char(seed + (i * 31) % 97);
  return s;
}

static const char* kXuid = "B13EBABEBABEBABE";

static void MakeUser(const fs::path& user, int version) {
  const std::string v = std::to_string(version);
  Write(user / kXuid / "454109AB" / "00000001" / "PROFILE_OPTIONS" / "PROFILE_OPTIONS", "options-v" + v);
  Write(user / kXuid / "454109AB" / "00000001" / "CHECKPOINT_1" / "save.dat", Blob(char(version), 300000));
  Write(user / kXuid / "454109AB" / "00000001" / "CHECKPOINT_1" / "meta.bin", "meta-" + v);
  Write(user / kXuid / "454109AB" / "Headers" / "00000001" / "PROFILE_OPTIONS.header", "hdr-p-" + v);
  Write(user / kXuid / "454109AB" / "Headers" / "00000001" / "CHECKPOINT_1.header", "hdr-c-" + v);
  Write(user / "achievements" / "454109AB.toml", "[unlocked.1]\nfiletime = " + v + "\n");
  // Things that must never be part of a backup.
  Write(user / "0000000000000000" / "454109AB" / "00000002" / "454109AB00000000" / "Data" / "Xenon" / "HD.sb", "HD");
  Write(user / "0000000000000000" / "454109AB" / "Headers" / "00000002" / "454109AB00000000.header", "x");
  Write(user / kXuid / "OTHER000" / "00000001" / "foreign", "foreign");
  Write(user / "logs" / "bench.log", "log");
}

static std::map<std::string, std::string> Snapshot(const fs::path& user) {
  std::map<std::string, std::string> m;
  for (const std::string& rel : ScopeFiles(user))
    m[rel] = Slurp(user / fs::path(rel));
  return m;
}

static Context Ctx(const fs::path& user, const fs::path& backups) {
  Context c;
  c.user_root = user;
  c.backups_root = backups;
  c.build = "test build";
  return c;
}

static size_t CountDirs(const fs::path& p) {
  size_t n = 0;
  std::error_code ec;
  if (fs::is_directory(p, ec))
    for (const auto& e : fs::directory_iterator(p))
      n += e.is_directory() ? 1 : 0;
  return n;
}

static bool HasTmp(const fs::path& backups) {
  std::error_code ec;
  if (!fs::is_directory(backups, ec))
    return false;
  for (const auto& e : fs::directory_iterator(backups))
    if (e.path().filename().string().rfind(".tmp-", 0) == 0)
      return true;
  return false;
}

struct Crash {};  // thrown by fault hooks to simulate the process dying

int main() {
  const fs::path base = fs::temp_directory_path() / "ao2dc-save-store-test";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base);
  const fs::path user = base / "release-A" / "user";
  const fs::path backups = base / "Backups";

  std::printf("scope and path rules\n");
  MakeUser(user, 1);
  {
    const auto scope = ScopeFiles(user);
    Check(scope.size() == 6, "scope holds the 5 profile files and the achievements file (" + std::to_string(scope.size()) + ")");
    bool bad = false;
    for (const auto& s : scope)
      if (s.find("HD") != std::string::npos || s.find("0000000000000000") == 0 || s.find("OTHER000") != std::string::npos ||
          s.find("logs") == 0)
        bad = true;
    Check(!bad, "HD content, DLC area, other titles and logs are excluded");
    Check(ListProfiles(user).size() == 1 && ListProfiles(user)[0] == kXuid, "profile identity found from the folder name");
    Check(!IsAllowedRelativePath("../evil"), "rejects ..");
    Check(!IsAllowedRelativePath("B13EBABEBABEBABE/454109AB/../../x"), "rejects embedded ..");
    Check(!IsAllowedRelativePath("C:/Windows/x"), "rejects drive paths");
    Check(!IsAllowedRelativePath("0000000000000000/454109AB/00000002/x/y"), "rejects the common content xuid");
    Check(!IsAllowedRelativePath("B13EBABEBABEBABE/454109AB/00000001/NUL"), "rejects reserved device names");
    Check(!IsAllowedRelativePath("B13EBABEBABEBABE\\454109AB\\x\\y"), "rejects backslashes");
    Check(IsAllowedRelativePath("B13EBABEBABEBABE/454109AB/00000001/PROFILE_OPTIONS/PROFILE_OPTIONS"), "accepts a save path");
  }

  std::printf("backup storage location\n");
  {
    const fs::path d = DefaultBackupsRoot();
    const std::string s = d.generic_string();
    Check(s.size() > 30 && s.find("/BrewSparrow/ArmyOfTwoTDC/Backups") == s.size() - 33,
          "Saved Games\\BrewSparrow\\ArmyOfTwoTDC\\Backups: " + d.string());
    Check(s.find("Saved Games") != std::string::npos, "resolved through the Saved Games known folder");
  }

  std::printf("backup, listing, verification\n");
  Result b1 = CreateBackup(Ctx(user, backups), "manual", "test");
  Check(b1.ok && fs::is_regular_file(backups / b1.id / "manifest.toml"), "backup created: " + b1.message);
  Check(!HasTmp(backups), "no unfinished .tmp folder left behind");
  {
    auto list = ListBackups(backups);
    Check(list.size() == 1 && list[0].valid && list[0].status == "Complete", "listed as Complete");
    Check(list.size() == 1 && list[0].profiles.size() == 1 && list[0].profiles[0] == kXuid && list[0].file_count == 6,
          "manifest records profile identity and 6 files");
    Check(list.size() == 1 && list[0].build == "test build" && !list[0].created.empty(), "manifest records build information and time");
  }
  const auto original = Snapshot(user);

  std::printf("round trip restore\n");
  MakeUser(user, 2);                                     // newer progress
  fs::remove(user / kXuid / "454109AB" / "Headers" / "00000001" / "CHECKPOINT_1.header");
  Write(user / kXuid / "454109AB" / "00000001" / "CHECKPOINT_9" / "new.dat", "added after the backup");
  const auto before_restore = Snapshot(user);
  Result r1 = RestoreBackup(Ctx(user, backups), b1.id);
  Check(r1.ok, "restore ok: " + r1.message);
  Check(Snapshot(user) == original, "restored files are byte-identical to the backed-up state");
  Check(!fs::exists(user / kXuid / "454109AB" / "00000001" / "CHECKPOINT_9"), "a file added after the backup is gone (no old/new mixture)");
  Check(fs::exists(user / "0000000000000000" / "454109AB" / "00000002" / "454109AB00000000" / "Data" / "Xenon" / "HD.sb") &&
            fs::exists(user / "logs" / "bench.log") && fs::exists(user / kXuid / "OTHER000" / "00000001" / "foreign"),
        "HD content, logs and other titles untouched by the restore");
  {
    auto list = ListBackups(backups);
    int recovery = 0;
    for (auto& b : list)
      if (b.kind == "recovery") {
        ++recovery;
        std::string content = Slurp(b.dir / "files" / kXuid / "454109AB" / "00000001" / "PROFILE_OPTIONS" / "PROFILE_OPTIONS");
        Check(content == "options-v2" && b.valid, "recovery backup holds the progress that was replaced");
      }
    Check(recovery == 1, "exactly one recovery backup was created");
  }
  Check(!fs::exists(user / ".ao2dc" / "restore-x") && ListPending(user).empty(), "no restore work left over");

  std::printf("fresh release folder\n");
  {
    const fs::path user2 = base / "release-B" / "user";  // a new release folder, nothing in it
    Check(ListBackups(backups).size() == 2, "a new release folder discovers the backups (shared location)");
    Result r = RestoreBackup(Ctx(user2, backups), b1.id);
    Check(r.ok && Snapshot(user2) == original, "restore into the empty new release folder reproduces the progress");
  }

  std::printf("missing saves\n");
  {
    const fs::path empty = base / "release-C" / "user";
    fs::create_directories(empty);
    const size_t n = CountDirs(backups);
    Result r = CreateBackup(Ctx(empty, backups), "manual", "t");
    Check(!r.ok && r.message.find("No saved progress") != std::string::npos, "nothing to back up is reported clearly");
    Check(CountDirs(backups) == n && !HasTmp(backups), "no empty backup and no temp folder created");
    Result ri = ImportFromInstallation(Ctx(user, backups), empty);
    Check(!ri.ok, "importing from a folder without saves is refused");
  }

  std::printf("corrupt backups\n");
  {
    Result b2 = CreateBackup(Ctx(user, backups), "manual", "to corrupt");
    const fs::path f = backups / b2.id / "files" / kXuid / "454109AB" / "00000001" / "CHECKPOINT_1" / "save.dat";
    std::string data = Slurp(f);
    data[1000] ^= 0x55;
    Write(f, data);
    const auto live = Snapshot(user);
    bool found = false;
    for (auto& b : ListBackups(backups))
      if (b.id == b2.id) {
        found = true;
        Check(!b.valid && b.status.find("checksum mismatch") != std::string::npos, "flipped byte detected: " + b.status);
      }
    Check(found, "damaged backup is still listed");
    Result r = RestoreBackup(Ctx(user, backups), b2.id);
    Check(!r.ok && Snapshot(user) == live && CountDirs(user / ".ao2dc") == 0, "restore of a corrupt backup is refused and changes nothing");

    Result b3 = CreateBackup(Ctx(user, backups), "manual", "manifest");
    std::string man = Slurp(backups / b3.id / "manifest.toml");
    Write(backups / b3.id / "manifest.toml", man.substr(0, man.size() / 2));
    Result rt = RestoreBackup(Ctx(user, backups), b3.id);
    Check(!rt.ok && Snapshot(user) == live, "truncated manifest is refused");

    Result b4 = CreateBackup(Ctx(user, backups), "manual", "traversal");
    man = Slurp(backups / b4.id / "manifest.toml");
    const size_t at = man.find("path = \"B13EBABEBABEBABE/454109AB/00000001/CHECKPOINT_1/meta.bin\"");
    Write(backups / b4.id / "manifest.toml",
          man.substr(0, at) + "path = \"B13EBABEBABEBABE/454109AB/../../../escape.txt\"" +
              man.substr(at + std::string("path = \"B13EBABEBABEBABE/454109AB/00000001/CHECKPOINT_1/meta.bin\"").size()));
    Result rp = RestoreBackup(Ctx(user, backups), b4.id);
    Check(!rp.ok && rp.message.find("not saved progress") != std::string::npos && !fs::exists(base / "release-A" / "escape.txt"),
          "a manifest path that leaves the save folder is refused");

    Result b5 = CreateBackup(Ctx(user, backups), "manual", "title");
    man = Slurp(backups / b5.id / "manifest.toml");
    Write(backups / b5.id / "manifest.toml", man.replace(man.find("454109AB"), 8, "4D5307E6"));
    Check(!RestoreBackup(Ctx(user, backups), b5.id).ok, "a backup for another title is refused");

    Result b6 = CreateBackup(Ctx(user, backups), "manual", "missing file");
    fs::remove(backups / b6.id / "files" / "achievements" / "454109AB.toml");
    Check(!RestoreBackup(Ctx(user, backups), b6.id).ok && Snapshot(user) == live, "a backup with a missing file is refused");
    Check(!RestoreBackup(Ctx(user, backups), "..\\..\\x").ok && !RestoreBackup(Ctx(user, backups), "nope").ok,
          "bad or unknown backup names are refused");
  }

  std::printf("failed writes\n");
  {
    const size_t n = CountDirs(backups);
    Context c = Ctx(user, backups);
    c.fault = [](const std::string& p) {
      if (p == "copy:2")
        throw std::runtime_error("simulated disk full");
    };
    Result r = CreateBackup(c, "manual", "fail");
    Check(!r.ok && CountDirs(backups) == n && !HasTmp(backups), "failure while copying leaves no partial backup and keeps the old ones");
    c.fault = [](const std::string& p) {
      if (p == "publish")
        throw std::runtime_error("simulated failure before publishing");
    };
    r = CreateBackup(c, "manual", "fail2");
    Check(!r.ok && CountDirs(backups) == n && !HasTmp(backups), "failure before publishing leaves nothing published");

    // Restore failing while staging: nothing changed.
    MakeUser(user, 3);
    const auto now = Snapshot(user);
    c.fault = [](const std::string& p) {
      if (p == "stage:2")
        throw std::runtime_error("simulated write failure");
    };
    Result rr = RestoreBackup(c, b1.id);
    Check(!rr.ok && Snapshot(user) == now && !fs::exists(user / ".ao2dc" / "pending"), "restore failing while staging changes nothing");
    Check(CountDirs(user / ".ao2dc") == 0, "staging leftovers removed");
    // Restore failing in the middle of the swap: rolled back, no mixture.
    c.fault = [](const std::string& p) {
      if (p == "commit:7")
        throw std::runtime_error("simulated failure during the swap");
    };
    rr = RestoreBackup(c, b1.id);
    Check(!rr.ok && Snapshot(user) == now, "restore failing mid-swap rolls back to the exact previous state");
    Check(CountDirs(user / ".ao2dc") == 0, "rollback leftovers removed");
  }

  std::printf("interrupted restore recovery\n");
  {
    const auto now = Snapshot(user);
    for (const char* point : {"stage:3", "commit:2", "commit:8"}) {
      Context c = Ctx(user, backups);
      c.fault = [point](const std::string& p) {
        if (p == point)
          throw Crash{};
      };
      bool crashed = false;
      try {
        RestoreBackup(c, b1.id);
      } catch (const Crash&) {
        crashed = true;
      }
      Check(crashed, std::string("simulated process death at ") + point);
      if (std::string(point).rfind("commit", 0) == 0)
        Check(Snapshot(user) != now, std::string("(files really were in a mixed state at ") + point + ")");
      Context fresh = Ctx(user, backups);  // next launch
      auto lines = RunLaunchTasks(fresh);
      Check(Snapshot(user) == now && !lines.empty(), std::string("next launch restored the previous progress after death at ") + point +
                                                      ": " + (lines.empty() ? "" : lines[0]));
      Check(CountDirs(user / ".ao2dc") == 0 || !fs::exists(user / ".ao2dc" / "restore-x"), "no restore work left after recovery");
      bool leftover = false;
      for (const auto& e : fs::directory_iterator(user / ".ao2dc"))
        if (e.path().filename().string().rfind("restore-", 0) == 0)
          leftover = true;
      Check(!leftover, "recovery removed the restore work folder");
    }
  }

  std::printf("queued work at launch\n");
  {
    Context c = Ctx(user, backups);
    Result q = QueueRestore(c, b1.id);
    Check(q.ok && ListPending(user).size() == 1 && ListPending(user)[0].action == "restore", "restore is queued");
    auto lines = RunLaunchTasks(c);
    Check(Snapshot(user) == original && ListPending(user).empty(), "launch task applied the restore and cleared the queue");
    Check(!LastLaunchTaskLines(user).empty(), "result is kept for the Save Manager: " + (lines.empty() ? "" : lines[0]));
    const size_t n = CountDirs(backups);
    Result qb = QueueBackup(c);
    Check(qb.ok && ListPending(user).size() == 1, "backup is queued");
    RunLaunchTasks(c);
    Check(CountDirs(backups) == n + 1 && ListPending(user).empty(), "launch task created the queued backup");
    bool queued_kind = false;
    for (auto& b : ListBackups(backups))
      if (b.kind == "queued" && b.valid)
        queued_kind = true;
    Check(queued_kind, "queued backup is marked and verified");
    QueueRestore(c, "does-not-exist");
    lines = RunLaunchTasks(c);
    Check(ListPending(user).empty() && fs::exists(user / ".ao2dc" / "pending" / "failed") && !lines.empty(),
          "a queued restore that cannot run is moved to failed\\ and reported: " + (lines.empty() ? "" : lines[0]));
  }

  std::printf("import from an older release\n");
  {
    const fs::path older = base / "old-release";
    fs::create_directories(older);
    Write(older / "ao2dc_manifest.toml", "x");
    MakeUser(older / "user", 7);
    const auto older_before = Snapshot(older / "user");
    std::map<std::string, std::string> raw_before;
    for (const auto& e : fs::recursive_directory_iterator(older))
      if (e.is_regular_file())
        raw_before[e.path().string()] = Slurp(e.path());
    Result r = ImportFromInstallation(Ctx(user, backups), older);
    Check(r.ok, "import ok: " + r.message);
    std::map<std::string, std::string> raw_after;
    for (const auto& e : fs::recursive_directory_iterator(older))
      if (e.is_regular_file())
        raw_after[e.path().string()] = Slurp(e.path());
    Check(raw_before == raw_after, "the older installation's files are untouched");
    bool ok = false;
    for (auto& b : ListBackups(backups))
      if (b.id == r.id)
        ok = b.valid && b.kind == "import" && b.file_count == 6 && b.source.find("old-release") != std::string::npos;
    Check(ok, "import is a verified backup containing only save data (6 files), labelled with its source");
    // Folder given as the user folder itself:
    Check(ImportFromInstallation(Ctx(user, backups), older / "user").ok, "the user folder itself is accepted too");
    Check(Snapshot(older / "user") == older_before, "still untouched");
  }

  std::printf("operation lock\n");
  {
    NamedLock held("OP", user);
    bool refused = false;
    std::thread t([&] { refused = !CreateBackup(Ctx(user, backups), "manual", "locked").ok; });
    t.join();
    Check(held.acquired() && refused, "a second operation on the same saves is refused while one runs");
    bool other_instance = false;
    NamedLock run("RUN", user);
    std::thread t2([&] {
      NamedLock again("RUN", user);
      other_instance = !again.acquired();
    });
    t2.join();
    Check(run.acquired() && other_instance, "a second instance with the same saves is detected");
  }

  std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
  fs::remove_all(base, ec);
  return g_failed ? 1 : 0;
}
