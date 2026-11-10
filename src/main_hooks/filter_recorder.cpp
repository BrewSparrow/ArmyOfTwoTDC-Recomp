// filter_recorder.cpp - raw transaction recorder for the game's mixer biquad, sub_838A58C0.
//
// Verified from the PPC body (and the generated C++, ao2dc_recomp.307.cpp):
//   r3 = state (4 floats: x1 x2 y1 y2), r4 = output, r5 = input (may equal r4), r6 = coefficients
//   (5 floats: a1 a2 b0 b1 b2), r7 = sample count. y = b0 x + b1 x1 + b2 x2 + 1e-18 - a1 y1 - a2 y2.
//   A count that is a non-zero multiple of 8 runs the unrolled loop and writes the state back at
//   0x838A5A50; any other count calls the generic sub_838A5750 (whose only caller is this
//   function). Both paths meet at 0x838A5A68, before the epilogue.
// Hooks: entry at 0x838A58D4 (after the prologue: r3-r7 still hold the arguments, the caller's LR
// is at r1 + 184) and exit at 0x838A5A68. The entry copies the input block, entry state and
// coefficients; the exit copies the output block and exit state. Every invocation goes into a
// preallocated ring (~1.2 s at the observed call rate is logged); nothing on these paths
// allocates, writes files or formats text.
//
// A worker thread watches the triggers (premix full-scale/non-finite frames counted by the SDK,
// suspicious filter output, F9), waits 200 ms after a trigger, freezes the ring for the copy, and
// writes logs/filtercap/cap_NNN_<reason>_*.{json,filter.bin,premix.bin,post.bin}: the filter
// transactions of [trigger - 500 ms, trigger + 200 ms] and the raw premix / post-host frames of
// [trigger - 600 ms, trigger + 300 ms] (SDK sdl_audio_driver.cpp, ao2dc_audio_raw_rings).
// Replay: tools/filter_replay.py.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_filter_rec, false, "Audio",
                    "AO2DC diagnostics: record every mixer biquad call (sub_838A58C0) and save the "
                    "transactions around premix full-scale events, suspicious filter output and F9 "
                    "to logs/filtercap. Needs ao2dc_audio_raw_rings for the premix/output frames");
REXCVAR_DEFINE_DOUBLE(ao2dc_filter_rec_threshold, 2.0, "Audio",
                      "AO2DC diagnostics: filter output magnitude that triggers a capture");

namespace rex::audio {
struct Ao2dcHostFrameHeader {
  uint64_t seq, t_ns, src_seq;
  uint32_t flags, channels, samples, queue_depth;
  float peak;
  uint32_t nonfinite;
};
uint64_t Ao2dcSubmitSeq();
uint64_t Ao2dcPremixEventCount();
size_t Ao2dcCopyHostFrames(int which, uint64_t t0_ns, uint64_t t1_ns, std::vector<uint8_t>& out);
}  // namespace rex::audio

namespace {

constexpr uint32_t kMaxSamples = 256;
constexpr uint32_t kRingSize = 32768;

enum Flags : uint32_t {
  kInPlace = 1,
  kTruncated = 2,
  kGenericPath = 4,
  kStateNew = 8,
  kStateContinuous = 16,
  kStateReset = 32,
  kStateForeign = 64,
  kConcurrent = 128,
  kCoefChanged = 256,
  kNonFinite = 512,
  kIncomplete = 1024,
  kCoefWrittenSincePrev = 2048,
  kCoefWrittenDuring = 4096,
};

// On-disk layout (little-endian), parsed by tools/filter_replay.py.
struct Rec {
  uint64_t seq;               // global invocation sequence (1-based)
  uint64_t t_entry_ns;        // steady_clock
  uint64_t t_exit_ns;
  uint64_t submit_seq;        // SDK premix submit sequence at entry
  uint64_t state_prev_seq;    // previous invocation on this state pointer (0 = none)
  uint64_t coef_change_seq;   // invocation at which this coefficient pointer last changed values
  uint64_t coef_write_t_ns;   // last designer write to this coefficient pointer (0 = none seen)
  uint64_t coef_write_seq;    // global designer-write sequence of that write
  uint32_t thread_id;
  uint32_t caller;            // guest return address
  uint32_t state_ptr, out_ptr, in_ptr, coef_ptr;
  uint32_t count;
  uint32_t flags;
  uint32_t state_gen;         // incremented when the state is reset or written by someone else
  uint32_t state_prev_thread;
  uint32_t coef_write_designer;  // 1 A, 2 B, 3 C, 4 V1, 5 V2
  uint32_t coef_write_thread;
  uint32_t entry_state[4];    // raw float bits x1 x2 y1 y2
  uint32_t exit_state[4];
  uint32_t prev_exit_state[4];   // what the previous invocation on this state left there
  uint32_t coef[5];           // raw bits a1 a2 b0 b1 b2
  uint32_t prev_coef[5];      // coefficients of the previous invocation on this coefficient pointer
  float max_in, max_out;
  uint32_t in[kMaxSamples];
  uint32_t out[kMaxSamples];
};
static_assert(sizeof(Rec) == 2256, "keep tools/filter_replay.py in sync");

Rec* ring = nullptr;
std::atomic<uint64_t>* published = nullptr;  // seq once complete, 0 while written
std::atomic<uint64_t> next_seq{0};
std::atomic<bool> frozen{false};

struct Pending {
  Rec* rec = nullptr;
  uint32_t slot = 0;
};
thread_local Pending pending;

// Per-pointer tables (open addressing, guarded by table_lock; short critical sections).
constexpr uint32_t kTableSize = 16384;
struct StateEntry {
  uint32_t ptr;
  uint32_t gen;
  uint64_t last_seq;
  uint32_t last_thread;
  uint32_t inflight;
  uint32_t last_exit[4];
};
struct CoefEntry {
  uint32_t ptr;
  uint32_t last[5];
  bool have_last;
  bool written_since;
  uint64_t change_seq;
  uint64_t write_t_ns, write_seq;
  uint32_t write_designer, write_thread;
};
StateEntry* states = nullptr;
CoefEntry* coefs = nullptr;
std::mutex table_lock;
std::atomic<uint64_t> coef_write_seq{0};

// Counters for the 5 s summary.
std::atomic<uint64_t> n_calls{0}, n_new{0}, n_cont{0}, n_reset{0}, n_foreign{0}, n_concurrent{0},
    n_coef_changed{0}, n_nonfinite{0}, n_over{0};

// Triggers.
std::atomic<uint64_t> filter_trigger_t{0};
std::atomic<uint64_t> filter_trigger_seq{0};
std::atomic<uint32_t> filter_trigger_reason{0};  // 1 = over threshold, 2 = non-finite

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint32_t Hash(uint32_t p) {
  return (p * 2654435761u) >> 18;  // 14 bits
}

StateEntry* FindState(uint32_t ptr, bool& is_new) {
  uint32_t i = Hash(ptr) & (kTableSize - 1);
  for (uint32_t n = 0; n < kTableSize; ++n, i = (i + 1) & (kTableSize - 1)) {
    if (states[i].ptr == ptr) {
      is_new = false;
      return &states[i];
    }
    if (!states[i].ptr) {
      states[i] = {};
      states[i].ptr = ptr;
      is_new = true;
      return &states[i];
    }
  }
  return nullptr;
}

CoefEntry* FindCoef(uint32_t ptr) {
  uint32_t i = Hash(ptr) & (kTableSize - 1);
  for (uint32_t n = 0; n < kTableSize; ++n, i = (i + 1) & (kTableSize - 1)) {
    if (coefs[i].ptr == ptr) {
      return &coefs[i];
    }
    if (!coefs[i].ptr) {
      coefs[i] = {};
      coefs[i].ptr = ptr;
      return &coefs[i];
    }
  }
  return nullptr;
}

uint32_t LoadBits(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

float BitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void WorkerMain();

void EnsureStarted() {
  static std::once_flag once;
  std::call_once(once, [] {
    ring = new Rec[kRingSize];
    published = new std::atomic<uint64_t>[kRingSize];
    for (uint32_t i = 0; i < kRingSize; ++i) {
      published[i].store(0);
    }
    states = new StateEntry[kTableSize]();
    coefs = new CoefEntry[kTableSize]();
    std::thread(WorkerMain).detach();
    REXLOG_INFO("Filter recorder: on ({} records of {} bytes, threshold {:.2f})", kRingSize,
                sizeof(Rec), REXCVAR_GET(ao2dc_filter_rec_threshold));
  });
}

// ---- worker ----

std::string Hex(uint32_t v) {
  return fmt::format("{:08X}", v);
}

void WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return;
  }
  if (size) {
    std::fwrite(data, 1, size, f);
  }
  std::fclose(f);
}

void Capture(uint32_t number, const char* reason, uint64_t trigger_t, uint64_t trigger_seq,
             uint64_t start_t) {
  const uint64_t t0 = trigger_t - 500000000ull, t1 = trigger_t + 200000000ull;
  // Freeze, let in-flight invocations finish, copy, unfreeze.
  frozen.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  const uint64_t freeze_t = NowNs();
  std::vector<Rec> recs;
  uint64_t oldest_t = UINT64_MAX, newest_t = 0;
  for (uint32_t i = 0; i < kRingSize; ++i) {
    const uint64_t seq = published[i].load(std::memory_order_acquire);
    if (!seq) {
      continue;
    }
    const Rec& r = ring[i];
    oldest_t = std::min(oldest_t, r.t_exit_ns);
    newest_t = std::max(newest_t, r.t_exit_ns);
    if (r.t_exit_ns >= t0 && r.t_exit_ns <= t1) {
      recs.push_back(r);
    }
  }
  frozen.store(false);
  const uint64_t unfreeze_t = NowNs();
  std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.seq < b.seq; });
  std::vector<uint8_t> premix, post;
  const size_t n_premix = rex::audio::Ao2dcCopyHostFrames(0, t0 - 100000000ull,
                                                          t1 + 100000000ull, premix);
  const size_t n_post =
      rex::audio::Ao2dcCopyHostFrames(1, t0 - 100000000ull, t1 + 100000000ull, post);

  std::error_code ec;
  std::filesystem::create_directories("logs/filtercap", ec);
  SYSTEMTIME st;
  GetLocalTime(&st);
  const std::string base = fmt::format("logs/filtercap/cap_{:03d}_{}_{:02d}{:02d}{:02d}_{:03d}",
                                       number, reason, st.wHour, st.wMinute, st.wSecond,
                                       st.wMilliseconds);
  WriteFile(base + ".filter.bin", recs.data(), recs.size() * sizeof(Rec));
  WriteFile(base + ".premix.bin", premix.data(), premix.size());
  WriteFile(base + ".post.bin", post.data(), post.size());
  // Wall clock of the trigger: local time now minus (now - trigger) in steady time.
  const uint64_t now = NowNs();
  const double trigger_ago_ms = double(now - trigger_t) / 1e6;
  const std::string json = fmt::format(
      "{{\n  \"capture\": {},\n  \"reason\": \"{}\",\n  \"trigger_t_ns\": {},\n"
      "  \"trigger_filter_seq\": {},\n  \"window_ns\": [{}, {}],\n"
      "  \"local_time_written\": \"{:02d}:{:02d}:{:02d}.{:03d}\",\n"
      "  \"trigger_ms_before_written\": {:.3f},\n  \"recorder_start_t_ns\": {},\n"
      "  \"frozen_ns\": [{}, {}],\n  \"ring_oldest_t_ns\": {},\n  \"ring_newest_t_ns\": {},\n"
      "  \"filter_records\": {},\n  \"record_size\": {},\n  \"premix_frames\": {},\n"
      "  \"post_frames\": {},\n  \"threshold\": {},\n  \"submit_seq_now\": {},\n"
      "  \"tiny_bits\": {}\n}}\n",
      number, reason, trigger_t, trigger_seq, t0, t1, st.wHour, st.wMinute, st.wSecond,
      st.wMilliseconds, trigger_ago_ms, start_t, freeze_t, unfreeze_t,
      oldest_t == UINT64_MAX ? 0 : oldest_t, newest_t, recs.size(), sizeof(Rec), n_premix, n_post,
      REXCVAR_GET(ao2dc_filter_rec_threshold), rex::audio::Ao2dcSubmitSeq(),
      // The loop's added constant (lfs f9, 0x82B40DC4; "1e-18").
      LoadBits(rex::Runtime::instance()->memory()->TranslateVirtual(0), 0x82B40DC4));
  WriteFile(base + ".json", json.data(), json.size());
  REXLOG_INFO("Filter capture {} ({}): {} filter records, {} premix and {} output frames -> {}",
              number, reason, recs.size(), n_premix, n_post, base);
}

void WorkerMain() {
  const uint64_t start_t = NowNs();
  uint64_t last_premix_events = rex::audio::Ao2dcPremixEventCount();
  bool key_was_down = false;
  uint64_t pending_t = 0, pending_seq = 0;
  const char* pending_reason = nullptr;
  uint64_t last_capture_t = 0;
  uint32_t captures = 0, quota_filter = 0, quota_premix = 0, quota_manual = 0;
  uint64_t summary_t = start_t;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t now = NowNs();
    // Triggers (the first one wins until its capture is written).
    if (!pending_reason) {
      const bool key_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      const uint64_t premix_events = rex::audio::Ao2dcPremixEventCount();
      const uint64_t ft = filter_trigger_t.exchange(0);
      const bool cooled = now - last_capture_t > 1500000000ull;
      if (key_down && !key_was_down && quota_manual < 10) {
        pending_reason = "manual";
        pending_t = now;
        ++quota_manual;
      } else if (premix_events != last_premix_events && cooled && quota_premix < 20) {
        pending_reason = "premix";
        pending_t = now;
        ++quota_premix;
      } else if (ft && cooled && quota_filter < 20) {
        pending_reason = filter_trigger_reason.load() == 2 ? "nonfinite" : "filter";
        pending_t = ft;
        pending_seq = filter_trigger_seq.load();
        ++quota_filter;
      }
      last_premix_events = premix_events;
      key_was_down = key_down;
    }
    if (pending_reason && now >= pending_t + 200000000ull) {
      Capture(captures++, pending_reason, pending_t, pending_seq, start_t);
      last_capture_t = NowNs();
      pending_reason = nullptr;
      pending_seq = 0;
      filter_trigger_t.store(0);
    }
    if (now - summary_t >= 5000000000ull) {
      const double s = double(now - summary_t) / 1e9;
      summary_t = now;
      // Ring coverage: span of the published records.
      uint64_t oldest = UINT64_MAX, newest = 0;
      for (uint32_t i = 0; i < kRingSize; i += 64) {
        if (published[i].load(std::memory_order_relaxed)) {
          oldest = std::min(oldest, ring[i].t_exit_ns);
          newest = std::max(newest, ring[i].t_exit_ns);
        }
      }
      REXLOG_INFO(
          "Filter recorder: {:.0f} calls/s (ring covers {:.0f} ms); states new {} continuous {} "
          "reset {} foreign {}, concurrent {}, coefficient changes {}, non-finite {}, over "
          "threshold {}",
          double(n_calls.exchange(0)) / s,
          oldest == UINT64_MAX ? 0.0 : double(newest - oldest) / 1e6, n_new.exchange(0),
          n_cont.exchange(0), n_reset.exchange(0), n_foreign.exchange(0),
          n_concurrent.exchange(0), n_coef_changed.exchange(0), n_nonfinite.exchange(0),
          n_over.exchange(0));
    }
  }
}

}  // namespace

// Designer hooks (xas_check_hook.cpp) report coefficient writes here.
void Ao2dcFilterRecCoefWrite(uint32_t address, uint32_t designer) {
  if (!REXCVAR_GET(ao2dc_filter_rec)) {
    return;
  }
  EnsureStarted();
  const uint64_t seq = coef_write_seq.fetch_add(1) + 1;
  std::lock_guard<std::mutex> guard(table_lock);
  CoefEntry* c = FindCoef(address);
  if (c) {
    c->write_t_ns = NowNs();
    c->write_seq = seq;
    c->write_designer = designer;
    c->write_thread = GetCurrentThreadId();
    c->written_since = true;
  }
}

void Ao2dcFilterRecEntry(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                         PPCRegister& r6, PPCRegister& r7) {
  pending.rec = nullptr;
  if (!REXCVAR_GET(ao2dc_filter_rec)) {
    return;
  }
  EnsureStarted();
  if (frozen.load(std::memory_order_acquire)) {
    return;
  }
  const uint8_t* base = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  const uint64_t seq = next_seq.fetch_add(1) + 1;
  const uint32_t slot = uint32_t(seq % kRingSize);
  published[slot].store(0, std::memory_order_release);
  Rec& r = ring[slot];
  r.seq = seq;
  r.t_entry_ns = NowNs();
  r.t_exit_ns = 0;
  r.submit_seq = rex::audio::Ao2dcSubmitSeq();
  r.thread_id = GetCurrentThreadId();
  r.caller = LoadBits(base, r1.u32 + 184);
  r.state_ptr = r3.u32;
  r.out_ptr = r4.u32;
  r.in_ptr = r5.u32;
  r.coef_ptr = r6.u32;
  r.count = r7.u32;
  uint32_t flags = 0;
  if (r4.u32 == r5.u32) {
    flags |= kInPlace;
  }
  if (r.count > kMaxSamples) {
    flags |= kTruncated;
  }
  if (!r.count || (r.count & 7)) {
    flags |= kGenericPath;
  }
  const uint32_t n = std::min(r.count, kMaxSamples);
  float max_in = 0.0f;
  bool nonfinite = false;
  for (uint32_t i = 0; i < n; ++i) {
    r.in[i] = LoadBits(base, r.in_ptr + i * 4);
    const float v = BitsToFloat(r.in[i]);
    if (!std::isfinite(v)) {
      nonfinite = true;
    } else {
      max_in = std::max(max_in, std::fabs(v));
    }
  }
  for (uint32_t i = n; i < kMaxSamples; ++i) {
    r.in[i] = 0;
  }
  r.max_in = max_in;
  for (int i = 0; i < 4; ++i) {
    r.entry_state[i] = LoadBits(base, r.state_ptr + i * 4);
  }
  for (int i = 0; i < 5; ++i) {
    r.coef[i] = LoadBits(base, r.coef_ptr + i * 4);
  }
  {
    std::lock_guard<std::mutex> guard(table_lock);
    bool is_new = false;
    StateEntry* s = FindState(r.state_ptr, is_new);
    if (s) {
      if (is_new) {
        flags |= kStateNew;
        n_new.fetch_add(1, std::memory_order_relaxed);
        std::memset(r.prev_exit_state, 0, sizeof(r.prev_exit_state));
      } else {
        std::memcpy(r.prev_exit_state, s->last_exit, sizeof(r.prev_exit_state));
        const bool same = std::memcmp(s->last_exit, r.entry_state, sizeof(r.entry_state)) == 0;
        const bool zero = !(r.entry_state[0] | r.entry_state[1] | r.entry_state[2] |
                            r.entry_state[3]);
        if (same) {
          flags |= kStateContinuous;
          n_cont.fetch_add(1, std::memory_order_relaxed);
        } else if (zero) {
          flags |= kStateReset;
          n_reset.fetch_add(1, std::memory_order_relaxed);
          ++s->gen;
        } else {
          flags |= kStateForeign;
          n_foreign.fetch_add(1, std::memory_order_relaxed);
          ++s->gen;
        }
      }
      if (s->inflight) {
        flags |= kConcurrent;
        n_concurrent.fetch_add(1, std::memory_order_relaxed);
      }
      ++s->inflight;
      r.state_prev_seq = s->last_seq;
      r.state_prev_thread = s->last_thread;
      r.state_gen = s->gen;
      s->last_seq = seq;
      s->last_thread = r.thread_id;
    }
    CoefEntry* c = FindCoef(r.coef_ptr);
    if (c) {
      if (c->have_last) {
        std::memcpy(r.prev_coef, c->last, sizeof(r.prev_coef));
        if (std::memcmp(c->last, r.coef, sizeof(r.coef)) != 0) {
          flags |= kCoefChanged;
          c->change_seq = seq;
          n_coef_changed.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        std::memcpy(r.prev_coef, r.coef, sizeof(r.prev_coef));
      }
      if (c->written_since) {
        flags |= kCoefWrittenSincePrev;
        c->written_since = false;
      }
      std::memcpy(c->last, r.coef, sizeof(r.coef));
      c->have_last = true;
      r.coef_change_seq = c->change_seq;
      r.coef_write_t_ns = c->write_t_ns;
      r.coef_write_seq = c->write_seq;
      r.coef_write_designer = c->write_designer;
      r.coef_write_thread = c->write_thread;
    }
  }
  r.flags = flags | (nonfinite ? kNonFinite : 0) | kIncomplete;
  pending.rec = &r;
  pending.slot = slot;
}

void Ao2dcFilterRecExit() {
  Rec* rp = pending.rec;
  if (!rp) {
    return;
  }
  pending.rec = nullptr;
  Rec& r = *rp;
  const uint8_t* base = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  r.t_exit_ns = NowNs();
  const uint32_t n = std::min(r.count, kMaxSamples);
  float max_out = 0.0f;
  bool nonfinite = (r.flags & kNonFinite) != 0;
  for (uint32_t i = 0; i < n; ++i) {
    r.out[i] = LoadBits(base, r.out_ptr + i * 4);
    const float v = BitsToFloat(r.out[i]);
    if (!std::isfinite(v)) {
      nonfinite = true;
    } else {
      max_out = std::max(max_out, std::fabs(v));
    }
  }
  for (uint32_t i = n; i < kMaxSamples; ++i) {
    r.out[i] = 0;
  }
  for (int i = 0; i < 4; ++i) {
    r.exit_state[i] = LoadBits(base, r.state_ptr + i * 4);
    if (!std::isfinite(BitsToFloat(r.exit_state[i])) ||
        !std::isfinite(BitsToFloat(r.entry_state[i]))) {
      nonfinite = true;
    }
  }
  r.max_out = max_out;
  uint32_t flags = r.flags & ~uint32_t(kIncomplete);
  if (nonfinite) {
    flags |= kNonFinite;
  }
  {
    std::lock_guard<std::mutex> guard(table_lock);
    bool is_new = false;
    StateEntry* s = FindState(r.state_ptr, is_new);
    if (s) {
      std::memcpy(s->last_exit, r.exit_state, sizeof(s->last_exit));
      if (s->inflight) {
        --s->inflight;
      }
    }
    CoefEntry* c = FindCoef(r.coef_ptr);
    if (c && c->write_t_ns > r.t_entry_ns) {
      flags |= kCoefWrittenDuring;
    }
  }
  r.flags = flags;
  published[pending.slot].store(r.seq, std::memory_order_release);
  n_calls.fetch_add(1, std::memory_order_relaxed);
  const bool over = max_out >= float(REXCVAR_GET(ao2dc_filter_rec_threshold));
  if (nonfinite) {
    n_nonfinite.fetch_add(1, std::memory_order_relaxed);
  }
  if (over) {
    n_over.fetch_add(1, std::memory_order_relaxed);
  }
  if (nonfinite || over) {
    uint64_t expected = 0;
    if (filter_trigger_t.compare_exchange_strong(expected, r.t_exit_ns)) {
      filter_trigger_seq.store(r.seq);
      filter_trigger_reason.store(nonfinite ? 2 : 1);
    }
  }
}
