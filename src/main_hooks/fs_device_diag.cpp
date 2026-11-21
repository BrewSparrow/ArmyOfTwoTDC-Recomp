// fs_device_diag.cpp - guard for a null filesystem device in the game's file request setup.
//
// Evidence (Chapter 18 "Room Service", 2 runs in 11): a job on a worker thread crashes with a read of
// guest 0x0000000A in sub_838C1B90 (an rw::core::filesystem request constructor). It stores the result
// of sub_838C14A0 (the path / device resolve) at +64 and then reads byte +10 of it with no null check;
// the result was null. The path it was handed was not a clean path: it started with binary bytes and a
// sound bank record ("...GUID,1,361156,AO4_Assets/Sound/Music/08_Palace/Bumper/..."), i.e. a pointer
// into a buffer whose contents had been reused, which is what a lifetime race between jobs looks like.
//
// The code already skips queueing the request when that device flag is non-zero (it branches to the
// function's epilogue at 0x838C1C8C). The hook at 0x838C1C74 (the `lbz r9,10(r3)`) takes the same
// branch when the device is null, so the request is dropped instead of crashing. The first few are
// logged with the path. ao2dc_test_null_fs_device forces a null device for music requests to exercise
// that path on demand.
//
// Declared by codegen from the [[midasm_hook]] entries in config/ao4_main_functions_tu1.toml.

#include <algorithm>
#include <atomic>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/thread.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_test_null_fs_device, false, "Game",
                    "Development only: make the filesystem device lookup fail for music requests, to test "
                    "the null-device guard");

REXCVAR_DEFINE_BOOL(ao2dc_diag_fs_requests, false, "Game",
                    "Development only: keep a ring of the last filesystem requests (caller, thread, path pointer "
                    "and its first bytes) and report it when a request carries a corrupted path");

REXCVAR_DEFINE_BOOL(ao2dc_diag_fs_watch, false, "Game",
                    "Development only: hardware write watch (all threads) on the first 8 bytes of the path "
                    "field of the first stream job slot seen, logging each writer's guest stack");

REXCVAR_DEFINE_UINT32(ao2dc_diag_fs_watch_addr, 0x4209DBB8, "Game",
                      "Development only: guest path-field address watched by ao2dc_diag_fs_watch");

namespace {
struct WatchHit {
  uint32_t tid;
  uint64_t value;
  uint64_t stack[12];
};
constexpr int kMaxWatch = 1500;
WatchHit g_watch[kMaxWatch];
std::atomic<int> g_watch_n{0};
std::atomic<uint8_t*> g_watch_addr{nullptr};
uint8_t* g_membase = nullptr;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
  uint8_t* a = g_watch_addr.load();
  if (a && ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 1) &&
      reinterpret_cast<uint8_t*>(ep->ContextRecord->Dr0) == a) {
    const int n = g_watch_n.fetch_add(1);
    if (n < kMaxWatch) {
      WatchHit& h = g_watch[n];
      h.tid = GetCurrentThreadId();
      uint64_t v;
      memcpy(&v, a, 8);
      h.value = __builtin_bswap64(v);
      CONTEXT c = *ep->ContextRecord;
      int i = 0;
      for (; i < 12; ++i) {
        h.stack[i] = c.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!fe) {
          if (!c.Rsp) break;
          c.Rip = *reinterpret_cast<uint64_t*>(c.Rsp);
          c.Rsp += 8;
        } else {
          PVOID hd = nullptr;
          DWORD64 ef = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &hd, &ef, nullptr);
        }
        if (!c.Rip) break;
      }
      for (; i < 12; ++i) h.stack[i] = 0;
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::string FnName(uint64_t rip) {
  static std::vector<PPCFuncMapping> maps = [] {
    std::vector<PPCFuncMapping> m;
    for (PPCFuncMapping* p = PPCFuncMappings; p->host; ++p) m.push_back(*p);
    std::sort(m.begin(), m.end(), [](const PPCFuncMapping& a, const PPCFuncMapping& b) {
      return reinterpret_cast<uintptr_t>(a.host) < reinterpret_cast<uintptr_t>(b.host);
    });
    return m;
  }();
  auto it = std::upper_bound(maps.begin(), maps.end(), rip, [](uint64_t r, const PPCFuncMapping& m) {
    return r < reinterpret_cast<uintptr_t>(m.host);
  });
  if (it == maps.begin()) return fmt::format("host:{:X}", rip);
  --it;
  const uint64_t d = rip - reinterpret_cast<uintptr_t>(it->host);
  if (d > 0x40000) return fmt::format("host:{:X}", rip);
  return fmt::format("sub_{:08X}+0x{:X}", uint32_t(it->guest), d);
}

void ArmAllThreads(uint8_t* a) {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!th) continue;
    if (SuspendThread(th) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(th, &c) && c.Dr0 != reinterpret_cast<DWORD64>(a)) {
        c.Dr0 = reinterpret_cast<DWORD64>(a);
        // L0, RW0 = 01 (write), LEN0 = 10 (8 bytes)
        c.Dr7 = (c.Dr7 & ~0x000F0003ull) | 0x1 | (0x1ull << 16) | (0x2ull << 18);
        SetThreadContext(th, &c);
      }
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
}

void WatchThread(uint32_t guest) {
  uint8_t* a = g_membase + guest;
  AddVectoredExceptionHandler(1, WatchHandler);
  g_watch_addr.store(a);
  REXLOG_WARN("FS watch: hardware write watch armed on guest 0x{:08X}", guest);
  int reported = 0;
  for (int i = 0; i < 1200; ++i) {  // about 10 minutes
    ArmAllThreads(a);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int n = std::min(g_watch_n.load(), kMaxWatch);
    while (reported < n) {
      const WatchHit& h = g_watch[reported++];
      std::string st;
      for (int k = 0; k < 12 && h.stack[k]; ++k) st += (k ? " <- " : "") + FnName(h.stack[k]);
      REXLOG_WARN("FS watch write #{} tid={} now={:016X}: {}", reported, h.tid, h.value, st);
    }
  }
}

struct FsReq {
  uint32_t tid, lr, req, path;
  int64_t us;
  uint8_t head[24];
  bool bad;
};
constexpr uint32_t kRing = 256;
FsReq g_ring[kRing];
std::atomic<uint32_t> g_ring_n{0};

std::string Hex(uint32_t guest, int n) {
  std::string s;
  if (guest < 0x10000u || guest >= 0xA0000000u)
    return s;
  auto* mem = rex::Runtime::instance()->memory();
  char b[4];
  for (int i = 0; i < n; ++i) {
    snprintf(b, sizeof b, "%02X", unsigned(*mem->TranslateVirtual(guest + i)));
    s += b;
    if (i % 4 == 3) s += ' ';
  }
  return s;
}

struct OwnerRec {
  uint32_t tid, job, owner, w0, w1, state80, w144;
  int64_t us;
};
OwnerRec g_owner[kRing];
std::atomic<uint32_t> g_owner_n{0};

std::string HexBytes(const uint8_t* p, int n) {
  std::string s;
  char b[4];
  for (int i = 0; i < n; ++i) {
    snprintf(b, sizeof b, "%02X", unsigned(p[i]));
    s += b;
    if (i % 4 == 3) s += ' ';
  }
  return s;
}

std::string GuestString(uint32_t guest) {
  std::string s;
  if (guest < 0x10000u || guest >= 0xA0000000u)
    return s;
  auto* mem = rex::Runtime::instance()->memory();
  for (int i = 0; i < 200; ++i) {
    char c = char(*mem->TranslateVirtual(guest + i));
    if (!c)
      break;
    s += (c >= 32 && c < 127) ? c : '.';
  }
  return s;
}
}  // namespace

void Ao2dcDumpFsRing(uint32_t path);
void Ao2dcDumpFsOwners(uint32_t owner_path);

// sub_838C1B90 (TU1) at 0x838C1BE8, right after the device resolve: r3 = result, r30 = the path.
void Ao2dcTestNullFsDevice(PPCRegister& r3, PPCRegister& r30) {
  if (!REXCVAR_GET(ao2dc_test_null_fs_device) || r3.u32 == 0)
    return;
  if (GuestString(r30.u32).find("Music/") != std::string::npos)
    r3.u64 = 0;
}

// sub_838C1B90 (TU1) at 0x838C1C74, before `lbz r9,10(r3)`: true skips to the epilogue.
bool Ao2dcSkipNullFsDevice(PPCRegister& r3, PPCRegister& r30) {
  if (r3.u32 != 0)
    return false;
  static std::atomic<uint32_t> n{0};
  const uint32_t k = n.fetch_add(1) + 1;
  if (k <= 20)
    REXLOG_WARN("Filesystem request with no device dropped (occurrence {}): path '{}'", k, GuestString(r30.u32));
  if (k <= 20)
    Ao2dcDumpFsRing(r30.u32);
  return true;
}

// sub_838C1B90 (TU1) at 0x838C1B9C (`mr r31,r3`, after `mflr r12`): r12 = the caller's return address,
// r3 = the request, r4 = the path. Bounded: only the first 24 bytes of the path are copied.
void Ao2dcLogFsRequest(PPCRegister& r3, PPCRegister& r4, PPCRegister& r12) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  FsReq& e = g_ring[g_ring_n.fetch_add(1) % kRing];
  e.tid = rex::thread::current_thread_id();
  e.lr = r12.u32;
  e.req = r3.u32;
  e.path = r4.u32;
  e.us = std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
  e.bad = false;
  if (REXCVAR_GET(ao2dc_diag_fs_watch) && !g_watch_addr.load()) {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) {
      g_membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
      std::thread(WatchThread, REXCVAR_GET(ao2dc_diag_fs_watch_addr)).detach();
    }
  }
  memset(e.head, 0, sizeof e.head);
  if (r4.u32 >= 0x10000u && r4.u32 < 0xA0000000u) {
    auto* mem = rex::Runtime::instance()->memory();
    for (int i = 0; i < 24; ++i) e.head[i] = *mem->TranslateVirtual(r4.u32 + i);
    // A real path starts with printable text; the failing one started with 8 binary bytes.
    for (int i = 0; i < 8 && e.head[i]; ++i) if (e.head[i] < 32 || e.head[i] >= 127) e.bad = true;
  }
  if (e.bad) {
    static std::atomic<uint32_t> n{0};
    if (n.fetch_add(1) < 10)
      REXLOG_WARN("Corrupted path AT ENTRY of a filesystem request: caller lr=0x{:08X} tid={} request=0x{:08X} path=0x{:08X} head={}",
                  e.lr, e.tid, e.req, e.path, HexBytes(e.head, 24));
  }
}

// Called when the guard fires: dump the ring entries for the same path pointer and the last few requests.
void Ao2dcDumpFsRing(uint32_t path) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  const uint32_t n = g_ring_n.load();
  const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  REXLOG_WARN("Guard fired for path 0x{:08X}; its bytes now: {}", path, Hex(path, 48));
  Ao2dcDumpFsOwners(path);
  for (uint32_t i = 0; i < kRing && i < n; ++i) {
    const FsReq& e = g_ring[(n - 1 - i) % kRing];
    if (e.path == path || i < 6)
      REXLOG_WARN("  ring[-{}] {:.1f} ms ago tid={} lr=0x{:08X} req=0x{:08X} path=0x{:08X} bad={} head={}", i,
                  double(now - e.us) / 1000.0, e.tid, e.lr, e.req, e.path, e.bad, HexBytes(e.head, 24));
  }
}

// sub_838C1F48 (TU1) entry, `r3` = the queued job: [r3+24] is the owner object whose +144 holds the path and
// +416 the embedded request. Records the owner's first words at the moment the job runs.
void Ao2dcLogFsJob(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  OwnerRec& o = g_owner[g_owner_n.fetch_add(1) % kRing];
  auto* mem = rex::Runtime::instance()->memory();
  auto rd = [&](uint32_t a) {
    if (a < 0x10000u || a >= 0xA0000000u) return 0u;
    uint32_t v; memcpy(&v, mem->TranslateVirtual(a), 4); return __builtin_bswap32(v);
  };
  o.tid = rex::thread::current_thread_id();
  o.job = r3.u32;
  o.owner = rd(r3.u32 + 24);
  o.w0 = rd(o.owner);
  o.w1 = rd(o.owner + 4);
  o.state80 = rd(o.owner + 80);
  o.w144 = rd(o.owner + 144);
  o.us = std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Ao2dcDumpFsOwners(uint32_t owner_path) {
  const uint32_t n = g_owner_n.load();
  const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  for (uint32_t i = 0; i < kRing && i < n; ++i) {
    const OwnerRec& o = g_owner[(n - 1 - i) % kRing];
    if (o.owner + 144 == owner_path || i < 3)
      REXLOG_WARN("  job[-{}] {:.1f} ms ago tid={} job=0x{:08X} owner=0x{:08X} [+0]=0x{:08X} [+4]=0x{:08X} [+80]=0x{:08X} [+144]=0x{:08X}",
                  i, double(now - o.us) / 1000.0, o.tid, o.job, o.owner, o.w0, o.w1, o.state80, o.w144);
  }
}

// sub_838C1FA0 (TU1) at 0x838C2108 (start of the path copy): r31 = the stream owner, r30 = its descriptor
// ([r31+124]); the path is copied from descriptor+20 into owner+144.
void Ao2dcLogFsCopy(PPCRegister& r30, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  static std::atomic<uint32_t> n{0};
  const uint32_t k = n.fetch_add(1);
  const std::string head = Hex(r30.u32 + 20, 8);
  bool bad = false;
  auto* mem = rex::Runtime::instance()->memory();
  if (r30.u32 >= 0x10000u && r30.u32 < 0xA0000000u)
    for (int i = 0; i < 8; ++i) {
      const uint8_t c = *mem->TranslateVirtual(r30.u32 + 20 + i);
      if (!c) break;
      if (c < 32 || c >= 127) bad = true;
    }
  if (k < 80 || bad)
    REXLOG_WARN("FS copy #{}: tid={} owner=0x{:08X} descriptor=0x{:08X} {} head[+20]={} str='{}' words[0..+44]={}",
                k, rex::thread::current_thread_id(), r31.u32, r30.u32, bad ? "CORRUPT" : "ok", head,
                GuestString(r30.u32 + 20).substr(0, 60), Hex(r30.u32, 48));
}

// sub_838C2290 (TU1) at 0x838C2298 (`mr r28,r4`): r4 = the path string the caller (AudioCore, sub_83879B08)
// hands over; it is strncpy'd into the new descriptor at +20.
void Ao2dcLogFsSource(PPCRegister& r4) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  static std::atomic<uint32_t> n{0};
  const uint32_t k = n.fetch_add(1);
  bool bad = false;
  if (r4.u32 >= 0x10000u && r4.u32 < 0xA0000000u) {
    auto* mem = rex::Runtime::instance()->memory();
    for (int i = 0; i < 8; ++i) {
      const uint8_t c = *mem->TranslateVirtual(r4.u32 + i);
      if (!c) break;
      if (c < 32 || c >= 127) bad = true;
    }
  }
  if (k < 80 || bad)
    REXLOG_WARN("FS source #{}: tid={} src=0x{:08X} {} bytes={} str='{}'", k, rex::thread::current_thread_id(),
                r4.u32, bad ? "CORRUPT" : "ok", Hex(r4.u32 - 16, 16) + "| " + Hex(r4.u32, 24),
                GuestString(r4.u32).substr(0, 70));
}

namespace {
// AudioCore stream-open message consumer: sub_83879B08 entry, r3 = the queued message; the path string is
// inline at msg+56, 16 header bytes before it. Bounded ring, no I/O until corruption is seen.
struct MsgRec {
  uint32_t seq, tid, msg, hdr[8];
  int64_t us;
};
constexpr uint32_t kMsgRing = 128;
MsgRec g_msg[kMsgRing];
std::atomic<uint32_t> g_msg_n{0};
}  // namespace

void Ao2dcUntrackAudioMsg(PPCRegister& r3);
void Ao2dcDumpQueueEvents(uint32_t msg);
void Ao2dcLogAudioMsg(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  auto* mem = rex::Runtime::instance()->memory();
  const uint32_t m = r3.u32;
  if (m < 0x10000u || m >= 0xA0000000u - 128)
    return;
  Ao2dcUntrackAudioMsg(r3);
  const uint32_t seq = g_msg_n.fetch_add(1);
  MsgRec& e = g_msg[seq % kMsgRing];
  e.seq = seq;
  e.tid = rex::thread::current_thread_id();
  e.msg = m;
  for (int i = 0; i < 8; ++i) {
    uint32_t v; memcpy(&v, mem->TranslateVirtual(m + 32 + i * 4), 4);
    e.hdr[i] = __builtin_bswap32(v);
  }
  e.us = std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
  // e.hdr[6] = first 4 bytes of the string; a real name is printable text, the corruption is a static pointer.
  const uint32_t s0 = e.hdr[6];
  bool bad = false;
  for (int i = 0; i < 8; ++i) {
    uint8_t c = uint8_t(e.hdr[6 + i / 4] >> (24 - 8 * (i % 4)));
    if (!c) break;
    if (c < 32 || c >= 127) bad = true;
  }
  (void)s0;
  if (seq < 40 || bad) {
    REXLOG_WARN("AudioMsg #{} tid={} msg=0x{:08X} {} hdr[+32..]={:08X} {:08X} {:08X} {:08X} | {:08X} {:08X} {:08X} {:08X}",
                seq, e.tid, m, bad ? "CORRUPT" : "ok", e.hdr[0], e.hdr[1], e.hdr[2], e.hdr[3], e.hdr[4], e.hdr[5],
                e.hdr[6], e.hdr[7]);
  }
  if (bad) {
    static std::atomic<uint32_t> d{0};
    if (d.fetch_add(1) < 3) {
      Ao2dcDumpQueueEvents(m);
      for (uint32_t i = 1; i <= 40 && i <= seq; ++i) {
        const MsgRec& p = g_msg[(seq - i) % kMsgRing];
        REXLOG_WARN("  prev[-{}] seq={} {:.1f} ms ago tid={} msg=0x{:08X} hdr={:08X} {:08X} {:08X} {:08X} | {:08X} {:08X}", i,
                    p.seq, double(e.us - p.us) / 1000.0, p.tid, p.msg, p.hdr[0], p.hdr[1], p.hdr[2], p.hdr[3], p.hdr[4], p.hdr[5]);
      }
    }
  }
}

namespace {
// Pending-message tracker: sub_8387A1E0 builds the stream-open message in the command buffer ([Q+56] + offset
// [Q+216], bump allocated) and publishes it. Each message is remembered with its first path bytes; a poll
// thread re-checks them every ~0.5 ms until the consumer (sub_83879B08) takes them, so the first change is
// timed to ~0.5 ms and reported with the surrounding events. No hardware watch, nothing per event but a ring write.
struct Pending {
  uint32_t msg = 0, q = 0, off = 0, base = 0, size = 0, tid = 0;
  uint8_t snap[16];
  int64_t us = 0;
};
struct Ev {
  int64_t us;
  uint32_t kind, tid, msg, q, off, base, size;  // kind 1 = enqueue, 2 = consume
};
constexpr int kPend = 32, kEv = 1024;
Pending g_pend[kPend];
std::mutex g_pend_mu;
Ev g_ev[kEv];
std::atomic<uint32_t> g_ev_n{0};
std::atomic<bool> g_poll_started{false}, g_frozen{false};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t Rd32(uint32_t a) {
  if (a < 0x10000u || a >= 0xA0000000u - 4) return 0;
  uint32_t v; memcpy(&v, rex::Runtime::instance()->memory()->TranslateVirtual(a), 4);
  return __builtin_bswap32(v);
}
void AddEv(uint32_t kind, uint32_t msg, uint32_t q, uint32_t off, uint32_t base, uint32_t size) {
  Ev& e = g_ev[g_ev_n.fetch_add(1) % kEv];
  e = {NowUs(), kind, uint32_t(rex::thread::current_thread_id()), msg, q, off, base, size};
}
void PollLoop() {
  auto* mem = rex::Runtime::instance()->memory();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    if (g_frozen.load()) return;
    std::lock_guard<std::mutex> lk(g_pend_mu);
    for (auto& p : g_pend) {
      if (!p.msg) continue;
      if (memcmp(mem->TranslateVirtual(p.msg + 56), p.snap, 8) == 0) continue;
      if (g_frozen.exchange(true)) return;
      const int64_t now = NowUs();
      uint8_t cur[16]; memcpy(cur, mem->TranslateVirtual(p.msg + 56), 16);
      REXLOG_WARN("MSG CHANGED while queued: msg=0x{:08X} Q=0x{:08X} buf=0x{:08X} off_at_enqueue=0x{:X} size={} enq_tid={} queued {:.2f} ms; was {} now {}",
                  p.msg, p.q, p.base, p.off, p.size, p.tid, double(now - p.us) / 1000.0, HexBytes(p.snap, 16), HexBytes(cur, 16));
      REXLOG_WARN("  Q now: [Q+56]=0x{:08X} [Q+216]=0x{:X}; msg-buf=0x{:X}", Rd32(p.q + 56), Rd32(p.q + 216), p.msg - Rd32(p.q + 56));
      const uint32_t n = g_ev_n.load();
      for (uint32_t i = 0; i < kEv && i < n && i < 120; ++i) {
        const Ev& e = g_ev[(n - 1 - i) % kEv];
        REXLOG_WARN("  ev[-{}] {:.2f} ms ago {} tid={} msg=0x{:08X} Q=0x{:08X} buf=0x{:08X} off=0x{:X} size={}", i, double(now - e.us) / 1000.0,
                    e.kind == 1 ? "ENQ" : e.kind == 2 ? "CONS" : e.kind == 3 ? "ALLOC" : e.kind == 4 ? "ALLOC8" : e.kind == 5 ? "COPY" : e.kind == 6 ? "CLEAR_A" : "CLEAR_B", e.tid, e.msg, e.q, e.base, e.off, e.size);
      }
      for (auto& o : g_pend) if (o.msg) REXLOG_WARN("  pending msg=0x{:08X} queued {:.2f} ms Q=0x{:08X}", o.msg, double(now - o.us) / 1000.0, o.q);
      return;
    }
  }
}
}  // namespace

// sub_8387A1E0 at 0x8387A3E0 (string just copied into the new message): r1 = guest stack; [r1+80] = Q, [r1+96] = msg.
void Ao2dcTrackAudioMsg(PPCRegister& r1) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  const uint32_t sp = r1.u32;
  Pending p;
  p.q = Rd32(sp + 80);
  p.msg = Rd32(sp + 96);
  p.size = Rd32(sp + 100) + 56;
  if (p.msg < 0x10000u || p.msg >= 0xA0000000u - 128 || g_frozen.load() || p.size <= 60)
    return;
  p.base = Rd32(p.q + 56);
  p.off = Rd32(p.q + 216);
  p.tid = rex::thread::current_thread_id();
  p.us = NowUs();
  memcpy(p.snap, rex::Runtime::instance()->memory()->TranslateVirtual(p.msg + 56), 16);
  {
    // Is the copied path already binary at publish time? Then the source (caller's string) was invalid before this
    // function copied it: [r1+104] = source string pointer, [r1+340] = the source object (string at +24).
    bool bad = false;
    for (int i = 0; i < 8 && p.snap[i]; ++i) if (p.snap[i] < 32 || p.snap[i] >= 127) bad = true;
    static std::atomic<uint32_t> nb{0};
    if (bad && nb.fetch_add(1) < 6) {
      const uint32_t srcp = Rd32(sp + 104), srco = Rd32(sp + 340);
      std::string chain;
      uint32_t fp = sp;
      for (int i = 0; i < 10; ++i) {
        const uint32_t up = Rd32(fp);
        if (up <= fp || up - fp > 0x10000) break;
        char b[16]; snprintf(b, sizeof b, "%08X ", Rd32(up - 8));
        chain += b;
        fp = up;
      }
      REXLOG_WARN("  guest return chain: {}", chain);
      REXLOG_WARN("AudioMsg BAD AT ENQUEUE: msg=0x{:08X} tid={} source_ptr=0x{:08X} source_obj=0x{:08X} msg_path={} src_path={} src_obj={}",
                  p.msg, p.tid, srcp, srco, Hex(p.msg + 56, 16), Hex(srcp, 16), Hex(srco, 48));
    }
  }
  AddEv(1, p.msg, p.q, p.off, p.base, p.size);
  {
    std::lock_guard<std::mutex> lk(g_pend_mu);
    Pending* slot = &g_pend[0];
    for (auto& x : g_pend) { if (x.msg == p.msg) { slot = &x; break; } if (!x.msg) slot = &x; }
    *slot = p;
  }
  if (!g_poll_started.exchange(true))
    std::thread(PollLoop).detach();
}

void Ao2dcUntrackAudioMsg(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  AddEv(2, r3.u32, 0, 0, 0, 0);
  std::lock_guard<std::mutex> lk(g_pend_mu);
  for (auto& x : g_pend) if (x.msg == r3.u32) x.msg = 0;
}

// sub_83868650 entry (command-buffer bump allocate): r3 = queue, r4 = size. Records who allocates and the
// offset before the bump; two threads getting the same offset would show here.
void Ao2dcLogQueueAlloc(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests) || g_frozen.load())
    return;
  AddEv(3, 0, r3.u32, Rd32(r3.u32 + 216), Rd32(r3.u32 + 56), r4.u32);
}
// sub_83868670 entry (8-byte command: handler, arg): r3 = queue.
void Ao2dcLogQueueAlloc8(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests) || g_frozen.load())
    return;
  AddEv(4, 0, r3.u32, Rd32(r3.u32 + 216), Rd32(r3.u32 + 56), 8);
}

void Ao2dcDumpQueueEvents(uint32_t msg) {
  const int64_t now = NowUs();
  const uint32_t n = g_ev_n.load();
  REXLOG_WARN("Queue events before corrupt message 0x{:08X} (consumed by tid {}):", msg, rex::thread::current_thread_id());
  for (uint32_t i = 0; i < kEv && i < n && i < 100; ++i) {
    const Ev& e = g_ev[(n - 1 - i) % kEv];
    REXLOG_WARN("  ev[-{}] {:.3f} ms ago {} tid={} msg=0x{:08X} Q=0x{:08X} buf=0x{:08X} off=0x{:X} size={}", i, double(now - e.us) / 1000.0,
                e.kind == 1 ? "ENQ" : e.kind == 2 ? "CONS" : e.kind == 3 ? "ALLOC" : e.kind == 4 ? "ALLOC8" : e.kind == 5 ? "COPY" : e.kind == 6 ? "CLEAR_A" : "CLEAR_B", e.tid, e.msg, e.q, e.base, e.off, e.size);
  }
}

// sub_82E385F8 (sound-instance update) at 0x82E38860: r3 = a property-list node whose 64-bit value at +24 has just
// been shallow-copied into the command parameter block (tag 0x7FF7FFF3 = pointer value). Logs the first bytes of
// the pointee when they are binary, i.e. a string pointer that is already invalid at copy time.
static void DumpStrPropSets(uint32_t ptr, int64_t now);
bool Ao2dcEmitterBufferHeld(uint32_t p);
void Ao2dcLogParamCopy(PPCRegister& r3, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  const uint32_t tag = Rd32(r3.u32 + 24), ptr = Rd32(r3.u32 + 28);
  if (tag != 0x7FF7FFF3u || ptr < 0x40000000u || ptr >= 0x50000000u)
    return;
  {
    // Copies whose text lives in an emitter buffer the guard is holding: without the guard that block would
    // already be freed here (the destructor ran before this update).
    static std::atomic<uint32_t> saved{0};
    if (Ao2dcEmitterBufferHeld(ptr)) {
      const uint32_t k = saved.fetch_add(1) + 1;
      if (k <= 10)
        REXLOG_INFO("Param copy reads a guard-held emitter buffer 0x{:08X}: {} (occurrence {})", ptr, Hex(ptr, 24), k);
    }
  }
  auto* mem = rex::Runtime::instance()->memory();
  const uint8_t* s = mem->TranslateVirtual(ptr);
  bool bad = false;
  for (int i = 0; i < 8 && s[i]; ++i) if (s[i] < 32 || s[i] >= 127) bad = true;
  static std::atomic<uint32_t> n{0}, total{0};
  total.fetch_add(1);
  AddEv(5, r31.u32, ptr, 0, 0, bad ? 1 : 0);
  if (bad && n.fetch_add(1) < 20) {
    const int64_t now = NowUs();
    const uint32_t en = g_ev_n.load();
    for (uint32_t i = 0; i < kEv && i < en; ++i) {
      const Ev& e = g_ev[(en - 1 - i) % kEv];
      if (e.kind >= 5 && (e.msg == r31.u32 || e.q == ptr))
        REXLOG_WARN("  instance/ptr event {:.3f} ms ago kind={} tid={} instance=0x{:08X} ptr=0x{:08X}", double(now - e.us) / 1000.0,
                    e.kind == 5 ? "COPY" : e.kind == 6 ? "CLEAR_A" : "CLEAR_B", e.tid, e.msg, e.q);
    }
    DumpStrPropSets(ptr, now);
    REXLOG_WARN("Param copy with binary string head: tid={} node=0x{:08X} instance=0x{:08X} ptr=0x{:08X} head={} (copies so far {})",
                rex::thread::current_thread_id(), r3.u32, r31.u32, ptr, Hex(ptr, 16), total.load());
  }
}

// sub_82E37510 (string property setter), hooked after its `mflr r12`: r3 = property list, r5 = property id,
// r6 = the string pointer stored raw (tag 0x7FF7FFF3, no copy, no refcount), r12 = caller's return address.
// Own small ring, so command-queue events do not push the setter out before the later update reads it.
struct SetEv { int64_t us; uint32_t tid, ptr, list, id, caller, h0, h1; };
constexpr uint32_t kSet = 512;
SetEv g_set[kSet];
std::atomic<uint32_t> g_set_n{0};
void Ao2dcLogStrPropSet(PPCRegister& r3, PPCRegister& r5, PPCRegister& r6, PPCRegister& r12) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  const uint32_t ptr = r6.u32;
  SetEv& e = g_set[g_set_n.fetch_add(1) % kSet];
  e = {NowUs(), uint32_t(rex::thread::current_thread_id()), ptr, r3.u32, r5.u32 & 0xFF, r12.u32,
       ptr >= 0x40000000u && ptr < 0x50000000u ? Rd32(ptr) : 0, ptr >= 0x40000000u && ptr < 0x50000000u ? Rd32(ptr + 4) : 0};
}
// sub_82E59620 at 0x82E59744 (before the text is rebuilt): r31 = the emitter, [r31+36] = its 512-byte scratch buffer
// that every request object keeps a pointer to ([request+32]). Records the buffer and the first 8 bytes it still holds
// (valid old text, or free-list data if it was freed while the emitter still points at it) and arms the free probe.
struct BufEv { int64_t us; uint32_t tid, emitter, buf, h0, h1; };
struct FreeEv { int64_t us; uint32_t tid, ptr, caller, chain[4]; };
constexpr uint32_t kBuf = 256, kFree = 256, kTrk = 4096;
BufEv g_buf[kBuf];
FreeEv g_free[kFree];
std::atomic<uint32_t> g_buf_n{0}, g_free_n{0}, g_trk[kTrk];
std::atomic<bool> g_trk_on{false};
void Ao2dcLogEmitterBuf(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests))
    return;
  const uint32_t buf = Rd32(r31.u32 + 36);
  if (buf < 0x40000000u || buf >= 0x50000000u)
    return;
  g_buf[g_buf_n.fetch_add(1) % kBuf] = {NowUs(), uint32_t(rex::thread::current_thread_id()), r31.u32, buf, Rd32(buf), Rd32(buf + 4)};
  g_trk[(buf >> 4) % kTrk].store(buf, std::memory_order_relaxed);
  g_trk_on.store(true, std::memory_order_relaxed);
}
// Heap free sub_82D6EBD0 (r3 = block), hooked at 0x82D6EBD4 right after `mflr r12`: r12 = caller, r1 = caller's sp.
// Hot path: one relaxed load until the first scratch buffer is seen, then one table compare per free.
void Ao2dcTrackedFree(PPCRegister& r3, PPCRegister& r12, PPCRegister& r1) {
  if (!g_trk_on.load(std::memory_order_relaxed))
    return;
  const uint32_t p = r3.u32;
  std::atomic<uint32_t>& slot = g_trk[(p >> 4) % kTrk];
  if (slot.load(std::memory_order_relaxed) != p)
    return;
  slot.store(0, std::memory_order_relaxed);
  FreeEv e{NowUs(), uint32_t(rex::thread::current_thread_id()), p, r12.u32, {0, 0, 0, 0}};
  uint32_t fp = r1.u32;
  for (int i = 0; i < 4; ++i) {
    const uint32_t up = Rd32(fp);
    if (up <= fp || up - fp > 0x10000) break;
    e.chain[i] = Rd32(up - 8);
    fp = up;
  }
  g_free[g_free_n.fetch_add(1) % kFree] = e;
}
// Called from the corrupt-copy report: every recorded setter call that stored this pointer.
static void DumpStrPropSets(uint32_t ptr, int64_t now) {
  {
    const uint32_t bn = g_buf_n.load(), fn = g_free_n.load();
    uint32_t nb = 0, nf = 0;
    for (uint32_t i = 0; i < kBuf && i < bn; ++i) {
      const BufEv& e = g_buf[(bn - 1 - i) % kBuf];
      if (e.buf != ptr) continue;
      ++nb;
      REXLOG_WARN("  emitter-buf {:.3f} ms ago tid={} emitter=0x{:08X} buf=0x{:08X} head-before-rebuild={:08X} {:08X}",
                  double(now - e.us) / 1000.0, e.tid, e.emitter, e.buf, e.h0, e.h1);
    }
    for (uint32_t i = 0; i < kFree && i < fn; ++i) {
      const FreeEv& e = g_free[(fn - 1 - i) % kFree];
      if (e.ptr != ptr) continue;
      ++nf;
      REXLOG_WARN("  buf-FREE {:.3f} ms ago tid={} ptr=0x{:08X} caller=0x{:08X} chain={:08X} {:08X} {:08X} {:08X}",
                  double(now - e.us) / 1000.0, e.tid, e.ptr, e.caller, e.chain[0], e.chain[1], e.chain[2], e.chain[3]);
    }
    REXLOG_WARN("  emitter-buf records for ptr 0x{:08X}: {} (of {}), tracked frees: {} (of {})", ptr, nb, bn, nf, fn);
  }
  const uint32_t n = g_set_n.load();
  uint32_t found = 0;
  for (uint32_t i = 0; i < kSet && i < n; ++i) {
    const SetEv& e = g_set[(n - 1 - i) % kSet];
    if (e.ptr != ptr) continue;
    ++found;
    REXLOG_WARN("  string-set {:.3f} ms ago tid={} ptr=0x{:08X} list=0x{:08X} id={} caller=0x{:08X} head-at-store={:08X} {:08X}",
                double(now - e.us) / 1000.0, e.tid, e.ptr, e.list, e.id, e.caller, e.h0, e.h1);
  }
  REXLOG_WARN("  string-set records for ptr 0x{:08X}: {} (of {} recorded, ring {})", ptr, found, n, kSet);
}

void Ao2dcLogListClear(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests)) return;
  AddEv(6, r31.u32, 0, 0, 0, 0);
}
void Ao2dcLogListClearB(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_diag_fs_requests)) return;
  AddEv(7, r31.u32, 0, 0, 0, 0);
}
