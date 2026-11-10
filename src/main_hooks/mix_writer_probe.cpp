// AO2DC diagnostic: find the guest code that writes the interleaved 6-channel frame the render
// driver client submits to the host, then walk one stage further back.
//
// sub_83CA7D40 (render driver client callback, reached through sub_83CA7F60) waits for its two
// events, then copies the interleaved float buffer at *(client+60) (256 frames x 6 channels,
// stride 24 bytes) into a planar static buffer, taps it (sub_83CA7B80, a plain 6144-byte copy) and
// calls XAudioSubmitRenderDriverFrame. Neither the copy nor the tap clamps.
//
// Round 1 of this probe (page guard on that buffer) showed one writer: sub_83CABE50, a VMX
// volume-ramp kernel (dst = src * ramped gain, optionally += dst) that is only called through a
// function pointer and contains no min/max. This version adds:
//   * an entry hook on sub_83CABE50 that records the parameter block of every call whose
//     destination overlaps the submitted buffer: source pointer, raw gain words, count, layout,
//     accumulate flag, and the source's peak and exact +-1.0 sample counts;
//   * host stack backtraces (unwound from the faulting context) for each distinct accessor, mapped
//     to guest functions through PPCFuncMappings, so indirect callers are named;
//   * alternating guard targets: odd rounds guard the submitted buffer, even rounds guard the most
//     recent kernel source buffer, to find the writer one stage up.
// Diagnostic only: no audio data is changed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_mix_probe, false, "AO2DC",
                    "Diagnostic: page-guard the submitted interleaved mix buffer (and the source of "
                    "the kernel that writes it) for a few frames at a time and log which generated "
                    "functions access them. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_mix_probe_gain, false, "AO2DC",
                    "Diagnostic: when an AudioCore gain plugin (sub_83887E78) runs with a target "
                    "gain above 1.0, page-guard its +52 target word for ~1 s and log every writer "
                    "(guest stack) and the value written. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_gain_hw_watch, false, "AO2DC",
                    "Diagnostic: when an AudioCore gain plugin (sub_83887E78) runs with target "
                    "exactly 4.0, set hardware write breakpoints (DR0/DR1) on its +52/+56 words "
                    "in every thread and log each writer's guest stack and value (the slot is "
                    "re-initialised when the voice is reused). Restart required.");

REXCVAR_DEFINE_DOUBLE(ao2dc_gain_hw_watch_min, 4.0, "AO2DC",
                      "Diagnostic: target gain at or above which ao2dc_gain_hw_watch dumps the voice "
                      "identity and arms the write watch on the first such gain plugin.");

REXCVAR_DEFINE_BOOL(ao2dc_create_watch, false, "AO2DC",
                    "Diagnostic: when sub_8386AEC8 initialises an AudioCore node/plugin object, "
                    "page-guard the region being constructed for up to 30 ms and log every write "
                    "of a gain plugin's +52 target (or any float 1.01..1000) with the writer's "
                    "guest stack. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_gain_init_watch, false, "AO2DC",
                    "Diagnostic: at the end of the AudioCore gain plugin init (sub_8388AA40, "
                    "0x8388AA80) arm a hardware write breakpoint on the new plugin's +52 target in "
                    "every thread (the creating thread immediately), so the parameter write that "
                    "follows creation is logged with its guest stack and value. First 60 plugins.");

REXCVAR_DEFINE_BOOL(ao2dc_param_set_log, false, "AO2DC",
                    "Diagnostic: log AudioCore parameter-set requests (sub_83869D08: plugin, "
                    "index, value) on gain plugins with a value above 1, with the guest call "
                    "stack. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_staging_watch, false, "AO2DC",
                    "Diagnostic: at the game's parameter flush (0x82E2CCDC in sub_82E2CC20) arm a "
                    "hardware write breakpoint on the staging slot that feeds a boosted (>= 3) "
                    "SndPlayer-voice gain, to catch the code that computes it. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_gain_calc_log, false, "AO2DC",
                    "Diagnostic: log the inputs of the game's voice gain calculation in "
                    "sub_82E2A040 (0x82E2B41C) when the resulting gain is >= 2.5. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_stage_set_log, false, "AO2DC",
                    "Diagnostic: log values >= 2.5 staged by sub_82E2CFC0 (game parameter staging) "
                    "with the guest call stack, deduplicated. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_voice56_watch, false, "AO2DC",
                    "Diagnostic: when the game's gain calculation sees a boosted gain (>= 2.5), arm "
                    "a hardware write breakpoint on that voice object's +56 factor and log its "
                    "writers. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_xas_capture, false, "AO2DC",
                    "Diagnostic: capture raw EA-XAS blocks (76 bytes) and the float output of the "
                    "decoder sub_8388FE88 (128 samples) for offline reference decoding; writes "
                    "logs/xas_blocks.bin. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_entry_gain_watch, false, "AO2DC",
                    "Diagnostic: after the sample-entry constructor sub_83ED8858 sets entry+20 = "
                    "1.0, arm a hardware write breakpoint on entry+20 in the constructing thread and "
                    "log the writer of the real per-sample gain. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_comp_param_log, false, "AO2DC",
                    "Diagnostic: log every AudioCore parameter set and staged value aimed at "
                    "compressor plugins (descriptor 0x8409400C), and dump the compressor "
                    "descriptor. Restart required.");

REXCVAR_DEFINE_BOOL(ao2dc_sndplayer_log, false, "AO2DC",
                    "Diagnostic: log AudioCore SndPlayer (sub_83876660) starvation episodes "
                    "(returns 0: no ready data slot / no active segment) and segment format "
                    "switches, with segment and slot-ring state. Restart required.");

extern PPCFuncMapping PPCFuncMappings[];

namespace {

constexpr uint32_t kOutBytes = 256 * 6 * 4;
constexpr uint32_t kMaxTargetBytes = 64 * 1024;
constexpr int kMaxHits = 256;
constexpr int kStack = 10;
constexpr int kArmFrames = 3;
constexpr int kArmFramesQueued = 40;
constexpr int kMaxRounds = 30;
constexpr uint64_t kRoundIntervalFrames = 1875;  // ~10 s at 187.5 frames/s

struct Hit {
  uint32_t value_after;
  uint32_t values_seen;
  uint64_t rip;
  uint32_t thread;
  uint32_t write;
  uint32_t count;
  uint32_t off_min;
  uint32_t off_max;
  uint64_t stack[kStack];
};

std::atomic<bool> armed{false};
uint8_t* membase = nullptr;
uint32_t out_guest = 0;
uint32_t target_guest = 0, target_bytes = 0;
uint8_t* target_host = nullptr;
uintptr_t page_lo = 0, page_hi = 0;  // [lo, hi)
std::atomic_flag hits_lock = ATOMIC_FLAG_INIT;
Hit hits[kMaxHits];
int n_hits = 0;
uint64_t overflow = 0;
uint64_t frame_count = 0;
uint64_t armed_at = 0;
int rounds = 0;
thread_local uintptr_t rearm_page = 0;
thread_local int rearm_hit = -1;
bool gain_mode = false;
PVOID veh = nullptr;

// Kernel (sub_83CABE50) calls that write the submitted buffer.
struct KernelCall {
  uint64_t frame;
  uint32_t block[10];   // raw parameter block words +0..+36
  uint32_t gain0[16];   // 16 words at *(block+8)
  uint32_t gain1[16];   // 16 words at *(block+12), when non-null
  float src_peak;
  uint32_t src_at_one;   // samples with |x| == 1.0 exactly
  uint32_t src_over_one; // samples with |x| > 1.0
  uint32_t thread;
};
std::mutex kc_mutex;  // kernel runs on one mix thread; the logger reads under the same lock
KernelCall kc_last[8];
uint32_t kc_last_n = 0;
uint64_t kc_calls = 0;
uint64_t kc_frames_with_rails = 0;
uint64_t kc_rail_logs = 0;
uint32_t last_src = 0, last_src_bytes = 0;
uint64_t kc_mode_counts[8] = {};
struct Feeder {
  uint32_t src;
  uint32_t bytes;
  uint64_t calls;
  uint64_t last_frame;
  float peak;
};
constexpr int kMaxFeeders = 64;
Feeder feeders[kMaxFeeders];
int feeder_n = 0;
uint64_t feeder_rail_logs = 0;
float hot_peak = 0.0f;
uint32_t hot_src = 0, hot_bytes = 0;

inline float Bits32(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

void Guard(bool on) {
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(page_lo), page_hi - page_lo,
                 on ? (PAGE_READWRITE | PAGE_GUARD) : PAGE_READWRITE, &old);
}

void Unwind(const CONTEXT* start, uint64_t* out) {
  CONTEXT c = *start;
  for (int i = 0; i < kStack; ++i) {
    out[i] = c.Rip;
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
    if (!c.Rip) {
      for (int j = i + 1; j < kStack; ++j) out[j] = 0;
      break;
    }
  }
}

void Record(const CONTEXT* ctx, uintptr_t addr, bool write) {
  const uint64_t rip = ctx->Rip;
  const uint32_t off = uint32_t(addr - reinterpret_cast<uintptr_t>(target_host));
  const uint32_t tid = GetCurrentThreadId();
  while (hits_lock.test_and_set(std::memory_order_acquire)) {
  }
  int i = 0;
  for (; i < n_hits; ++i) {
    if (hits[i].rip == rip && hits[i].thread == tid && hits[i].write == uint32_t(write)) break;
  }
  bool fresh = false;
  if (i == n_hits) {
    if (n_hits < kMaxHits) {
      Hit& h = hits[n_hits++];
      h = Hit{0, 0, rip, tid, uint32_t(write), 0, off, off, {}};
      fresh = true;
    } else {
      ++overflow;
      i = -1;
    }
  }
  if (i >= 0) {
    Hit& h = hits[i];
    ++h.count;
    h.off_min = std::min(h.off_min, off);
    h.off_max = std::max(h.off_max, off);
  }
  hits_lock.clear(std::memory_order_release);
  if (fresh) Unwind(ctx, hits[i].stack);  // slot is owned by this new entry
}

// Creation watch: writes into a node/plugin region while it is being constructed.
struct CreateHit {
  uint64_t t_ns;
  uint32_t addr;       // guest address written
  uint32_t value;      // value after the write
  uint32_t region;     // guest start of the guarded region (r3 - 8 at sub_8386AEC8)
  uint32_t desc;       // descriptor word at (addr - 52 + 20) when written (gain = 0x84093270)
  uint32_t thread;
  uint64_t stack[kStack];
};
constexpr int kMaxCreate = 512;
CreateHit create_hits[kMaxCreate];
std::atomic<int> create_n{0};
std::atomic<bool> create_armed{false};
uint32_t create_lo = 0, create_hi = 0;  // guest [lo, hi)
uintptr_t create_page_lo = 0, create_page_hi = 0;
uint64_t create_armed_at = 0;
thread_local uint32_t create_pending_addr = 0;
thread_local CONTEXT create_pending_ctx;

// Gain init watch: DR0 on the newest gain plugin's +52.
constexpr DWORD kArmSelfCode = 0xE0A0D0C1;
std::atomic<uint32_t> init_watch_guest{0};
struct InitHit {
  uint64_t t_ns;
  uint32_t plugin;
  uint32_t value;
  uint32_t node;
  uint32_t gen0_desc;
  uint32_t thread;
  uint32_t arm_index;
  uint64_t stack[kStack];
};
constexpr int kMaxInit = 400;
InitHit init_hits[kMaxInit];
std::atomic<int> init_n{0};
std::atomic<uint32_t> init_arm_index{0};

struct HwHit {
  uint64_t t_ns;
  uint32_t thread;
  uint32_t which;  // 0 = +52 target, 1 = +56 current
  uint32_t value;
  uint64_t stack[kStack];
};
constexpr int kMaxHw = 64;
HwHit hw_hits[kMaxHw];
std::atomic<int> hw_n{0};
uint8_t* hw_addr[2] = {nullptr, nullptr};
std::atomic<bool> hw_armed{false};

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == kArmSelfCode) {
    // Arm DR0 (write, 4 bytes) on the calling thread itself.
    const uint32_t g = init_watch_guest.load();
    ep->ContextRecord->Dr0 = reinterpret_cast<DWORD64>(membase + g);
    ep->ContextRecord->Dr7 =
        (ep->ContextRecord->Dr7 & ~0x000F0003ull) | 0x1 | (0x1ull << 16) | (0x3ull << 18);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 1) && init_watch_guest.load()) {
    // Use the address this thread's DR0 actually holds: a thread can still carry the previous
    // arm until the helper re-arms it, and every DR0 hit must be claimed here.
    const uint32_t g = uint32_t(ep->ContextRecord->Dr0 - reinterpret_cast<DWORD64>(membase));
    const int n = init_n.fetch_add(1);
    if (n < kMaxInit) {
      InitHit& h = init_hits[n];
      h.t_ns = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
      h.plugin = g - 52;
      uint32_t v;
      std::memcpy(&v, membase + g, 4);
      h.value = __builtin_bswap32(v);
      std::memcpy(&v, membase + g - 52 + 12, 4);
      h.node = __builtin_bswap32(v);
      h.gen0_desc = 0;
      if (h.node >= 0x40000000u && h.node < 0x50000000u) {
        std::memcpy(&v, membase + h.node + 80, 4);
        const uint32_t gen0 = __builtin_bswap32(v);
        if (gen0 >= 0x40000000u && gen0 < 0x50000000u) {
          std::memcpy(&v, membase + gen0 + 20, 4);
          h.gen0_desc = __builtin_bswap32(v);
        }
      }
      h.thread = GetCurrentThreadId();
      h.arm_index = init_arm_index.load();
      Unwind(ep->ContextRecord, h.stack);
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == STATUS_GUARD_PAGE_VIOLATION && create_armed.load(std::memory_order_acquire)) {
    const uintptr_t a = uintptr_t(ep->ExceptionRecord->ExceptionInformation[1]);
    if (a >= create_page_lo && a < create_page_hi) {
      const uint32_t g = uint32_t(a - reinterpret_cast<uintptr_t>(membase));
      if (ep->ExceptionRecord->ExceptionInformation[0] == 1 && g >= create_lo && g < create_hi) {
        create_pending_addr = g & ~3u;
        create_pending_ctx = *ep->ContextRecord;
      }
      rearm_page = a & ~uintptr_t(0xFFF);
      ep->ContextRecord->EFlags |= 0x100;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  if (code == EXCEPTION_SINGLE_STEP && rearm_page && create_page_lo &&
      rearm_page >= create_page_lo && rearm_page < create_page_hi) {
    if (create_pending_addr) {
      const uint32_t g = create_pending_addr;
      create_pending_addr = 0;
      uint32_t v;
      std::memcpy(&v, membase + g, 4);
      v = __builtin_bswap32(v);
      float f;
      std::memcpy(&f, &v, 4);
      uint32_t desc = 0;
      if (g >= 52 + 0x40000000u) {
        uint32_t d;
        std::memcpy(&d, membase + g - 52 + 20, 4);
        desc = __builtin_bswap32(d);
      }
      const bool gain_target = desc == 0x84093270u;
      const bool boost = std::fabs(f) >= 1.01f && std::fabs(f) <= 1000.0f;
      if (gain_target || boost) {
        const int n = create_n.fetch_add(1);
        if (n < kMaxCreate) {
          CreateHit& h = create_hits[n];
          h.t_ns = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
          h.addr = g;
          h.value = v;
          h.region = create_lo;
          h.desc = desc;
          h.thread = GetCurrentThreadId();
          Unwind(&create_pending_ctx, h.stack);
        }
      }
    }
    if (create_armed.load(std::memory_order_acquire)) {
      DWORD old = 0;
      VirtualProtect(reinterpret_cast<void*>(rearm_page), 0x1000, PAGE_READWRITE | PAGE_GUARD,
                     &old);
    }
    rearm_page = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && hw_armed.load(std::memory_order_acquire) &&
      (ep->ContextRecord->Dr6 & 3)) {
    const uint32_t which = (ep->ContextRecord->Dr6 & 1) ? 0 : 1;
    const int n = hw_n.fetch_add(1);
    if (n < kMaxHw) {
      HwHit& h = hw_hits[n];
      h.t_ns = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
      h.thread = GetCurrentThreadId();
      h.which = which;
      uint32_t v;
      std::memcpy(&v, hw_addr[which], 4);
      h.value = __builtin_bswap32(v);
      Unwind(ep->ContextRecord, h.stack);
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == STATUS_GUARD_PAGE_VIOLATION) {
    const uintptr_t addr = uintptr_t(ep->ExceptionRecord->ExceptionInformation[1]);
    if (addr < page_lo || addr >= page_hi) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t b = reinterpret_cast<uintptr_t>(target_host);
    if (addr >= b && addr < b + target_bytes) {
      Record(ep->ContextRecord, addr, ep->ExceptionRecord->ExceptionInformation[0] == 1);
      if (gain_mode && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        // Find the entry just recorded so the single step can read the value written.
        const uint64_t rip = ep->ContextRecord->Rip;
        const uint32_t tid = GetCurrentThreadId();
        for (int i = 0; i < n_hits; ++i)
          if (hits[i].rip == rip && hits[i].thread == tid && hits[i].write) rearm_hit = i;
      }
    }
    rearm_page = addr & ~uintptr_t(0xFFF);
    ep->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && !rearm_page && !create_pending_addr &&
      (ep->ContextRecord->Dr6 & 0xF) && (init_watch_guest.load() || hw_armed.load())) {
    ep->ContextRecord->Dr6 = 0;  // stale breakpoint from an earlier arm: ignore
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && rearm_page) {
    if (rearm_hit >= 0) {
      uint32_t v;
      std::memcpy(&v, target_host, 4);
      v = __builtin_bswap32(v);
      if (hits[rearm_hit].value_after != v) hits[rearm_hit].values_seen++;
      hits[rearm_hit].value_after = v;
      rearm_hit = -1;
    }
    if (armed.load(std::memory_order_acquire)) {
      DWORD old = 0;
      VirtualProtect(reinterpret_cast<void*>(rearm_page), 0x1000, PAGE_READWRITE | PAGE_GUARD,
                     &old);
    }
    rearm_page = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

struct FnMap {
  std::vector<PPCFuncMapping> maps;
  FnMap() {
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) maps.push_back(*m);
    std::sort(maps.begin(), maps.end(), [](const PPCFuncMapping& a, const PPCFuncMapping& b) {
      return reinterpret_cast<uintptr_t>(a.host) < reinterpret_cast<uintptr_t>(b.host);
    });
  }
  std::string Name(uint64_t rip) const {
    if (!rip) return "-";
    auto it = std::upper_bound(maps.begin(), maps.end(), rip,
                               [](uint64_t r, const PPCFuncMapping& m) {
                                 return r < reinterpret_cast<uintptr_t>(m.host);
                               });
    if (it == maps.begin()) return fmt::format("host:{:X}", rip);
    --it;
    const uint64_t d = rip - reinterpret_cast<uintptr_t>(it->host);
    if (d > 0x40000) return fmt::format("host:{:X}", rip);  // outside generated code
    return fmt::format("sub_{:08X}+0x{:X}", uint32_t(it->guest), d);
  }
};

void Report(std::vector<Hit> snap, uint64_t ovf, int round, uint32_t guest, uint32_t bytes,
            bool is_out) {
  static FnMap fmap;
  std::sort(snap.begin(), snap.end(), [](const Hit& a, const Hit& b) { return a.count > b.count; });
  REXLOG_INFO("AO2DC mix probe round {}: {} buffer guest {:08X} ({} bytes), {} distinct accessors{}",
              round, is_out ? "SUBMITTED" : "NON-OUTPUT", guest, bytes, snap.size(),
              ovf ? fmt::format(" ({} dropped)", ovf) : std::string());
  for (const Hit& h : snap) {
    std::string stack;
    for (int i = 1; i < kStack && h.stack[i]; ++i) stack += " <- " + fmap.Name(h.stack[i]);
    float fv;
    std::memcpy(&fv, &h.value_after, 4);
    REXLOG_INFO("AO2DC mix probe:   {} by {} thread {} x{} bytes {}..{}{}{}",
                h.write ? "WRITE" : "read ", fmap.Name(h.rip), h.thread, h.count, h.off_min,
                h.off_max,
                h.write && gain_mode
                    ? fmt::format(" value after {:08X} ({:.6g}), {} changes", h.value_after, fv,
                                  h.values_seen)
                    : std::string(),
                stack);
  }
}

std::string Words(const uint32_t* w, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) {
    float f;
    std::memcpy(&f, &w[i], 4);
    s += fmt::format("{}{:08X}({:.4g})", i ? " " : "", w[i], f);
  }
  return s;
}

void LogKernelCall(const char* why, const KernelCall& k) {
  REXLOG_INFO(
      "AO2DC mix probe kernel [{}] frame {} thread {}: src {:08X} dst {:08X} g0 {:08X} g1 {:08X} "
      "+16 {:08X} count {} layout {} +28 {:08X} accumulate {} mono-flag {} | src peak {:.6g} "
      "exact+-1 {} over1 {}",
      why, k.frame, k.thread, k.block[0], k.block[1], k.block[2], k.block[3], k.block[4],
      k.block[5], k.block[6], k.block[7], k.block[8], k.block[9], k.src_peak, k.src_at_one,
      k.src_over_one);
  REXLOG_INFO("AO2DC mix probe kernel   gains@g0: {}", Words(k.gain0, 16));
  if (k.block[3]) REXLOG_INFO("AO2DC mix probe kernel   gains@g1: {}", Words(k.gain1, 16));
}

}  // namespace

// Per-frame record of kernel destinations, to tell whether a source buffer was produced by this
// same kernel earlier in the frame (a voice-to-voice send) or by some other stage.
constexpr int kMaxDst = 512;
uint32_t frame_dst[kMaxDst];
int frame_dst_n = 0;
uint64_t frame_dst_frame = ~0ull;
uint64_t rail_logs = 0;
std::atomic<uint32_t> origin_src{0}, origin_bytes{0};
std::atomic<uint32_t> rail_voice_in{0};  // first rail origin: a source voice's input scratch buffer
uint64_t voice_copy_logs = 0;
uint32_t queued_guarded = 0;

// Midasm hook at the entry of sub_83CABE50 (r3 = parameter block). Runs on the XAudio2 engine
// thread for every voice send/copy.
void Ao2dcMixProbeKernel(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_mix_probe) || !out_guest || !membase) return;
  const uint8_t* b = membase + r3.u32;
  uint32_t block[10];
  for (int i = 0; i < 10; ++i) block[i] = rex::memory::load_and_swap<uint32_t>(b + 4 * i);
  const uint32_t src = block[0], dst = block[1];
  const uint32_t count = std::min<uint32_t>(block[5], kMaxTargetBytes / 4);
  KernelCall k{};
  k.frame = frame_count;
  k.thread = GetCurrentThreadId();
  std::memcpy(k.block, block, sizeof(block));
  float peak = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t bits = rex::memory::load_and_swap<uint32_t>(membase + src + 4 * i);
    float v;
    std::memcpy(&v, &bits, 4);
    const float a = std::fabs(v);
    if (!(a <= peak)) peak = a;
    if (a == 1.0f) ++k.src_at_one;
    else if (a > 1.0f) ++k.src_over_one;
  }
  k.src_peak = peak;
  const bool to_out = !(dst + 4 <= out_guest || dst >= out_guest + kOutBytes);
  std::lock_guard<std::mutex> lock(kc_mutex);
  if (frame_dst_frame != frame_count) {
    frame_dst_frame = frame_count;
    frame_dst_n = 0;
  }
  bool src_from_kernel = false;
  for (int i = 0; i < frame_dst_n; ++i) {
    if (frame_dst[i] == src) {
      src_from_kernel = true;
      break;
    }
  }
  if (frame_dst_n < kMaxDst) frame_dst[frame_dst_n++] = dst;
  if (to_out) {
    ++kc_calls;
    kc_mode_counts[std::min<uint32_t>(block[6], 7)]++;
    last_src = src;
    last_src_bytes = std::min<uint32_t>(block[5] * 4, kMaxTargetBytes);
  }
  if (to_out || kc_last_n < 8) kc_last[kc_last_n++ % 8] = k;
  if ((k.src_at_one || k.src_over_one) && rail_logs < 300) {
    ++rail_logs;
    if (block[2]) {
      for (int i = 0; i < 16; ++i)
        k.gain0[i] = rex::memory::load_and_swap<uint32_t>(membase + block[2] + 4 * i);
    }
    if (block[3]) {
      for (int i = 0; i < 16; ++i)
        k.gain1[i] = rex::memory::load_and_swap<uint32_t>(membase + block[3] + 4 * i);
    }
    LogKernelCall(src_from_kernel ? "RAILS in source (source written by kernel this frame)"
                                  : "RAILS in source (ORIGIN: source not written by kernel this frame)",
                  k);
    if (!src_from_kernel && !rail_voice_in.load()) rail_voice_in.store(src);
    if (!src_from_kernel && !origin_src.load() && !queued_guarded) {
      origin_bytes.store(std::min<uint32_t>(block[5] * 4, kMaxTargetBytes));
      origin_src.store(src);
    }
  }
}


// Midasm hooks at 0x83CC7E8C and 0x83CC7EAC in sub_83CC7E18 (XAudio2 source voice: memcpy of the
// queued client buffer into the voice's input scratch). r3 = dst, r4 = src, r5 = bytes,
// r31 = voice; *(voice+184) = current buffer descriptor (+0 data, +16, +20).
void Ao2dcMixProbeVoiceCopy(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_mix_probe) || !membase) return;
  const uint32_t vin = rail_voice_in.load();
  if (!vin || r3.u32 + 4 <= vin || r3.u32 >= vin + kOutBytes) return;
  const uint32_t desc = rex::memory::load_and_swap<uint32_t>(membase + r31.u32 + 184);
  uint32_t d[8] = {};
  if (desc) {
    for (int i = 0; i < 8; ++i) d[i] = rex::memory::load_and_swap<uint32_t>(membase + desc + 4 * i);
  }
  const uint32_t n = std::min<uint32_t>(r5.u32, kMaxTargetBytes) / 4;
  uint32_t at_one = 0, over = 0;
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t bits = rex::memory::load_and_swap<uint32_t>(membase + r4.u32 + 4 * i);
    float v;
    std::memcpy(&v, &bits, 4);
    const float a = std::fabs(v);
    if (!(a <= peak)) peak = a;
    if (a == 1.0f) ++at_one;
    else if (a > 1.0f) ++over;
  }
  std::lock_guard<std::mutex> lock(kc_mutex);
  if ((at_one || over || voice_copy_logs < 3) && voice_copy_logs < 80) {
    ++voice_copy_logs;
    REXLOG_INFO(
        "AO2DC mix probe voice copy frame {} voice {:08X}: dst {:08X} src {:08X} bytes {} | desc "
        "{:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} | src peak {:.6g} "
        "exact+-1 {} over1 {}",
        frame_count, r31.u32, r3.u32, r4.u32, r5.u32, desc, d[0], d[1], d[2], d[3], d[4], d[5],
        d[6], d[7], peak, at_one, over);
  }
  if ((at_one || over) && !queued_guarded && d[0]) {
    // Guard the whole queued client buffer: d[0] = data, the extent is d[4] + d[5] bytes
    // (the voice compares its read position against data + [16] + [20]).
    const uint32_t len = std::min<uint32_t>(d[4] + d[5], kMaxTargetBytes);
    queued_guarded = d[0];
    origin_bytes.store(len ? len : kOutBytes);
    origin_src.store(d[0]);
    REXLOG_INFO("AO2DC mix probe: rails are already in the client buffer queued on voice {:08X}; "
                "guarding {:08X} ({} bytes) next",
                r31.u32, d[0], len);
  }
}



// Sets DR0/DR1 (write, 4 bytes) on every thread of the process. Runs on its own thread so the
// requesting audio thread is covered too.
void ArmHwWatch(uint32_t guest_target) {
  hw_addr[0] = membase + guest_target;
  hw_addr[1] = membase + guest_target + 4;
  if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
  hw_armed.store(true, std::memory_order_release);
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  int armed_threads = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                           te.th32ThreadID);
    if (!th) continue;
    if (SuspendThread(th) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(th, &c)) {
        c.Dr0 = reinterpret_cast<DWORD64>(hw_addr[0]);
        // L0 only: RW0 = 01 (write), LEN0 = 11 (4 bytes) on the +52 target word. (+56 is
        // rewritten by the gain process every block and would flood the hit buffer.)
        c.Dr7 = (c.Dr7 & ~0x00FF000Full) | 0x1 | (0x1ull << 16) | (0x3ull << 18);
        if (SetThreadContext(th, &c)) ++armed_threads;
      }
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
  REXLOG_INFO("AO2DC gain hw watch: DR0 on {:08X} (+52 target) armed in {} threads",
              guest_target, armed_threads);
  // Report new hits as they arrive (up to kMaxHw).
  static FnMap fmap;
  int reported = 0;
  for (int idle = 0; idle < 3000 && reported < kMaxHw; ++idle) {  // up to ~5 minutes
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int n = std::min(hw_n.load(), kMaxHw);
    while (reported < n) {
      const HwHit& h = hw_hits[reported++];
      float f;
      std::memcpy(&f, &h.value, 4);
      std::string stack;
      for (int i = 1; i < kStack && h.stack[i]; ++i) stack += " <- " + fmap.Name(h.stack[i]);
      REXLOG_INFO("AO2DC gain hw watch hit {}: write {} -> {:08X} ({:.6g}) by {} thread {}{}",
                  reported, h.which ? "+56 current" : "+52 target", h.value, f,
                  fmap.Name(h.stack[0]), h.thread, stack);
      idle = 0;
    }
  }
  REXLOG_INFO("AO2DC gain hw watch: reporter finished after {} hits", reported);
}


// Readable ASCII runs (>= 5 chars) in guest memory [addr, addr+len).
std::string Strings(uint32_t addr, uint32_t len) {
  std::string out, cur;
  for (uint32_t i = 0; i < len; ++i) {
    const char ch = char(membase[addr + i]);
    if (ch >= 0x20 && ch < 0x7F) {
      cur += ch;
    } else {
      if (cur.size() >= 5) out += (out.empty() ? "" : " | ") + cur;
      cur.clear();
    }
  }
  if (cur.size() >= 5) out += (out.empty() ? "" : " | ") + cur;
  return out;
}

// A guest address worth following: heap (0x40000000-0x4FFFFFFF) or image data below the code
// (0x82000000-0x82FFFFFF), and committed + readable on the host. Code (0x83...) is never followed.
bool GuestPtr(uint32_t v) {
  if (!((v >= 0x40000000u && v < 0x50000000u) || (v >= 0x82000000u && v < 0x83000000u)))
    return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(membase + v, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<uintptr_t>(membase + v) + 256 <= end;
}

// One-time identity dump of a voice node holding a boosted gain plugin.
void DumpVoiceIdentity(uint32_t gain_plugin) {
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  const uint32_t node = W(gain_plugin + 12);
  std::string nw;
  for (int i = 0; i < 32; ++i) nw += fmt::format("{:08X} ", W(node + 4 * i));
  REXLOG_INFO("AO2DC voice identity: node {:08X} (gain plugin {:08X}, target {:08X}) words: {}",
              node, gain_plugin, W(gain_plugin + 52), nw);
  const uint32_t count = membase[node + 68];
  const uint32_t last_gen = membase[node + 70];
  for (uint32_t i = 0; i < count && i < 12; ++i) {
    const uint32_t pl = W(node + 80 + 4 * i);
    if (!pl) continue;
    std::string pw;
    for (int k = 0; k < 40; ++k) pw += fmt::format("{:08X} ", W(pl + 4 * k));
    REXLOG_INFO("AO2DC voice identity:   plugin #{} {:08X} descriptor {:08X} nch {} words: {}", i,
                pl, W(pl + 20), membase[pl + 42], pw);
    if (last_gen != 0xFF && i > last_gen) continue;  // follow pointers of generators only
    for (int k = 6; k < 40; ++k) {
      const uint32_t ptr = W(pl + 4 * k);
      if (!GuestPtr(ptr) || ptr == pl || ptr == node) continue;
      const std::string str = Strings(ptr, 256);
      std::string pw2;
      for (int m = 0; m < 16; ++m) pw2 += fmt::format("{:08X} ", W(ptr + 4 * m));
      REXLOG_INFO("AO2DC voice identity:     #{} +0x{:X} -> {:08X}: {}{}", i, 4 * k, ptr, pw2,
                  str.empty() ? std::string() : " strings: " + str);
      for (int m = 0; m < 16; ++m) {
        const uint32_t p2 = W(ptr + 4 * m);
        if (!GuestPtr(p2) || p2 == ptr) continue;
        const std::string s2 = Strings(p2, 128);
        if (!s2.empty())
          REXLOG_INFO("AO2DC voice identity:       {:08X}+0x{:X} -> {:08X} strings: {}", ptr,
                      4 * m, p2, s2);
      }
    }
  }
}


void CreateReporter() {
  static FnMap fmap;
  int reported = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Disarm a region after 30 ms.
    if (create_armed.load() &&
        uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) - create_armed_at >
            30000000ull) {
      create_armed.store(false);
      DWORD old = 0;
      VirtualProtect(reinterpret_cast<void*>(create_page_lo), create_page_hi - create_page_lo,
                     PAGE_READWRITE, &old);
    }
    const int n = std::min(create_n.load(), kMaxCreate);
    while (reported < n) {
      const CreateHit& h = create_hits[reported++];
      float f;
      std::memcpy(&f, &h.value, 4);
      std::string stack;
      for (int i = 1; i < kStack && h.stack[i]; ++i) stack += " <- " + fmap.Name(h.stack[i]);
      REXLOG_INFO("AO2DC create watch {}: {} write {:08X} = {:08X} ({:.6g}) region {:08X}+0x{:X} "
                  "by {} thread {}{}",
                  reported, h.desc == 0x84093270u ? "GAIN-TARGET" : "float", h.addr, h.value, f,
                  h.region, h.addr - h.region, fmap.Name(h.stack[0]), h.thread, stack);
    }
  }
}

// Entry of sub_8386AEC8: r3 = embedded object being initialised (voice nodes embed it at +8).
void Ao2dcCreateWatchInit(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_create_watch)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  static std::atomic<int> arms{0};
  static std::atomic<bool> reporter{false};
  if (!reporter.exchange(true)) {
    if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
    std::thread(CreateReporter).detach();
  }
  // Only AudioCore objects in the guest heap (voice nodes live at 0x42000000-0x47FFFFFF); never
  // static image data, which host/kernel code also touches. And not during startup.
  if (r3.u32 < 0x42000000u || r3.u32 >= 0x48000000u) return;
  static const uint64_t t_start =
      uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  if (now - t_start < 20000000000ull) return;
  if (create_armed.load()) {
    if (now - create_armed_at < 30000000ull) return;  // one region at a time
    create_armed.store(false);
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(create_page_lo), create_page_hi - create_page_lo,
                   PAGE_READWRITE, &old);
  }
  if (arms.fetch_add(1) >= 600 || create_n.load() >= kMaxCreate) return;
  create_lo = r3.u32 - 8;
  create_hi = create_lo + 0x900;
  create_page_lo = reinterpret_cast<uintptr_t>(membase + create_lo) & ~uintptr_t(0xFFF);
  create_page_hi = (reinterpret_cast<uintptr_t>(membase + create_hi) + 0xFFF) & ~uintptr_t(0xFFF);
  create_armed_at = now;
  create_armed.store(true, std::memory_order_release);
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(create_page_lo), create_page_hi - create_page_lo,
                 PAGE_READWRITE | PAGE_GUARD, &old);
}


void SetDr0AllThreads(uint32_t guest, DWORD skip) {
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == skip ||
        te.th32ThreadID == GetCurrentThreadId())
      continue;
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                           te.th32ThreadID);
    if (!th) continue;
    if (SuspendThread(th) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(th, &c)) {
        c.Dr0 = reinterpret_cast<DWORD64>(membase + guest);
        c.Dr7 = (c.Dr7 & ~0x000F0003ull) | 0x1 | (0x1ull << 16) | (0x3ull << 18);
        SetThreadContext(th, &c);
      }
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
}

void InitWatchReporter() {
  static FnMap fmap;
  int reported = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int n = std::min(init_n.load(), kMaxInit);
    while (reported < n) {
      const InitHit& h = init_hits[reported++];
      float f;
      std::memcpy(&f, &h.value, 4);
      std::string stack;
      for (int i = 1; i < kStack && h.stack[i]; ++i) stack += " <- " + fmap.Name(h.stack[i]);
      if (REXCVAR_GET(ao2dc_entry_gain_watch) && h.value == 0x3F800000u) continue;
      if (REXCVAR_GET(ao2dc_staging_watch) || REXCVAR_GET(ao2dc_voice56_watch) ||
          REXCVAR_GET(ao2dc_entry_gain_watch)) {
        REXLOG_INFO("AO2DC watch hit {} (arm {}): slot {:08X} <- {:08X} ({:.6g}) by {} "
                    "thread {}{}",
                    reported, h.arm_index, h.plugin + 52, h.value, f, fmap.Name(h.stack[0]),
                    h.thread, stack);
      } else {
        REXLOG_INFO("AO2DC gain init watch hit {} (arm {}): plugin {:08X} node {:08X} gen0 {:08X}{} "
                    "+52 <- {:08X} ({:.6g}) by {} thread {}{}",
                    reported, h.arm_index, h.plugin, h.node, h.gen0_desc,
                    h.gen0_desc == 0x8408C750u ? " (SndPlayer voice)" : "", h.value, f,
                    fmap.Name(h.stack[0]), h.thread, stack);
      }
    }
  }
}

// 0x8388AA80 at the end of sub_8388AA40 (gain plugin init): r31 = the new plugin.
void Ao2dcGainInitWatch(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_gain_init_watch)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  static std::atomic<bool> started{false};
  if (!started.exchange(true)) {
    if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
    std::thread(InitWatchReporter).detach();
  }
  static std::atomic<uint32_t> arms{0};
  if (r31.u32 < 0x42000000u || r31.u32 >= 0x48000000u) return;
  if (arms.load() >= 60) return;
  static std::atomic<uint64_t> last_arm{0};
  const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  if (now - last_arm.load() < 250000000ull) return;  // at most 4 arms per second
  last_arm.store(now);
  const uint32_t idx = arms.fetch_add(1) + 1;
  init_arm_index.store(idx);
  init_watch_guest.store(r31.u32 + 52);
  // This thread now (it may write the parameter right after init) ...
  RaiseException(kArmSelfCode, 0, 0, nullptr);
  // ... and every other thread from a helper.
  const DWORD self = GetCurrentThreadId();
  std::thread(SetDr0AllThreads, r31.u32 + 52, self).detach();
}


// Entry of sub_83869D08 (AudioCore parameter set: r3 = plugin, r4 = parameter index, f1 = value;
// it queues {0x838699F8, plugin, index, value} for the mixer thread).
void CompParamLog(const char* what, uint32_t plugin, uint32_t index, float value) {
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  static std::atomic<bool> dumped{false};
  if (!dumped.exchange(true)) {
    for (uint32_t base : {0x84094000u, 0x8409400Cu}) {
      std::string w;
      for (int i = 0; i < 40; ++i) w += fmt::format("{:08X} ", W(base + 4 * i));
      REXLOG_INFO("AO2DC compressor descriptor {:08X}: {}", base, w);
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 800) return;
  void* frames[24];
  const USHORT nf = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
  static FnMap fmap;
  std::string stack;
  int shown = 0;
  for (USHORT i = 0; i < nf && shown < 8; ++i) {
    const std::string nm = fmap.Name(reinterpret_cast<uint64_t>(frames[i]));
    if (nm.rfind("sub_", 0) != 0) continue;
    stack += (shown ? " <- " : "") + nm;
    ++shown;
  }
  std::string pw;
  for (int k = 12; k < 24; ++k) pw += fmt::format("{:08X} ", W(plugin + 4 * k));
  REXLOG_INFO("AO2DC comp {}: plugin {:08X} node {:08X} param {} <- {:.6g} | now +48..: {}| {}", what,
              plugin, W(plugin + 12), index, value, pw, stack);
}

void Ao2dcParamSet(PPCRegister& r3, PPCRegister& r4, PPCRegister& f1) {
  if (REXCVAR_GET(ao2dc_comp_param_log)) {
    if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
    if (r3.u32 >= 0x40000000u && r3.u32 < 0x50000000u &&
        rex::memory::load_and_swap<uint32_t>(membase + r3.u32 + 20) == 0x8409400Cu)
      CompParamLog("param set", r3.u32, r4.u32, float(f1.f64));
  }
  if (!REXCVAR_GET(ao2dc_param_set_log)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  const float value = float(f1.f64);
  if (!(value > 1.0f)) return;
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  if (r3.u32 < 0x40000000u || r3.u32 >= 0x50000000u) return;
  const uint32_t desc = W(r3.u32 + 20);
  if (desc != 0x84093270u) return;
  {
    static std::mutex m;
    static uint32_t last_plugin[256], last_value[256];
    uint32_t vb0;
    std::memcpy(&vb0, &value, 4);
    std::lock_guard<std::mutex> lock(m);
    const uint32_t h = (r3.u32 >> 4) & 255;
    if (last_plugin[h] == r3.u32 && last_value[h] == vb0) return;
    last_plugin[h] = r3.u32;
    last_value[h] = vb0;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 600) return;
  const uint32_t node = W(r3.u32 + 12);
  uint32_t gen0 = 0, gen0_desc = 0;
  if (node >= 0x40000000u && node < 0x50000000u) {
    gen0 = W(node + 80);
    if (gen0 >= 0x40000000u && gen0 < 0x50000000u) gen0_desc = W(gen0 + 20);
  }
  void* frames[24];
  const USHORT nf = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
  static FnMap fmap;
  std::string stack;
  int shown = 0;
  for (USHORT i = 0; i < nf && shown < 12; ++i) {
    const std::string nm = fmap.Name(reinterpret_cast<uint64_t>(frames[i]));
    if (nm.rfind("sub_", 0) != 0) continue;  // keep guest frames only
    stack += (shown ? " <- " : "") + nm;
    ++shown;
  }
  uint32_t vb;
  std::memcpy(&vb, &value, 4);
  REXLOG_INFO("AO2DC param set: gain plugin {:08X} node {:08X}{} param {} <- {:08X} ({:.6g}) "
              "[{:.2f} dB] thread {} : {}",
              r3.u32, node, gen0_desc == 0x8408C750u ? " SndPlayer" : "", r4.u32, vb, value,
              20.0 * std::log10(double(value)), GetCurrentThreadId(), stack);
}


// 0x82E2CCDC in sub_82E2CC20 (game parameter flush), before "lfs f1,4(r11)":
// r11 + 4 = staging slot of parameter r30 of plugin r27.
void Ao2dcStagingWatch(PPCRegister& r11, PPCRegister& r27, PPCRegister& r30) {
  if (!REXCVAR_GET(ao2dc_staging_watch)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  static std::atomic<bool> started{false};
  if (!started.exchange(true)) {
    if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
    std::thread(InitWatchReporter).detach();
  }
  const uint32_t plugin = r27.u32;
  if (plugin < 0x40000000u || plugin >= 0x50000000u || W(plugin + 20) != 0x84093270u) return;
  const uint32_t slot = r11.u32 + 4;
  const uint32_t vb = W(slot);
  float value;
  std::memcpy(&value, &vb, 4);
  if (!(value >= 3.0f)) return;
  const uint32_t node = W(plugin + 12);
  const uint32_t gen0 = (node >= 0x40000000u && node < 0x50000000u) ? W(node + 80) : 0;
  if (!gen0 || W(gen0 + 20) != 0x8408C750u) return;
  static std::atomic<uint32_t> arms{0};
  static std::atomic<uint64_t> last_arm{0};
  const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  if (arms.load() >= 20 || now - last_arm.load() < 2000000000ull) return;
  last_arm.store(now);
  const uint32_t idx = arms.fetch_add(1) + 1;
  REXLOG_INFO("AO2DC staging watch arm {}: slot {:08X} (param {} of gain plugin {:08X}, node "
              "{:08X}) holds {:08X} ({:.6g}); staging entry words {:08X} {:08X}",
              idx, slot, r30.u32, plugin, node, vb, value, W(slot - 4), W(slot));
  init_arm_index.store(idx);
  init_watch_guest.store(slot);
  RaiseException(kArmSelfCode, 0, 0, nullptr);
  std::thread(SetDr0AllThreads, slot, GetCurrentThreadId()).detach();
}


// 0x82E2B41C in sub_82E2A040, just before "bl sub_82E1F088": the game's voice gain calculation.
//   a   = sub_82E2CD28(r20) = *(r20 + 240)
//   vol = def[+46] ? voice[+104] (= sub_82E3DBC0(ctx r14, def + 40)) : *(def + 40)
//   f30 = def[+132] * vol * a                     -> gain parameter (paramRefs + 3)
//   f31 = (def[+54] ? def[+48] : 1) * eval(def+48) * voice[+56] -> second parameter (paramRefs + 9)
// r16 = sound definition, r31 = voice instance, r14 = evaluation context, r20 = group object.
void Ao2dcPingCapture(uint32_t r1, uint32_t ctxp, uint32_t def, uint32_t patch, uint32_t voice,
                      double f30, double f31);
void Ao2dcPingStage(uint32_t r1, uint32_t patch, uint32_t ref, uint32_t index, double value);

void Ao2dcGainCalc(PPCRegister& r1, PPCRegister& r14, PPCRegister& r16, PPCRegister& r20,
                   PPCRegister& r31, PPCRegister& f30, PPCRegister& f31) {
  Ao2dcPingCapture(r1.u32, r14.u32, r16.u32, r20.u32, r31.u32, f30.f64, f31.f64);
  if (!REXCVAR_GET(ao2dc_gain_calc_log)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  const float gain = float(f30.f64);
  if (!(gain >= 2.5f) && !(float(f31.f64) >= 2.5f)) return;
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  auto F = [&](uint32_t a) {
    const uint32_t b = W(a);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
  };
  auto ok = [](uint32_t a) {
    if (a < 0x40000000u || a >= 0xA0000000u) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(membase + a, &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
  };
  const uint32_t def = r16.u32, voice = r31.u32, ctx = r14.u32, grp = r20.u32;
  {
    static std::mutex m;
    static uint32_t seen_def[128], seen_gain[128];
    static int n = 0;
    uint32_t gb;
    const float g2 = float(f31.f64);
    std::memcpy(&gb, &g2, 4);
    std::lock_guard<std::mutex> lock(m);
    for (int i = 0; i < n; ++i)
      if (seen_def[i] == def && seen_gain[i] == gb) return;
    if (n >= 128) return;
    seen_def[n] = def;
    seen_gain[n] = gb;
    ++n;
  }
  std::string dw;
  if (ok(def) || (def >= 0x82000000u && def < 0x84800000u))
    for (int i = 0; i < 48; ++i) dw += fmt::format("{:08X} ", W(def + 4 * i));
  const uint8_t flag46 = ok(def) ? membase[def + 46] : 0;
  const uint8_t var40 = ok(def) ? membase[def + 40 + 6] : 0;
  const uint16_t idx40 = ok(def) ? rex::memory::load_and_swap<uint16_t>(membase + def + 44) : 0;
  float var_value = 0.0f;
  if (var40 && ok(ctx)) {
    const uint32_t vals = W(ctx + 4);
    if (ok(vals)) var_value = F(vals + 8 * idx40);
  }
  const uint8_t var48 = ok(def) ? membase[def + 48 + 6] : 0;
  const uint16_t idx48 = ok(def) ? rex::memory::load_and_swap<uint16_t>(membase + def + 52) : 0;
  float var48_value = 0.0f;
  if (var48 && ok(ctx)) {
    const uint32_t vals = W(ctx + 4);
    if (ok(vals)) var48_value = F(vals + 8 * idx48);
  }
  std::string vw;
  if (ok(voice))
    for (int i = 0; i < 32; ++i) vw += fmt::format("{:08X} ", W(voice + 4 * i));
  REXLOG_INFO("AO2DC gain calc voice {:08X} words: {} | ctx {:08X} values {:08X}", voice, vw, ctx,
              ok(ctx) ? W(ctx + 4) : 0);
  if (ok(voice)) {
    const uint32_t entry = W(voice + 48), bank = W(voice + 40);
    std::string ew, bw;
    if (ok(entry))
      for (int i = 0; i < 16; ++i) ew += fmt::format("{:08X} ", W(entry + 4 * i));
    if (ok(bank))
      for (int i = 0; i < 16; ++i) bw += fmt::format("{:08X} ", W(bank + 4 * i));
    REXLOG_INFO("AO2DC gain calc sample entry voice+48 {:08X}: {}| bank voice+40 {:08X}: {}", entry,
                ew, bank, bw);
  }
  REXLOG_INFO("AO2DC gain calc f31: {:.6g} [{:.2f} dB] = (def+54 {} ? def+48 {:.6g} : 1) * "
              "eval(def+48: variable {} idx {} value {:.6g}) * voice+56 {:.6g}",
              float(f31.f64), 20.0 * std::log10(std::fabs(double(f31.f64)) + 1e-30),
              ok(def) ? membase[def + 54] : 0, ok(def) ? F(def + 48) : 0.0f, var48, idx48,
              var48_value, ok(voice) ? F(voice + 56) : 0.0f);
  REXLOG_INFO(
      "AO2DC gain calc: gain f30 {:.6g} [{:.2f} dB] second f31 {:.6g} | def {:08X} def+132 {:.6g} "
      "def+40 {:.6g} (variable {} idx {} value {:.6g}) def+46 {} voice {:08X} voice+104 {:.6g} "
      "voice+56 {:.6g} group {:08X} group+240 {:.6g} | def words: {}",
      gain, 20.0 * std::log10(std::fabs(double(gain)) + 1e-30), float(f31.f64), def,
      ok(def) ? F(def + 132) : 0.0f, ok(def) ? F(def + 40) : 0.0f, var40, idx40, var_value,
      flag46, voice, ok(voice) ? F(voice + 104) : 0.0f, ok(voice) ? F(voice + 56) : 0.0f, grp,
      ok(grp) ? F(grp + 240) : 0.0f, dw);
  if (REXCVAR_GET(ao2dc_voice56_watch) && float(f31.f64) >= 2.5f && ok(voice)) {
    static std::atomic<bool> rep{false};
    if (!rep.exchange(true)) {
      if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
      std::thread(InitWatchReporter).detach();
    }
    static std::atomic<uint32_t> arms{0};
    static std::atomic<uint64_t> last_arm{0};
    const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    if (arms.load() < 30 && now - last_arm.load() >= 1500000000ull) {
      last_arm.store(now);
      const uint32_t idx = arms.fetch_add(1) + 1;
      REXLOG_INFO("AO2DC voice56 watch arm {}: voice {:08X} +56 = {:.6g} (gain {:.6g})", idx, voice,
                  F(voice + 56), float(f31.f64));
      init_arm_index.store(idx);
      init_watch_guest.store(voice + 56);
      RaiseException(kArmSelfCode, 0, 0, nullptr);
      std::thread(SetDr0AllThreads, voice + 56, GetCurrentThreadId()).detach();
    }
  }
}


// Entry of sub_82E2CFC0 (game parameter staging): r3 = patch instance, r4 = parameter reference
// (3 bytes: enabled, slot, plugin), r5 = parameter index, f1 = value.
void Ao2dcStageSet(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                   PPCRegister& f1) {
  Ao2dcPingStage(r1.u32, r3.u32, r4.u32, r5.u32, f1.f64);
  if (REXCVAR_GET(ao2dc_comp_param_log)) {
    if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
    auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
    auto okp = [](uint32_t a) { return a >= 0x40000000u && a < 0x50000000u; };
    if (okp(r3.u32) && okp(r4.u32)) {
      const uint8_t* rf = membase + r4.u32;
      const uint32_t table = W(r3.u32 + 232);
      if (okp(table)) {
        const uint32_t node = W(table + 24u * rf[1] + 4);
        if (okp(node)) {
          const uint32_t plugin = W(node + 80 + 4u * rf[2]);
          if (okp(plugin) && W(plugin + 20) == 0x8409400Cu)
            CompParamLog("stage set", plugin, r5.u32, float(f1.f64));
        }
      }
    }
  }
  if (!REXCVAR_GET(ao2dc_stage_set_log)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  const float value = float(f1.f64);
  if (!(std::fabs(value) >= 2.5f) || !(std::fabs(value) < 1e6f)) return;
  // Resolve the target plugin exactly as the flush (sub_82E2CC20) does.
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  auto ok = [](uint32_t a) { return a >= 0x40000000u && a < 0x50000000u; };
  if (!ok(r3.u32) || !ok(r4.u32)) return;
  const uint8_t* ref0 = membase + r4.u32;
  const uint32_t table = W(r3.u32 + 232);
  if (!ok(table)) return;
  const uint32_t entry = table + 24u * ref0[1];
  const uint32_t node = W(entry + 4);
  if (!ok(node)) return;
  const uint32_t plugin = W(node + 80 + 4u * ref0[2]);
  if (!ok(plugin) || W(plugin + 20) != 0x84093270u) return;  // gain plugins only
  const uint32_t gen0 = W(node + 80);
  const bool snd = ok(gen0) && W(gen0 + 20) == 0x8408C750u;
  void* frames[24];
  const USHORT nf = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
  static FnMap fmap;
  std::string stack;
  uint64_t key = 0;
  int shown = 0;
  for (USHORT i = 0; i < nf && shown < 10; ++i) {
    const std::string nm = fmap.Name(reinterpret_cast<uint64_t>(frames[i]));
    if (nm.rfind("sub_", 0) != 0) continue;
    if (shown == 1 || shown == 2) key = key * 1315423911ull + std::hash<std::string>{}(nm);
    stack += (shown ? " <- " : "") + nm;
    ++shown;
  }
  uint32_t vb;
  std::memcpy(&vb, &value, 4);
  {
    static std::mutex m;
    static uint64_t seen[512];
    static int n = 0;
    const uint64_t k = key ^ (uint64_t(vb) << 1);
    std::lock_guard<std::mutex> lock(m);
    for (int i = 0; i < n; ++i)
      if (seen[i] == k) return;
    if (n >= 512) return;
    seen[n++] = k;
  }
  const uint8_t* ref = membase + r4.u32;
  REXLOG_INFO("AO2DC stage set: GAIN plugin {:08X} node {:08X}{} <- {:08X} ({:.6g}) [{:.2f} dB] "
              "patch {:08X} ref [{:02X} {:02X} {:02X}] index {} thread {} : {}",
              plugin, node, snd ? " SndPlayer" : "", vb, value,
              20.0 * std::log10(std::fabs(double(value))), r3.u32, ref[0], ref[1], ref[2], r5.u32,
              GetCurrentThreadId(), stack);
}


// EA-XAS block decoder capture: entry of sub_8388FE88 (r4 = 76-byte block, r5 = 128 float output,
// 4 sub-blocks of 32 at stride 128 bytes) and its tail at 0x83890268 (after the loop).
struct XasRec {
  uint32_t in_addr, out_addr;
  uint8_t in[76];
  uint32_t out[128];
};
constexpr int kXasMax = 512;
XasRec* xas_recs = nullptr;
std::atomic<int> xas_n{0};
thread_local uint32_t xas_in = 0, xas_out = 0;

void XasWriter() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const int n = std::min(xas_n.load(), kXasMax);
    FILE* f = std::fopen("logs/xas_blocks.bin", "wb");
    if (f) {
      std::fwrite(xas_recs, sizeof(XasRec), size_t(n), f);
      std::fclose(f);
    }
    if (n >= kXasMax) {
      REXLOG_INFO("AO2DC xas capture: {} blocks written to logs/xas_blocks.bin", n);
      return;
    }
  }
}

void Ao2dcPingXas(uint32_t in, uint32_t out);
thread_local uint32_t ping_xas_in = 0, ping_xas_out = 0;

void Ao2dcXasEntry(PPCRegister& r4, PPCRegister& r5) {
  // (disabled: load-stall test) ping_xas_in = r4.u32;
  // (disabled: load-stall test) ping_xas_out = r5.u32;
  if (!REXCVAR_GET(ao2dc_xas_capture)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  static std::atomic<bool> started{false};
  if (!started.exchange(true)) {
    xas_recs = new XasRec[kXasMax]();
    auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
    std::string c;
    for (uint32_t a : {0x82B40B70u}) c += fmt::format("scale@{:08X}={:08X} ", a, W(a));
    c += "coef@82B3C144:";
    for (int i = 0; i < 8; ++i) c += fmt::format(" {:08X}", W(0x82B3C144u + 4 * i));
    c += " shift@82B3C164:";
    for (int i = 0; i < 16; ++i) c += fmt::format(" {:08X}", W(0x82B3C164u + 4 * i));
    REXLOG_INFO("AO2DC xas constants: {}", c);
    std::thread(XasWriter).detach();
  }
  xas_in = r4.u32;
  xas_out = r5.u32;
}

void Ao2dcXasExit() {
  // (disabled: load-stall test) if (ping_xas_in) Ao2dcPingXas(ping_xas_in, ping_xas_out);
  // (disabled: load-stall test) ping_xas_in = 0;
  if (!REXCVAR_GET(ao2dc_xas_capture) || !xas_recs || !xas_in) return;
  const int n = xas_n.fetch_add(1);
  if (n >= kXasMax) return;
  XasRec& r = xas_recs[n];
  r.in_addr = xas_in;
  r.out_addr = xas_out;
  std::memcpy(r.in, membase + xas_in, 76);
  for (int i = 0; i < 128; ++i)
    r.out[i] = rex::memory::load_and_swap<uint32_t>(membase + xas_out + 4 * i);
  xas_in = 0;
}


// 0x83ED88C4 in sub_83ED8858 (sample entry constructor), right after "stfs f0,20(r31)" (1.0).
void Ao2dcEntryGainWatch(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_entry_gain_watch)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  static std::atomic<bool> rep{false};
  if (!rep.exchange(true)) {
    if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
    std::thread(InitWatchReporter).detach();
  }
  static std::atomic<uint32_t> arms{0};
  const uint32_t idx = arms.fetch_add(1) + 1;
  if (idx == 1 || idx % 500 == 0)
    REXLOG_INFO("AO2DC entry gain watch: sample entry constructor call {} (entry {:08X})", idx,
                r31.u32);
  init_arm_index.store(idx);
  init_watch_guest.store(r31.u32 + 20);
  RaiseException(kArmSelfCode, 0, 0, nullptr);  // this thread only
}


// SndPlayer (sub_83876660) instrumentation. r31 = player, r28 = current segment.
//   player: +473 segment index, +477/+478 slot ring write/read, +112+16k slot k (state +117,
//           id +116), +460 current rate, +42 channels, +464 max samples, +424 data handle.
//   segment: +46 state (2/3 playing, 4 done), +47 channels, +16 rate, +20 data, +44, +28.
namespace {
struct PlayerState {
  uint32_t player;
  uint32_t starved_calls;
  uint64_t starve_start_ns;
};
PlayerState g_players[256];
std::mutex g_players_m;
std::atomic<int> g_sp_logs{0};

PlayerState* FindPlayer(uint32_t p) {
  PlayerState& e = g_players[(p >> 4) & 255];
  if (e.player != p) e = PlayerState{p, 0, 0};
  return &e;
}

std::string SegDesc(uint32_t player, uint32_t seg) {
  auto W = [](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); };
  auto F = [&](uint32_t a) {
    const uint32_t b = W(a);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
  };
  std::string slots;
  for (int k = 0; k < 20; ++k) slots += char('0' + std::min<int>(membase[player + 117 + 16 * k], 9));
  std::string sd = "seg none";
  if (seg >= 0x40000000u && seg < 0x70000000u)
    sd = fmt::format("seg {:08X} state {} ch {} rate {:.0f} data {:08X} +44 {} +28 {}", seg,
                     membase[seg + 46], membase[seg + 47], F(seg + 16), W(seg + 20),
                     rex::memory::load_and_swap<uint16_t>(membase + seg + 44), W(seg + 28));
  return fmt::format("player {:08X} segidx {} slots[{}] r{} w{} rate {:.0f} ch {} | {}", player,
                     membase[player + 473], slots, membase[player + 478], membase[player + 477],
                     F(player + 460), membase[player + 42], sd);
}
}  // namespace

void Ao2dcPingSndPlayer(uint32_t player, int exit_id);

// 0x83876CA4: SndPlayer returns 0 (nothing produced this call).
void Ao2dcSndStarve(PPCRegister& r28, PPCRegister& r31) {
  Ao2dcPingSndPlayer(r31.u32, 0);
  if (!REXCVAR_GET(ao2dc_sndplayer_log)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  std::lock_guard<std::mutex> lock(g_players_m);
  PlayerState* ps = FindPlayer(r31.u32);
  if (ps->starved_calls++ == 0) {
    ps->starve_start_ns = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    if (g_sp_logs.fetch_add(1) < 1500)
      REXLOG_INFO("AO2DC sndplayer STARVE begin: {}", SegDesc(r31.u32, r28.u32));
  }
}

// 0x83876CB0: next segment differs in rate/channels -> outputs nothing, switches format.
void Ao2dcSndFormat(PPCRegister& r28, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_sndplayer_log)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  if (g_sp_logs.fetch_add(1) < 1500)
    REXLOG_INFO("AO2DC sndplayer FORMAT switch: {}", SegDesc(r31.u32, r28.u32));
}

// 0x838768A8 / 0x83876CD4: SndPlayer returns 1 (produced data or delay).
void Ao2dcSndOk(PPCRegister& r31) {
  Ao2dcPingSndPlayer(r31.u32, 1);
  if (!REXCVAR_GET(ao2dc_sndplayer_log)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  std::lock_guard<std::mutex> lock(g_players_m);
  PlayerState* ps = FindPlayer(r31.u32);
  if (ps->starved_calls) {
    const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    if (g_sp_logs.fetch_add(1) < 1500)
      REXLOG_INFO("AO2DC sndplayer STARVE end: player {:08X} after {} empty calls ({:.1f} ms) | {}",
                  r31.u32, ps->starved_calls, double(now - ps->starve_start_ns) / 1e6,
                  SegDesc(r31.u32, 0));
    ps->starved_calls = 0;
  }
}

// Entry of sub_83887E78 (AudioCore gain plugin process): r3 = plugin (+52 target, +56 current,
// +42 channels), r5 = first-call flag.
uint32_t gain_watched = 0;
uint64_t gain_logs = 0;
void Ao2dcMixProbeGain(PPCRegister& r3, PPCRegister& r5) {
  if (!REXCVAR_GET(ao2dc_mix_probe_gain) && !REXCVAR_GET(ao2dc_gain_hw_watch)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  const float target = Bits32(rex::memory::load_and_swap<uint32_t>(membase + r3.u32 + 52));
  if (REXCVAR_GET(ao2dc_gain_hw_watch) && target >= float(REXCVAR_GET(ao2dc_gain_hw_watch_min))) {
    static std::mutex id_mutex;
    static uint32_t id_nodes[32];
    static int id_n = 0;
    const uint32_t node = rex::memory::load_and_swap<uint32_t>(membase + r3.u32 + 12);
    std::lock_guard<std::mutex> lock(id_mutex);
    bool seen = false;
    for (int i = 0; i < id_n; ++i) seen |= id_nodes[i] == node;
    if (!seen && id_n < 32) {
      id_nodes[id_n++] = node;
      DumpVoiceIdentity(r3.u32);
    }
  }
  if (REXCVAR_GET(ao2dc_gain_hw_watch)) {
    static std::atomic<bool> requested{false};
    const uint32_t wnode = rex::memory::load_and_swap<uint32_t>(membase + r3.u32 + 12);
    const uint32_t wgen0 = wnode ? rex::memory::load_and_swap<uint32_t>(membase + wnode + 80) : 0;
    const bool sndplayer_voice =
        wgen0 && rex::memory::load_and_swap<uint32_t>(membase + wgen0 + 20) == 0x8408C750;
    if (sndplayer_voice && target >= float(REXCVAR_GET(ao2dc_gain_hw_watch_min)) &&
        !requested.exchange(true)) {
      REXLOG_INFO("AO2DC gain hw watch: gain plugin {:08X} (node {:08X}) has target {:.6g}; arming",
                  r3.u32, rex::memory::load_and_swap<uint32_t>(membase + r3.u32 + 12), target);
      std::thread(ArmHwWatch, r3.u32 + 52).detach();
    }
    return;
  }
  if (!(target > 1.0f)) return;
  if (gain_logs < 40) {
    ++gain_logs;
    const float cur = Bits32(rex::memory::load_and_swap<uint32_t>(membase + r3.u32 + 56));
    REXLOG_INFO("AO2DC gain probe: plugin {:08X} target {:.6g} current {:.6g} first {} frame {}",
                r3.u32, target, cur, r5.u32 & 0xFF, frame_count);
  }
  if (gain_watched || armed.load()) return;
  if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
  gain_mode = true;
  gain_watched = r3.u32;
  target_guest = r3.u32 + 52;
  target_bytes = 4;
  target_host = membase + target_guest;
  page_lo = reinterpret_cast<uintptr_t>(target_host) & ~uintptr_t(0xFFF);
  page_hi = page_lo + 0x1000;
  armed_at = frame_count;
  REXLOG_INFO("AO2DC gain probe: guarding target gain word {:08X} of plugin {:08X} (value {:.6g})",
              target_guest, r3.u32, target);
  armed.store(true, std::memory_order_release);
  Guard(true);
}

// Midasm hook at 0x83CA7DC4 in sub_83CA7D40: the wait succeeded, r31 = render driver client.
void Ao2dcMixProbeSubmit(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_mix_probe) && !REXCVAR_GET(ao2dc_mix_probe_gain)) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  ++frame_count;
  out_guest = rex::memory::load_and_swap<uint32_t>(membase + r31.u32 + 60);
  if (frame_count == 2000 || frame_count % 3750 == 0) {
    std::lock_guard<std::mutex> lock(kc_mutex);
    REXLOG_INFO("AO2DC mix probe: master input {:08X} has {} feeder buffers so far", last_src,
                feeder_n);
    for (int f = 0; f < feeder_n; ++f) {
      REXLOG_INFO("AO2DC mix probe:   feeder src {:08X} bytes {} calls {} last frame {} peak {:.4g}",
                  feeders[f].src, feeders[f].bytes, feeders[f].calls, feeders[f].last_frame,
                  feeders[f].peak);
    }
    REXLOG_INFO("AO2DC mix probe: {} kernel calls into the submitted buffer by frame {}; layouts "
                "1:{} 2:{} 4:{} other:{}",
                kc_calls, frame_count, kc_mode_counts[1], kc_mode_counts[2], kc_mode_counts[4],
                kc_calls - kc_mode_counts[1] - kc_mode_counts[2] - kc_mode_counts[4]);
    for (uint32_t i = 0; i < std::min<uint32_t>(kc_last_n, 3); ++i) LogKernelCall("sample", kc_last[i]);
  }
  if (gain_mode) {
    if (armed.load(std::memory_order_acquire) && frame_count - armed_at >= 190) {
      armed.store(false, std::memory_order_release);
      Guard(false);
      std::vector<Hit> snap;
      while (hits_lock.test_and_set(std::memory_order_acquire)) {
      }
      snap.assign(hits, hits + n_hits);
      n_hits = 0;
      hits_lock.clear(std::memory_order_release);
      std::thread(Report, std::move(snap), uint64_t(0), 0, target_guest, target_bytes, false)
          .detach();
    }
    return;
  }
  if (armed.load(std::memory_order_acquire)) {
    if (frame_count - armed_at >= (target_guest == queued_guarded ? kArmFramesQueued : kArmFrames)) {
      armed.store(false, std::memory_order_release);
      Guard(false);
      std::vector<Hit> snap;
      uint64_t ovf;
      while (hits_lock.test_and_set(std::memory_order_acquire)) {
      }
      snap.assign(hits, hits + n_hits);
      ovf = overflow;
      n_hits = 0;
      overflow = 0;
      hits_lock.clear(std::memory_order_release);
      std::thread(Report, std::move(snap), ovf, rounds, target_guest, target_bytes,
                  target_guest == out_guest)
          .detach();
    }
    return;
  }
  const uint32_t origin = origin_src.exchange(0);
  if (!origin &&
      (rounds >= kMaxRounds || frame_count < uint64_t(rounds + 1) * kRoundIntervalFrames))
    return;
  uint32_t guest = out_guest, bytes = kOutBytes;
  if (origin) {
    guest = origin;
    bytes = origin_bytes.load();
    REXLOG_INFO("AO2DC mix probe: arming guard on rail ORIGIN buffer {:08X} ({} bytes)", guest,
                bytes);
  } else
  {
    std::lock_guard<std::mutex> lock(kc_mutex);
    if (rounds % 3 == 1 && last_src && last_src_bytes) {
      guest = last_src;
      bytes = last_src_bytes;
    } else if (rounds % 3 == 2 && hot_src && hot_bytes) {
      guest = hot_src;
      bytes = hot_bytes;
      hot_peak = 0.0f;  // pick the loudest feeder afresh for the next round
    }
  }
  if (!guest) return;
  if (!veh) veh = AddVectoredExceptionHandler(1, Handler);
  target_guest = guest;
  target_bytes = bytes;
  target_host = membase + guest;
  page_lo = reinterpret_cast<uintptr_t>(target_host) & ~uintptr_t(0xFFF);
  page_hi = (reinterpret_cast<uintptr_t>(target_host) + bytes + 0xFFF) & ~uintptr_t(0xFFF);
  ++rounds;
  armed_at = frame_count;
  armed.store(true, std::memory_order_release);
  Guard(true);
}
