// save_store.cpp - see save_store.h.
#define _CRT_SECURE_NO_WARNINGS
#include "save_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace ao2dc::saves {

namespace {

constexpr const char* kManifestName = "manifest.toml";
constexpr const char* kFilesDir = "files";
constexpr const char* kWorkDir = ".ao2dc";

// ---- small helpers -------------------------------------------------------------------------

void Say(const Context& ctx, const std::string& text) {
  if (ctx.log)
    ctx.log(text);
}

void Fault(const Context& ctx, const std::string& point) {
  if (ctx.fault)
    ctx.fault(point);
}

std::string Narrow(const fs::path& p) {
  return p.string();
}

std::string NowUtc() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string StampForId() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

std::string RandomHex(int n) {
  static std::mt19937_64 rng{std::random_device{}()};
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < n; ++i)
    s += digits[rng() & 15];
  return s;
}

bool IsHex16(const std::string& s) {
  if (s.size() != 16)
    return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool IsZeroXuid(const std::string& s) {
  return s.find_first_not_of('0') == std::string::npos;
}

std::string Upper(std::string s) {
  for (char& c : s)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> SplitPath(const std::string& rel) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : rel) {
    if (c == '/') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  parts.push_back(cur);
  return parts;
}

bool IsReservedName(std::string name) {
  const auto dot = name.find('.');
  if (dot != std::string::npos)
    name.resize(dot);
  name = Upper(name);
  static const char* names[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  return std::any_of(std::begin(names), std::end(names), [&](const char* n) { return name == n; });
}

// ---- SHA-256 (Windows CNG) ----------------------------------------------------------------

class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
      alg_ = nullptr;
    if (alg_ && BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0)
      hash_ = nullptr;
  }
  ~Sha256() {
    if (hash_)
      BCryptDestroyHash(hash_);
    if (alg_)
      BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void Update(const void* data, size_t size) {
    if (hash_)
      BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), 0);
  }
  std::string Final() {
    UCHAR digest[32] = {};
    if (hash_)
      BCryptFinishHash(hash_, digest, sizeof(digest), 0);
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (UCHAR b : digest) {
      s += digits[b >> 4];
      s += digits[b & 15];
    }
    return hash_ ? s : std::string();
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

struct FileStat {
  uint64_t size = 0;
  fs::file_time_type mtime{};
  bool ok = false;
  bool operator==(const FileStat& o) const { return ok && o.ok && size == o.size && mtime == o.mtime; }
};

FileStat Stat(const fs::path& p) {
  FileStat s;
  std::error_code ec;
  s.size = fs::file_size(p, ec);
  if (ec)
    return s;
  s.mtime = fs::last_write_time(p, ec);
  s.ok = !ec;
  return s;
}

// Copies src to dst (creating folders), hashing what was read; the copy is flushed to disk.
bool CopyHashed(const fs::path& src, const fs::path& dst, uint64_t* size, std::string* hash,
                std::string* error) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  FILE* in = _wfopen(src.c_str(), L"rb");
  if (!in) {
    *error = "cannot read " + Narrow(src);
    return false;
  }
  FILE* out = _wfopen(dst.c_str(), L"wb");
  if (!out) {
    fclose(in);
    *error = "cannot write " + Narrow(dst);
    return false;
  }
  Sha256 sha;
  std::vector<unsigned char> buf(1 << 20);
  uint64_t total = 0;
  bool ok = true;
  for (;;) {
    const size_t n = fread(buf.data(), 1, buf.size(), in);
    if (n == 0)
      break;
    sha.Update(buf.data(), n);
    if (fwrite(buf.data(), 1, n, out) != n) {
      ok = false;
      *error = "write failed for " + Narrow(dst) + " (disk full?)";
      break;
    }
    total += n;
  }
  if (ferror(in)) {
    ok = false;
    *error = "read failed for " + Narrow(src);
  }
  fclose(in);
  if (ok && (fflush(out) != 0 || _commit(_fileno(out)) != 0)) {
    ok = false;
    *error = "flush failed for " + Narrow(dst);
  }
  fclose(out);
  if (!ok)
    return false;
  *size = total;
  *hash = sha.Final();
  return true;
}

void WriteTextAtomic(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.string() + ".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f)
    throw std::runtime_error("cannot write " + Narrow(tmp));
  const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0 &&
                  _commit(_fileno(f)) == 0;
  fclose(f);
  if (!ok)
    throw std::runtime_error("cannot write " + Narrow(tmp));
  fs::rename(tmp, path);
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---- manifest ------------------------------------------------------------------------------

struct Manifest {
  int version = 0;
  std::string format, title, created, kind, source, build;
  std::vector<std::string> profiles;
  uint64_t files_total = 0, bytes_total = 0;
  std::vector<FileEntry> files;
};

std::string Quote(const std::string& s) {
  std::string q = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"')
      q += '\\';
    if (c == '\n' || c == '\r')
      q += ' ';
    else
      q += c;
  }
  return q + "\"";
}

std::string FormatManifest(const Manifest& m) {
  std::ostringstream o;
  o << "# Army of Two: The Devil's Cartel - saved progress backup. Written by the Save Manager.\n";
  o << "format = " << Quote(m.format) << "\nversion = " << m.version << "\ntitle = " << Quote(m.title)
    << "\ncreated = " << Quote(m.created) << "\nkind = " << Quote(m.kind) << "\nsource = " << Quote(m.source)
    << "\nbuild = " << Quote(m.build) << "\nprofiles = [";
  for (size_t i = 0; i < m.profiles.size(); ++i)
    o << (i ? ", " : "") << Quote(m.profiles[i]);
  o << "]\nfiles_total = " << m.files.size() << "\nbytes_total = " << m.bytes_total << "\n";
  for (const FileEntry& f : m.files)
    o << "\n[[files]]\npath = " << Quote(f.rel) << "\nsize = " << f.size << "\nsha256 = " << Quote(f.sha256)
      << "\n";
  return o.str();
}

// Parses a double-quoted string at s[i] (escapes \\ and \"); advances i past the closing quote.
bool ParseQuoted(const std::string& s, size_t& i, std::string* out) {
  if (i >= s.size() || s[i] != '"')
    return false;
  ++i;
  out->clear();
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"')
      return true;
    if (c == '\\' && i < s.size())
      c = s[i++];
    *out += c;
  }
  return false;
}

bool ParseManifest(const std::string& text, Manifest* m, std::string* error) {
  std::istringstream in(text);
  std::string line;
  FileEntry* cur = nullptr;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#')
      continue;
    line = line.substr(b);
    if (line == "[[files]]") {
      m->files.emplace_back();
      cur = &m->files.back();
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      *error = "manifest line " + std::to_string(line_no) + " is not key = value";
      return false;
    }
    std::string key = line.substr(0, eq);
    key.erase(key.find_last_not_of(" \t") + 1);
    size_t v = line.find_first_not_of(" \t", eq + 1);
    if (v == std::string::npos) {
      *error = "manifest line " + std::to_string(line_no) + " has no value";
      return false;
    }
    std::string str;
    uint64_t num = 0;
    const bool is_str = line[v] == '"';
    if (is_str) {
      if (!ParseQuoted(line, v, &str)) {
        *error = "manifest line " + std::to_string(line_no) + " has an unterminated string";
        return false;
      }
    } else if (line[v] == '[') {
      ++v;
      m->profiles.clear();
      for (;;) {
        v = line.find_first_not_of(" \t,", v);
        if (v == std::string::npos || line[v] == ']')
          break;
        std::string item;
        if (!ParseQuoted(line, v, &item)) {
          *error = "manifest profiles list is malformed";
          return false;
        }
        m->profiles.push_back(item);
      }
      continue;
    } else {
      char* end = nullptr;
      num = std::strtoull(line.c_str() + v, &end, 10);
      if (end == line.c_str() + v) {
        *error = "manifest line " + std::to_string(line_no) + " has a bad number";
        return false;
      }
    }
    if (cur) {
      if (key == "path")
        cur->rel = str;
      else if (key == "size")
        cur->size = num;
      else if (key == "sha256")
        cur->sha256 = str;
    } else if (key == "format")
      m->format = str;
    else if (key == "version")
      m->version = int(num);
    else if (key == "title")
      m->title = str;
    else if (key == "created")
      m->created = str;
    else if (key == "kind")
      m->kind = str;
    else if (key == "source")
      m->source = str;
    else if (key == "build")
      m->build = str;
    else if (key == "files_total")
      m->files_total = num;
    else if (key == "bytes_total")
      m->bytes_total = num;
  }
  return true;
}

// Loads and structurally validates a backup's manifest (no file contents touched).
bool LoadManifest(const fs::path& dir, Manifest* m, std::string* error) {
  std::error_code ec;
  const fs::path file = dir / kManifestName;
  if (!fs::is_regular_file(file, ec)) {
    *error = "manifest.toml is missing";
    return false;
  }
  if (!ParseManifest(ReadText(file), m, error))
    return false;
  if (m->format != "ao2dc-save-backup") {
    *error = "not an Army of Two save backup";
    return false;
  }
  if (m->version != kManifestVersion) {
    *error = "manifest version " + std::to_string(m->version) + " is not supported";
    return false;
  }
  if (m->title != kTitle) {
    *error = "backup is for title " + m->title + ", not " + kTitle;
    return false;
  }
  if (m->files.empty() || m->files.size() != m->files_total) {
    *error = "manifest file count does not match";
    return false;
  }
  uint64_t bytes = 0;
  std::set<std::string> seen;
  std::set<std::string> profiles(m->profiles.begin(), m->profiles.end());
  for (const FileEntry& f : m->files) {
    if (!IsAllowedRelativePath(f.rel)) {
      *error = "manifest contains a path that is not saved progress: " + f.rel;
      return false;
    }
    if (!seen.insert(f.rel).second) {
      *error = "manifest lists " + f.rel + " twice";
      return false;
    }
    if (f.sha256.size() != 64) {
      *error = "manifest has no checksum for " + f.rel;
      return false;
    }
    const std::string first = SplitPath(f.rel)[0];
    if (first != "achievements" && !profiles.count(first)) {
      *error = "file " + f.rel + " belongs to a profile the manifest does not list";
      return false;
    }
    bytes += f.size;
  }
  if (bytes != m->bytes_total) {
    *error = "manifest size total does not match";
    return false;
  }
  return true;
}

// Re-hashes every file of a backup; returns "" when all match.
std::string VerifyFiles(const fs::path& dir, const Manifest& m) {
  for (const FileEntry& f : m.files) {
    const fs::path p = dir / kFilesDir / fs::path(f.rel);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
      return "missing file " + f.rel;
    uint64_t size = 0;
    const std::string hash = Sha256OfFile(p, &size);
    if (size != f.size)
      return "wrong size for " + f.rel;
    if (hash != f.sha256)
      return "checksum mismatch for " + f.rel;
  }
  return {};
}

void RemoveAll(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
}

bool UnderRoot(const fs::path& root, const fs::path& target) {
  auto lower = [](fs::path p) {
    std::string s = p.lexically_normal().generic_string();
    for (char& ch : s)
      ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
  };
  std::string r = lower(root);
  if (r.empty() || r.back() != '/')
    r += '/';
  const std::string t = lower(target);
  return t.size() > r.size() && t.compare(0, r.size(), r) == 0;
}

std::string UniqueId(const fs::path& backups_root) {
  for (int i = 0; i < 50; ++i) {
    const std::string id = StampForId() + "-" + RandomHex(6);
    std::error_code ec;
    if (!fs::exists(backups_root / id, ec) && !fs::exists(backups_root / (".tmp-" + id), ec))
      return id;
  }
  return StampForId() + "-" + RandomHex(12);
}

// ---- backup creation ----------------------------------------------------------------------

Result CreateFrom(const Context& ctx, const fs::path& source_root, const std::string& kind,
                  const std::string& note) {
  Result r;
  if (ctx.backups_root.empty()) {
    r.message = "The Saved Games folder could not be found, so there is nowhere to store backups.";
    return r;
  }
  NamedLock op("OP", ctx.user_root);
  if (!op.acquired()) {
    r.message = "Another save operation is running; try again in a moment.";
    return r;
  }
  const std::vector<std::string> scope = ScopeFiles(source_root);
  if (scope.empty()) {
    r.message = "No saved progress was found in " + Narrow(source_root) + ".";
    return r;
  }
  std::error_code ec;
  fs::create_directories(ctx.backups_root, ec);
  if (ec) {
    r.message = "Cannot create the backup folder " + Narrow(ctx.backups_root) + ": " + ec.message();
    return r;
  }
  const std::string id = UniqueId(ctx.backups_root);
  const fs::path tmp = ctx.backups_root / (".tmp-" + id);
  const fs::path final_dir = ctx.backups_root / id;
  try {
    Manifest m;
    m.format = "ao2dc-save-backup";
    m.version = kManifestVersion;
    m.title = kTitle;
    m.created = NowUtc();
    m.kind = kind;
    m.source = note;
    m.build = ctx.build;
    std::set<std::string> profiles;
    int index = 0;
    for (const std::string& rel : scope) {
      Fault(ctx, "copy:" + std::to_string(index++));
      const fs::path src = source_root / fs::path(rel);
      const fs::path dst = tmp / kFilesDir / fs::path(rel);
      FileEntry entry;
      entry.rel = rel;
      bool done = false;
      std::string error;
      for (int attempt = 0; attempt < 3 && !done; ++attempt) {
        const FileStat before = Stat(src);
        if (!CopyHashed(src, dst, &entry.size, &entry.sha256, &error))
          throw std::runtime_error(error);
        const FileStat after = Stat(src);
        if (before == after && after.size == entry.size)
          done = true;  // the source did not change while it was copied
      }
      if (!done)
        throw std::runtime_error(rel + " kept changing while it was copied");
      const std::string first = SplitPath(rel)[0];
      if (first != "achievements")
        profiles.insert(first);
      m.bytes_total += entry.size;
      m.files.push_back(entry);
    }
    m.profiles.assign(profiles.begin(), profiles.end());
    Fault(ctx, "verify");
    const std::string bad = VerifyFiles(tmp, m);
    if (!bad.empty())
      throw std::runtime_error("verification of the copy failed: " + bad);
    WriteTextAtomic(tmp / kManifestName, FormatManifest(m));
    Manifest check;
    std::string error;
    if (!LoadManifest(tmp, &check, &error))
      throw std::runtime_error("manifest check failed: " + error);
    Fault(ctx, "publish");
    fs::rename(tmp, final_dir);  // the backup exists, complete and verified, only from here on
    r.ok = true;
    r.id = id;
    r.message = "Backed up " + std::to_string(m.files.size()) + " files (" + std::to_string(m.bytes_total) +
                " bytes) as " + id + ".";
    Say(ctx, "Save backup: " + r.message);
  } catch (const std::exception& e) {
    RemoveAll(tmp);
    r.ok = false;
    r.message = std::string("Backup failed: ") + e.what() + ". Existing backups are untouched.";
    Say(ctx, "Save backup: " + r.message);
  }
  return r;
}

// ---- restore ------------------------------------------------------------------------------

struct JournalEntry {
  std::string rel;
  bool had_old = false;
};

std::string FormatJournal(const std::string& state, const std::string& backup,
                          const std::vector<JournalEntry>& entries) {
  std::ostringstream o;
  o << "state = " << Quote(state) << "\nbackup = " << Quote(backup) << "\n";
  for (const JournalEntry& e : entries)
    o << "\n[[files]]\npath = " << Quote(e.rel) << "\nhad_old = " << (e.had_old ? 1 : 0) << "\n";
  return o.str();
}

bool ParseJournal(const std::string& text, std::string* state, std::string* backup,
                  std::vector<JournalEntry>* entries) {
  std::istringstream in(text);
  std::string line;
  JournalEntry* cur = nullptr;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    if (line == "[[files]]") {
      entries->emplace_back();
      cur = &entries->back();
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      return false;
    std::string key = line.substr(0, eq);
    key.erase(key.find_last_not_of(" \t") + 1);
    size_t v = line.find_first_not_of(" \t", eq + 1);
    if (v == std::string::npos)
      return false;
    std::string str;
    if (line[v] == '"') {
      if (!ParseQuoted(line, v, &str))
        return false;
    }
    if (cur) {
      if (key == "path")
        cur->rel = str;
      else if (key == "had_old")
        cur->had_old = line[v] == '1';
    } else if (key == "state")
      *state = str;
    else if (key == "backup")
      *backup = str;
  }
  return !state->empty();
}

void Rollback(const fs::path& user_root, const fs::path& work, const std::vector<JournalEntry>& entries) {
  std::error_code ec;
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    const fs::path target = user_root / fs::path(it->rel);
    const fs::path pre = work / "pre" / fs::path(it->rel);
    if (fs::exists(pre, ec)) {
      fs::remove(target, ec);
      fs::create_directories(target.parent_path(), ec);
      fs::rename(pre, target, ec);
    } else if (!it->had_old) {
      fs::remove(target, ec);  // a restored file that did not exist before
    }
  }
  RemoveAll(work);
}

std::vector<std::string> RecoverInterrupted(const Context& ctx) {
  std::vector<std::string> lines;
  std::error_code ec;
  const fs::path base = ctx.user_root / kWorkDir;
  if (!fs::is_directory(base, ec))
    return lines;
  for (const auto& e : fs::directory_iterator(base, ec)) {
    const std::string name = e.path().filename().string();
    if (!e.is_directory(ec) || name.rfind("restore-", 0) != 0)
      continue;
    std::string state, backup;
    std::vector<JournalEntry> entries;
    const fs::path journal = e.path() / "journal.toml";
    if (!fs::is_regular_file(journal, ec) || !ParseJournal(ReadText(journal), &state, &backup, &entries)) {
      RemoveAll(e.path());
      lines.push_back("An interrupted restore had not changed any save file; its leftovers were removed.");
      continue;
    }
    if (state == "committing") {
      Rollback(ctx.user_root, e.path(), entries);
      lines.push_back("A restore of " + backup + " was interrupted; your previous saved progress was put back.");
    } else if (state == "committed") {
      RemoveAll(e.path());
      lines.push_back("A finished restore of " + backup + " was cleaned up.");
    } else {
      RemoveAll(e.path());
      lines.push_back("A restore of " + backup + " was interrupted before it changed anything; nothing to undo.");
    }
  }
  for (const std::string& l : lines)
    Say(ctx, "Save restore: " + l);
  return lines;
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------

std::string Sha256OfFile(const fs::path& file, uint64_t* size_out) {
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) {
    if (size_out)
      *size_out = 0;
    return {};
  }
  Sha256 sha;
  std::vector<unsigned char> buf(1 << 20);
  uint64_t total = 0;
  for (;;) {
    const size_t n = fread(buf.data(), 1, buf.size(), f);
    if (n == 0)
      break;
    sha.Update(buf.data(), n);
    total += n;
  }
  fclose(f);
  if (size_out)
    *size_out = total;
  return sha.Final();
}

fs::path DefaultBackupsRoot() {
  // Test hook only: lets automated runs keep their backups out of the real Saved Games folder.
  wchar_t override_path[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"AO2DC_TEST_BACKUPS_ROOT", override_path, MAX_PATH) > 0)
    return fs::path(override_path);
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &raw)) || !raw) {
    if (raw)
      CoTaskMemFree(raw);
    return {};
  }
  fs::path p(raw);
  CoTaskMemFree(raw);
  return p / "BrewSparrow" / "ArmyOfTwoTDC" / "Backups";
}

bool IsAllowedRelativePath(const std::string& rel) {
  if (rel.empty() || rel.size() > 400)
    return false;
  const std::vector<std::string> parts = SplitPath(rel);
  for (const std::string& p : parts) {
    if (p.empty() || p == "." || p == ".." || p.size() > 200)
      return false;
    if (p.back() == ' ' || p.back() == '.')
      return false;
    for (unsigned char c : p) {
      if (c < 0x20 || std::string("<>:\"\\|?*").find(char(c)) != std::string::npos)
        return false;
    }
    if (IsReservedName(p))
      return false;
  }
  if (parts[0] == "achievements")
    return parts.size() == 2 && parts[1] == std::string(kTitle) + ".toml";
  return parts.size() >= 3 && IsHex16(parts[0]) && !IsZeroXuid(parts[0]) && parts[1] == kTitle;
}

std::vector<std::string> ListProfiles(const fs::path& user_root) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(user_root, ec))
    return out;
  for (const auto& e : fs::directory_iterator(user_root, ec)) {
    const std::string name = e.path().filename().string();
    if (e.is_directory(ec) && IsHex16(name) && !IsZeroXuid(name) && fs::is_directory(e.path() / kTitle, ec))
      out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> ScopeFiles(const fs::path& user_root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const std::string& xuid : ListProfiles(user_root)) {
    const fs::path base = user_root / xuid / kTitle;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::none, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec)
        break;
      if (!it->is_regular_file(ec))
        continue;
      const std::string rel = fs::relative(it->path(), user_root, ec).generic_string();
      if (IsAllowedRelativePath(rel))
        out.push_back(rel);
    }
  }
  const fs::path ach = user_root / "achievements" / (std::string(kTitle) + ".toml");
  if (fs::is_regular_file(ach, ec))
    out.push_back(std::string("achievements/") + kTitle + ".toml");
  std::sort(out.begin(), out.end());
  return out;
}

Result CreateBackup(const Context& ctx, const std::string& kind, const std::string& source_note) {
  return CreateFrom(ctx, ctx.user_root, kind, source_note);
}

Result ImportFromInstallation(const Context& ctx, const fs::path& older) {
  Result r;
  std::error_code ec;
  fs::path candidate = older;
  if (fs::is_directory(older / "user", ec))
    candidate = older / "user";
  if (ScopeFiles(candidate).empty()) {
    r.message = "No saved progress was found in " + Narrow(older) +
                ". Choose the folder of an older release (the one that holds ao2dc_manifest.toml) or its user folder.";
    return r;
  }
  return CreateFrom(ctx, candidate, "import", "imported from " + Narrow(candidate));
}

std::vector<BackupInfo> ListBackups(const fs::path& backups_root) {
  std::vector<BackupInfo> out;
  std::error_code ec;
  if (!fs::is_directory(backups_root, ec))
    return out;
  for (const auto& e : fs::directory_iterator(backups_root, ec)) {
    const std::string name = e.path().filename().string();
    if (!e.is_directory(ec) || name.empty() || name[0] == '.')
      continue;
    BackupInfo b;
    b.id = name;
    b.dir = e.path();
    Manifest m;
    std::string error;
    if (!LoadManifest(e.path(), &m, &error)) {
      b.status = "Damaged: " + error;
      out.push_back(b);
      continue;
    }
    b.created = m.created;
    b.kind = m.kind;
    b.profiles = m.profiles;
    b.source = m.source;
    b.build = m.build;
    b.bytes = m.bytes_total;
    b.file_count = m.files.size();
    const std::string bad = VerifyFiles(e.path(), m);
    b.valid = bad.empty();
    b.status = b.valid ? "Complete" : "Damaged: " + bad;
    out.push_back(b);
  }
  std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
    return a.created != b.created ? a.created > b.created : a.id > b.id;
  });
  return out;
}

Result RestoreBackup(const Context& ctx, const std::string& backup_id) {
  Result r;
  r.id = backup_id;
  if (backup_id.empty() || backup_id[0] == '.' || backup_id.find_first_of("/\\:") != std::string::npos) {
    r.message = "That is not a backup name.";
    return r;
  }
  const fs::path dir = ctx.backups_root / backup_id;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    r.message = "The backup " + backup_id + " no longer exists.";
    return r;
  }
  // 1. Everything is checked before anything is replaced.
  Manifest m;
  std::string error;
  if (!LoadManifest(dir, &m, &error)) {
    r.message = "The backup cannot be restored: " + error + ". Nothing was changed.";
    return r;
  }
  if (const std::string bad = VerifyFiles(dir, m); !bad.empty()) {
    r.message = "The backup is damaged (" + bad + "). Nothing was changed.";
    return r;
  }
  for (const FileEntry& f : m.files) {
    if (!UnderRoot(ctx.user_root, ctx.user_root / fs::path(f.rel))) {
      r.message = "The backup points outside the save folder (" + f.rel + "). Nothing was changed.";
      return r;
    }
  }
  NamedLock op("OP", ctx.user_root);
  if (!op.acquired()) {
    r.message = "Another save operation is running; try again in a moment.";
    return r;
  }

  // 2. Recovery backup of everything that exists now, verified before we go on.
  const std::vector<std::string> existing = ScopeFiles(ctx.user_root);
  std::string recovery_note;
  if (!existing.empty()) {
    const Result rec = CreateFrom(ctx, ctx.user_root, "recovery", "before restoring " + backup_id);
    if (!rec.ok) {
      r.message = "Could not create the recovery backup of your current progress (" + rec.message +
                  ") so nothing was changed.";
      return r;
    }
    recovery_note = " Your previous progress was saved as recovery backup " + rec.id + ".";
  }

  // 3. Stage the replacement files next to the targets and verify them.
  const fs::path work = ctx.user_root / kWorkDir / ("restore-" + RandomHex(8));
  const fs::path staged = work / "staged";
  const fs::path pre = work / "pre";
  std::vector<JournalEntry> entries;
  try {
    fs::create_directories(staged, ec);
    fs::create_directories(pre, ec);
    WriteTextAtomic(work / "journal.toml", FormatJournal("staging", backup_id, {}));
    int index = 0;
    for (const FileEntry& f : m.files) {
      Fault(ctx, "stage:" + std::to_string(index++));
      uint64_t size = 0;
      std::string hash, err;
      if (!CopyHashed(dir / kFilesDir / fs::path(f.rel), staged / fs::path(f.rel), &size, &hash, &err))
        throw std::runtime_error(err);
      if (size != f.size || hash != f.sha256)
        throw std::runtime_error("staged copy of " + f.rel + " does not match its checksum");
    }
  } catch (const std::exception& e) {
    RemoveAll(work);
    r.message = std::string("Restore failed while preparing files: ") + e.what() + ". Nothing was changed.";
    Say(ctx, "Save restore: " + r.message);
    return r;
  }

  // 4. Commit: move the current files of the affected profiles aside, then move the staged ones in.
  std::set<std::string> profiles(m.profiles.begin(), m.profiles.end());
  std::set<std::string> new_rels;
  for (const FileEntry& f : m.files)
    new_rels.insert(f.rel);
  std::set<std::string> all = new_rels;
  std::set<std::string> old_rels;
  for (const std::string& rel : existing) {
    const std::string first = SplitPath(rel)[0];
    const bool affected = first == "achievements" ? new_rels.count(rel) != 0 : profiles.count(first) != 0;
    if (affected) {
      old_rels.insert(rel);
      all.insert(rel);
    }
  }
  for (const std::string& rel : all)
    entries.push_back({rel, old_rels.count(rel) != 0});
  try {
    WriteTextAtomic(work / "journal.toml", FormatJournal("committing", backup_id, entries));
    int n = 0;
    for (const std::string& rel : old_rels) {
      Fault(ctx, "commit:" + std::to_string(n++));
      const fs::path target = ctx.user_root / fs::path(rel);
      fs::create_directories((pre / fs::path(rel)).parent_path());
      fs::rename(target, pre / fs::path(rel));
    }
    for (const std::string& rel : new_rels) {
      Fault(ctx, "commit:" + std::to_string(n++));
      const fs::path target = ctx.user_root / fs::path(rel);
      fs::create_directories(target.parent_path());
      fs::rename(staged / fs::path(rel), target);
    }
    // Folders that only held files of the old state (a save made after the backup) are removed:
    // an empty leftover folder would make the game refuse to create a save of that name again.
    for (const std::string& rel : old_rels) {
      const std::vector<std::string> parts = SplitPath(rel);
      if (parts[0] == "achievements")
        continue;
      const fs::path stop = ctx.user_root / parts[0] / parts[1];
      fs::path dir = (ctx.user_root / fs::path(rel)).parent_path();
      while (UnderRoot(stop, dir)) {
        std::error_code remove_ec;
        if (!fs::remove(dir, remove_ec))
          break;  // not empty (or not removable): keep it
        dir = dir.parent_path();
      }
    }
    WriteTextAtomic(work / "journal.toml", FormatJournal("committed", backup_id, entries));
  } catch (const std::exception& e) {
    Rollback(ctx.user_root, work, entries);
    r.message = std::string("Restore failed (") + e.what() + "); your previous saved progress was put back." +
                recovery_note;
    Say(ctx, "Save restore: " + r.message);
    return r;
  }
  RemoveAll(work);
  r.ok = true;
  r.message = "Restored " + std::to_string(m.files.size()) + " files from " + backup_id + "." + recovery_note;
  Say(ctx, "Save restore: " + r.message);
  return r;
}

// ---- queue and launch tasks ----------------------------------------------------------------

static fs::path PendingDir(const fs::path& user_root) {
  return user_root / kWorkDir / "pending";
}

static Result QueueItem(const Context& ctx, const std::string& action, const std::string& backup) {
  Result r;
  try {
    const std::string name = StampForId() + "-" + RandomHex(4) + "-" + action + ".toml";
    std::ostringstream o;
    o << "action = " << Quote(action) << "\nbackup = " << Quote(backup) << "\nrequested = " << Quote(NowUtc())
      << "\n";
    WriteTextAtomic(PendingDir(ctx.user_root) / name, o.str());
    r.ok = true;
    r.id = name;
  } catch (const std::exception& e) {
    r.message = std::string("Could not queue the request: ") + e.what();
  }
  return r;
}

Result QueueBackup(const Context& ctx) {
  Result r = QueueItem(ctx, "backup", "");
  if (r.ok)
    r.message = "Queued: the backup runs the next time the game starts, before it opens any save file.";
  return r;
}

Result QueueRestore(const Context& ctx, const std::string& backup_id) {
  Result r = QueueItem(ctx, "restore", backup_id);
  if (r.ok)
    r.message = "Queued: the restore runs the next time the game starts, before it opens any save file. "
                "Close the game now and start it again.";
  return r;
}

std::vector<Pending> ListPending(const fs::path& user_root) {
  std::vector<Pending> out;
  std::error_code ec;
  const fs::path dir = PendingDir(user_root);
  if (!fs::is_directory(dir, ec))
    return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".toml")
      continue;
    Pending p;
    p.file = e.path();
    std::istringstream in(ReadText(e.path()));
    std::string line;
    while (std::getline(in, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      std::string key = line.substr(0, eq);
      key.erase(key.find_last_not_of(" \t") + 1);
      size_t v = line.find('"', eq);
      std::string val;
      if (v == std::string::npos || !ParseQuoted(line, v, &val))
        continue;
      if (key == "action")
        p.action = val;
      else if (key == "backup")
        p.backup = val;
      else if (key == "requested")
        p.requested = val;
    }
    if (p.action == "backup" || p.action == "restore")
      out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const Pending& a, const Pending& b) { return a.file < b.file; });
  return out;
}

bool CancelPending(const Pending& pending) {
  std::error_code ec;
  return fs::remove(pending.file, ec);
}

std::vector<std::string> LastLaunchTaskLines(const fs::path& user_root) {
  std::vector<std::string> lines;
  std::istringstream in(ReadText(user_root / kWorkDir / "last-launch-tasks.txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty())
      lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> RunLaunchTasks(const Context& ctx) {
  std::vector<std::string> lines = RecoverInterrupted(ctx);
  const std::vector<Pending> pending = ListPending(ctx.user_root);
  for (const Pending& p : pending) {
    Result r;
    if (p.action == "backup")
      r = CreateFrom(ctx, ctx.user_root, "queued", "queued from the Save Manager (" + p.requested + ")");
    else
      r = RestoreBackup(ctx, p.backup);
    lines.push_back((p.action == "backup" ? "Queued backup: " : "Queued restore: ") + r.message);
    std::error_code ec;
    if (r.ok) {
      fs::remove(p.file, ec);
    } else {
      fs::create_directories(PendingDir(ctx.user_root) / "failed", ec);
      fs::rename(p.file, PendingDir(ctx.user_root) / "failed" / p.file.filename(), ec);
    }
  }
  try {
    std::string text;
    for (const std::string& l : lines)
      text += l + "\n";
    if (!lines.empty() || fs::exists(ctx.user_root / kWorkDir / "last-launch-tasks.txt"))
      WriteTextAtomic(ctx.user_root / kWorkDir / "last-launch-tasks.txt", text);
  } catch (...) {
  }
  return lines;
}

// ---- lock and shell helpers ----------------------------------------------------------------

NamedLock::NamedLock(const char* kind, const fs::path& user_root) {
  std::error_code ec;
  fs::path canonical = fs::weakly_canonical(user_root, ec);
  std::string key = (ec ? user_root : canonical).generic_string();
  for (char& c : key)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  const size_t h = std::hash<std::string>{}(key);
  wchar_t name[96];
  swprintf(name, 96, L"Local\\AO2DC_SAVE_%hs_%016llX", kind, static_cast<unsigned long long>(h));
  HANDLE handle = CreateMutexW(nullptr, FALSE, name);
  handle_ = handle;
  if (handle) {
    const DWORD w = WaitForSingleObject(handle, 0);
    acquired_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
  }
}

NamedLock::~NamedLock() {
  if (handle_) {
    if (acquired_)
      ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
  }
}

fs::path PickFolder(const wchar_t* title) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    if (SUCCEEDED(dialog->Show(GetForegroundWindow()))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
          result = name;
          CoTaskMemFree(name);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(init))
    CoUninitialize();
  return result;
}

void OpenFolder(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace ao2dc::saves
