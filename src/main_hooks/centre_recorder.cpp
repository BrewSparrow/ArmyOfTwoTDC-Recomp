// AO2DC diagnostic: centre-path transaction recorder for the AudioCore graph.
//
// Path established by the mix writer probe (mix_writer_probe.cpp):
//   AudioCore graph job sub_838A6780 (one per mix job, on the job-system threads)
//     per node: sub_838A63E8 pulls the node input through generator plugins
//               (vtable slot +4, bctrl at 0x838A64A4), then the node's plugin chain runs
//               (vtable slot +8, bctrl at 0x838A68F4) on the context's three 8-channel x 256
//               ping-pong buffers (ctx+30860 current, +30864, +30868; descriptor +4 data,
//               +12 u16 channel count, +14 u16 channel stride in floats).
//     master node's output plugin sub_8388F2C8 -> sub_8388D980:
//               sub_83868250/sub_838A7050 route A (ctx+30860) -> B (ctx+30864),
//               sub_83867708 interleaves B into the client buffer queued on one XAudio2 source
//               voice, then sub_838BDD20(buf, 1536, f1, f2) at 0x8388DB1C clamps every sample:
//               if x < f1 then x = f1 else if x > f2 then x = f2.
//   XAudio2 copies that buffer through its voice/submix/master chain at gain 1.0 into the
//   render-driver frame the host receives (SDLAudioDriver::SubmitFrame).
//
// This recorder keeps, in preallocated rings and without allocation or I/O on the audio path:
//   * one record per generator and per plugin invocation on every node: node, plugin object,
//     guest function, index, return value, thread, per-channel peaks of the three ping-pong
//     buffers before and after, the raw centre (channel 2) block of the current and second
//     buffers after the call, and the first 128 bytes of the plugin object / 64 of the node;
//   * one record per output frame: the raw interleaved client frame before the clamp, the
//     clamp bounds (raw bits), and the clamped centre after it.
// Triggers: pre-clamp centre at or beyond the clamp bounds, or F9. A worker thread exports
// 500 ms before and 200 ms after the trigger to logs/centrecap/ with the premix/post-host rings.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_centre_rec, false, "AO2DC",
                    "Diagnostic: record every AudioCore generator/plugin invocation and the output "
                    "clamp, and export 700 ms around centre-channel rails (or F9) to "
                    "logs/centrecap. Restart required.");

REXCVAR_DEFINE_STRING(ao2dc_mute_voice_key, "", "AO2DC",
                      "Diagnostic only: 'gain:XXXXXXXX' (gain plugin target bits on a SndPlayer voice) or 'K:XXXXXXXX' (generator #0 word K) zeroes the output of every AudioCore voice "
                      "node whose generator plugin #0 has word K equal to XXXXXXXX, right after "
                      "its gain plugin (sub_83887E78) runs. Decoding, playback and lifecycle "
                      "continue; only the voice's contribution to its sends is removed. Empty = "
                      "off.");

REXCVAR_DEFINE_DOUBLE(ao2dc_preclip_trim, 1.0, "Audio",
                      "Opt-in test: scale the input of AudioCore's hard-clip bus (sub_83887878, "
                      "threshold 1.0) by this factor before it clips. 1.0 = the game's normal mix "
                      "(default). Lowers the whole mix by the same amount. Restart required.")
    .range(0.05, 1.0);
REXCVAR_DEFINE_BOOL(ao2dc_preclip_stats, false, "Audio",
                    "Diagnostic: log hard-clip bus statistics every 5 s (input peak, frames over "
                    "the threshold before and after ao2dc_preclip_trim, mean level).");

namespace rex::audio {
struct Ao2dcHostFrameHeader;
uint64_t Ao2dcSubmitSeq();
uint64_t Ao2dcHostEventCount();
size_t Ao2dcCopyHostFrames(int which, uint64_t t0_ns, uint64_t t1_ns, std::vector<uint8_t>& out);
}  // namespace rex::audio

namespace {

constexpr uint32_t kCh = 8;
constexpr uint32_t kN = 256;
constexpr uint32_t kProcRing = 40960;
constexpr uint32_t kOutRing = 512;

// kGen: negotiation (vtable +4, bctrl 0x838A64A4) in sub_838A63E8 -- writes no samples.
// kProc: node plugin process (vtable +8, bctrl 0x838A68F4) in sub_838A6780.
// kGenProc: generator process (vtable +8, bctrl 0x838A6540) in sub_838A63E8 -- decoder,
//           sample player, resampler: the stage that writes the voice's samples.
// kFallback: sub_838A6320 (call 0x838A6560) after a generator returned 0 -- silence padding.
// kNodeOut: end of sub_838A63E8 (0x838A6654) -- the assembled 256-sample node input block;
//           centre0 holds channel 0 and raw_before holds channel 1 for this kind.
enum Kind : uint32_t { kGen = 1, kProc = 2, kGenProc = 3, kFallback = 4, kNodeOut = 5 };

struct ProcRec {
  uint64_t seq;
  uint64_t t_ns;
  uint64_t submit_seq;
  uint64_t ac_frame;
  uint32_t thread;
  uint32_t kind;
  uint32_t ctx;
  uint32_t node;
  uint32_t plugin;
  uint32_t fn;
  uint32_t index;
  uint32_t ret;
  uint32_t buf[3];       // data pointers of ctx+30860/+30864/+30868 after the call
  uint16_t nch_before;
  uint16_t nch_after[3];
  uint16_t stride_after;
  uint16_t pad0;
  float peak_before[kCh];        // ctx+30860 before
  float peak_after[3][kCh];      // all three after
  uint32_t plugin_raw[32];
  uint32_t node_raw[16];
  uint32_t centre0[kN];          // ctx+30860 after (raw bits): channel 1 (centre) if the plugin
                                 // runs >2 channels, else channel 0
  uint32_t raw_before[kN];       // ctx+30860 before the call, same channel as centre0
  uint32_t data_before;          // ctx+30860 data pointer before the call
  uint32_t chan;                 // channel copied into centre0/raw_before
};
static_assert(sizeof(ProcRec) == 2464, "ProcRec layout changed; update tools/centre_replay.py");

struct OutRec {
  uint64_t seq;
  uint64_t t_ns;
  uint64_t submit_seq;
  uint64_t ac_frame;
  uint32_t thread;
  uint32_t buf;
  uint32_t lo_bits;
  uint32_t hi_bits;
  uint32_t count;
  uint32_t centre_at_or_beyond;  // pre-clamp centre samples with x <= lo or x >= hi
  uint32_t pre[kN * 6];          // raw interleaved client frame before the clamp
  uint32_t post_centre[kN];      // channel 2 after the clamp
  uint32_t ctx;
  uint32_t route_flag;           // byte at 0x8423F742 (extra routed channels when non-zero)
  uint32_t a_planar[kN * 6];     // ctx+30860 channels 0..5 (master bus before routing)
  uint32_t b_planar[kN * 6];     // ctx+30864 channels 0..5 (after sub_83868250 routing)
};
static_assert(sizeof(OutRec) == 19520, "OutRec layout changed; update tools/centre_replay.py");

ProcRec* proc_ring = nullptr;
std::atomic<uint64_t>* proc_pub = nullptr;
std::atomic<uint64_t> proc_next{0};
OutRec* out_ring = nullptr;
std::atomic<uint64_t>* out_pub = nullptr;
std::atomic<uint64_t> out_next{0};
std::atomic<uint64_t> ac_frame{0};
std::atomic<bool> frozen{false};
std::atomic<uint64_t> trigger_t{0};
std::atomic<uint64_t> trigger_frame{0};
std::atomic<uint64_t> request_t{0};  // Ao2dcCentreRequest: centre time of a requested capture
std::atomic<bool> started{false};
uint8_t* membase = nullptr;

thread_local float tl_peak_before[kCh];
thread_local uint16_t tl_nch_before = 0;
thread_local uint32_t tl_raw_before[256];
thread_local uint32_t tl_data_before = 0;
thread_local uint32_t tl_chan = 0;
thread_local uint32_t tl_out_slot = UINT32_MAX;
thread_local uint32_t tl_out_ctx = 0;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

inline uint32_t L32(uint32_t a) { return rex::memory::load_and_swap<uint32_t>(membase + a); }
inline uint16_t L16(uint32_t a) { return rex::memory::load_and_swap<uint16_t>(membase + a); }
inline float Bits(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

void WorkerMain();

void EnsureStarted() {
  bool expected = false;
  if (!started.compare_exchange_strong(expected, true)) return;
  membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  proc_ring = new ProcRec[kProcRing]();
  proc_pub = new std::atomic<uint64_t>[kProcRing]();
  out_ring = new OutRec[kOutRing]();
  out_pub = new std::atomic<uint64_t>[kOutRing]();
  REXLOG_INFO("Centre recorder: on ({} plugin records of {} bytes, {} output frames of {} bytes)",
              kProcRing, sizeof(ProcRec), kOutRing, sizeof(OutRec));
  // One-time dump of AudioCore plugin interface tables / descriptors (gain, SndPlayer,
  // resampler, send, bus input, hard clip, pan) for locating init and parameter-set functions.
  for (uint32_t base : {0x84093264u, 0x84093270u, 0x8408C744u, 0x8408C750u, 0x8408E714u,
                        0x8408E748u, 0x8408C4DCu, 0x8408C8A0u, 0x84092E00u, 0x840910ECu}) {
    std::string w;
    for (int i = 0; i < 24; ++i) w += fmt::format("{:08X} ", L32(base + 4 * i));
    REXLOG_INFO("AudioCore table {:08X}: {}", base, w);
  }
  std::thread(WorkerMain).detach();
}

// Per-channel peaks of an AudioCore buffer descriptor; returns the channel count.
uint16_t Peaks(uint32_t desc, float* out, uint32_t* data_out = nullptr,
               uint16_t* stride_out = nullptr) {
  for (uint32_t c = 0; c < kCh; ++c) out[c] = 0.0f;
  if (!desc) return 0;
  // The descriptor's +12 halfword is 0 (sub_83869AE8 clears it); the buffers are 8 channels by
  // capacity and the channel count in use is the plugin's byte +42. Scan all 8.
  const uint32_t data = L32(desc + 4);
  const uint16_t nch = kCh;
  const uint16_t stride = L16(desc + 14);
  if (data_out) *data_out = data;
  if (stride_out) *stride_out = stride;
  if (!data || stride < kN) return nch;
  for (uint32_t c = 0; c < nch; ++c) {
    float p = 0.0f;
    const uint32_t base = data + c * stride * 4;
    for (uint32_t i = 0; i < kN; ++i) {
      const float a = std::fabs(Bits(L32(base + 4 * i)));
      if (!(a <= p)) p = a;  // NaN propagates
    }
    out[c] = p;
  }
  return nch;
}

// AudioCore buffers are ordered L, C, R, Ls, Rs, LFE: the centre is channel 1 for multichannel
// plugins; mono plugins (byte +42 == 1) carry their signal in channel 0.
void CopyCentre(uint32_t desc, uint32_t* dst, uint32_t ch) {
  std::memset(dst, 0, kN * 4);
  if (!desc) return;
  const uint32_t data = L32(desc + 4);
  const uint16_t stride = L16(desc + 14);
  if (!data || stride < kN) return;
  const uint8_t* src = membase + data + ch * stride * 4;
  for (uint32_t i = 0; i < kN; ++i) {
    uint32_t v;
    std::memcpy(&v, src + 4 * i, 4);
    dst[i] = __builtin_bswap32(v);
  }
}

void Before(uint32_t ctx, uint32_t plugin) {
  if (!REXCVAR_GET(ao2dc_centre_rec)) return;
  EnsureStarted();
  const uint32_t desc = L32(ctx + 30860);
  tl_nch_before = Peaks(desc, tl_peak_before, &tl_data_before);
  const uint8_t nch = plugin ? membase[plugin + 42] : 0;
  tl_chan = nch > 2 ? 1 : 0;
  CopyCentre(desc, tl_raw_before, tl_chan);
}

void After(Kind kind, uint32_t ret, uint32_t ctx, uint32_t node, uint32_t plugin, uint32_t fn,
           uint32_t index) {
  if (!REXCVAR_GET(ao2dc_centre_rec) || !proc_ring || frozen.load(std::memory_order_acquire))
    return;
  const uint64_t seq = proc_next.fetch_add(1) + 1;
  const uint32_t slot = uint32_t(seq % kProcRing);
  proc_pub[slot].store(0, std::memory_order_release);
  ProcRec& r = proc_ring[slot];
  r.seq = seq;
  r.t_ns = NowNs();
  r.submit_seq = rex::audio::Ao2dcSubmitSeq();
  r.ac_frame = ac_frame.load(std::memory_order_relaxed);
  r.thread = GetCurrentThreadId();
  r.kind = kind;
  r.ctx = ctx;
  r.node = node;
  r.plugin = plugin;
  r.fn = fn;
  r.index = index;
  r.ret = ret;
  r.nch_before = tl_nch_before;
  std::memcpy(r.peak_before, tl_peak_before, sizeof(r.peak_before));
  for (int b = 0; b < 3; ++b) {
    const uint32_t desc = L32(ctx + 30860 + 4 * b);
    uint16_t stride = 0;
    r.buf[b] = 0;
    r.nch_after[b] = Peaks(desc, r.peak_after[b], &r.buf[b], b == 0 ? &stride : nullptr);
    if (b == 0) r.stride_after = stride;
  }
  for (int i = 0; i < 32; ++i) r.plugin_raw[i] = plugin ? L32(plugin + 4 * i) : 0;
  for (int i = 0; i < 16; ++i) r.node_raw[i] = node ? L32(node + 4 * i) : 0;
  if (kind == kNodeOut) {
    CopyCentre(L32(ctx + 30860), r.centre0, 0);
    CopyCentre(L32(ctx + 30860), r.raw_before, 1);
    r.data_before = 0;
    r.chan = 0x10;  // marker: centre0 = ch0, raw_before = ch1
  } else {
    CopyCentre(L32(ctx + 30860), r.centre0, tl_chan);
    std::memcpy(r.raw_before, tl_raw_before, sizeof(r.raw_before));
    r.data_before = tl_data_before;
    r.chan = tl_chan;
  }
  proc_pub[slot].store(seq, std::memory_order_release);
}

void WriteFile(const std::string& path, const void* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  if (size) std::fwrite(data, 1, size, f);
  std::fclose(f);
}

void Capture(uint32_t number, const char* reason, uint64_t t_trig, uint64_t frame_trig) {
  const uint64_t t0 = t_trig - 500000000ull, t1 = t_trig + 200000000ull;
  frozen.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  std::vector<ProcRec> procs;
  std::vector<OutRec> outs;
  uint64_t proc_oldest = UINT64_MAX;
  for (uint32_t i = 0; i < kProcRing; ++i) {
    if (!proc_pub[i].load(std::memory_order_acquire)) continue;
    proc_oldest = std::min(proc_oldest, proc_ring[i].t_ns);
    if (proc_ring[i].t_ns >= t0 && proc_ring[i].t_ns <= t1) procs.push_back(proc_ring[i]);
  }
  for (uint32_t i = 0; i < kOutRing; ++i) {
    if (!out_pub[i].load(std::memory_order_acquire)) continue;
    if (out_ring[i].t_ns >= t0 && out_ring[i].t_ns <= t1) outs.push_back(out_ring[i]);
  }
  frozen.store(false);
  std::sort(procs.begin(), procs.end(), [](auto& a, auto& b) { return a.seq < b.seq; });
  std::sort(outs.begin(), outs.end(), [](auto& a, auto& b) { return a.seq < b.seq; });
  std::vector<uint8_t> premix, post;
  const size_t n_premix =
      rex::audio::Ao2dcCopyHostFrames(0, t0 - 100000000ull, t1 + 100000000ull, premix);
  const size_t n_post =
      rex::audio::Ao2dcCopyHostFrames(1, t0 - 100000000ull, t1 + 100000000ull, post);
  std::error_code ec;
  std::filesystem::create_directories("logs/centrecap", ec);
  SYSTEMTIME st;
  GetLocalTime(&st);
  const std::string base =
      fmt::format("logs/centrecap/cap_{:03d}_{}_{:02d}{:02d}{:02d}_{:03d}", number, reason,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  WriteFile(base + ".proc.bin", procs.data(), procs.size() * sizeof(ProcRec));
  WriteFile(base + ".out.bin", outs.data(), outs.size() * sizeof(OutRec));
  WriteFile(base + ".premix.bin", premix.data(), premix.size());
  WriteFile(base + ".post.bin", post.data(), post.size());
  const std::string json = fmt::format(
      "{{\n  \"capture\": {},\n  \"reason\": \"{}\",\n  \"trigger_t_ns\": {},\n"
      "  \"trigger_ac_frame\": {},\n  \"window_ns\": [{}, {}],\n"
      "  \"local_time_written\": \"{:02d}:{:02d}:{:02d}.{:03d}\",\n"
      "  \"proc_records\": {},\n  \"proc_record_size\": {},\n  \"proc_ring_oldest_t_ns\": {},\n"
      "  \"out_records\": {},\n  \"out_record_size\": {},\n  \"premix_frames\": {},\n"
      "  \"post_frames\": {},\n  \"submit_seq_now\": {}\n}}\n",
      number, reason, t_trig, frame_trig, t0, t1, st.wHour, st.wMinute, st.wSecond,
      st.wMilliseconds, procs.size(), sizeof(ProcRec),
      proc_oldest == UINT64_MAX ? 0 : proc_oldest, outs.size(), sizeof(OutRec), n_premix, n_post,
      rex::audio::Ao2dcSubmitSeq());
  WriteFile(base + ".json", json.data(), json.size());
  REXLOG_INFO("Centre capture {} ({}): {} plugin records, {} output frames, {} premix, {} post -> {}",
              number, reason, procs.size(), outs.size(), n_premix, n_post, base);
}

void WorkerMain() {
  bool key_was_down = false;
  const char* pending = nullptr;
  uint64_t pending_t = 0, pending_frame = 0, last_capture_t = 0;
  uint32_t captures = 0, quota_rail = 0, quota_manual = 0, quota_jump = 0, quota_limit = 0;
  uint32_t quota_zoom = 0;
  uint64_t last_host = rex::audio::Ao2dcHostEventCount();
  uint64_t summary_t = NowNs(), last_proc = 0, last_out = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t now = NowNs();
    if (!pending) {
      const bool key_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      const uint64_t tt = trigger_t.exchange(0);
      const bool cooled = now - last_capture_t > 1500000000ull;
      if (key_down && !key_was_down && quota_manual < 10) {
        pending = "manual";
        pending_t = now;
        pending_frame = ac_frame.load();
        ++quota_manual;
      } else if (const uint64_t rt = request_t.exchange(0); rt && cooled && quota_zoom < 14) {
        // Requested by the ping capture at a zoom foley request: the window (-500..+200 ms
        // around this time) covers the play start whether or not it clips.
        pending = "zoom";
        pending_t = rt;
        pending_frame = ac_frame.load();
        ++quota_zoom;
      } else if (tt && cooled && quota_rail < 12) {
        pending = "rail";
        pending_t = tt;
        pending_frame = trigger_frame.load();
        ++quota_rail;
      }
      // Host-side signals (sdl_audio_driver): an output sample jump >= 0.5, or the limiter
      // starting to turn the mix down. Saved even when no centre sample reaches +-1.0.
      const uint64_t host = rex::audio::Ao2dcHostEventCount();
      if (!pending && cooled && host != last_host) {
        const bool jump = (host & 0xFFFFFFFFull) != (last_host & 0xFFFFFFFFull);
        const bool limit = (host >> 32) != (last_host >> 32);
        if (jump && quota_jump < 12) {
          pending = "hostjump";
          ++quota_jump;
        } else if (limit && quota_limit < 8) {
          pending = "limiter";
          ++quota_limit;
        }
        if (pending) {
          pending_t = now - 10000000ull;  // the event happened within the last poll period
          pending_frame = ac_frame.load();
        }
      }
      last_host = host;
      key_was_down = key_down;
    }
    if (pending && now >= pending_t + 200000000ull) {
      Capture(captures++, pending, pending_t, pending_frame);
      last_capture_t = NowNs();
      pending = nullptr;
      trigger_t.store(0);
    }
    if (now - summary_t >= 10000000000ull) {
      const double s = double(now - summary_t) / 1e9;
      const uint64_t p = proc_next.load(), o = out_next.load();
      REXLOG_INFO("Centre recorder: {:.0f} plugin records/s, {:.1f} output frames/s (proc ring "
                  "covers {:.0f} ms)",
                  double(p - last_proc) / s, double(o - last_out) / s,
                  p == last_proc ? 0.0 : 1000.0 * s * kProcRing / double(p - last_proc));
      last_proc = p;
      last_out = o;
      summary_t = now;
    }
  }
}

}  // namespace

uint64_t Ao2dcAcFrame() { return ac_frame.load(std::memory_order_relaxed); }

// Ask for a capture centred delay_ns from now (used by ping_capture.cpp for zoom play starts).
void Ao2dcCentreRequest(uint64_t delay_ns) {
  if (!REXCVAR_GET(ao2dc_centre_rec)) return;
  request_t.store(NowNs() + delay_ns, std::memory_order_relaxed);
}

// 0x838A64A4 (negotiation bctrl in sub_838A63E8): r3 = plugin, r4 = ctx.
void Ao2dcCentreGenBefore(PPCRegister& r3, PPCRegister& r4) { Before(r4.u32, r3.u32); }

// 0x838A6540 (generator process bctrl in sub_838A63E8): r3 = plugin, r4 = ctx.
void Ao2dcCentreGenProcBefore(PPCRegister& r3, PPCRegister& r4) { Before(r4.u32, r3.u32); }

// 0x838A6544: r3 = result, r19 = vtable entries, r25 = node, r26 = ctx, r30 = plugin,
// r31 = plugin index.
void Ao2dcCentreGenProcAfter(PPCRegister& r3, PPCRegister& r19, PPCRegister& r25,
                             PPCRegister& r26, PPCRegister& r30, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_centre_rec) || !membase) return;
  const uint32_t entry = L32(r19.u32 + 8 * r31.u32);
  After(kGenProc, r3.u32, r26.u32, r25.u32, r30.u32, entry ? L32(entry + 8) : 0, r31.u32);
}

// 0x838A6560 (call sub_838A6320): r3 = ctx, r4 = node, r5 = plugin, r6 = count.
void Ao2dcCentreFallbackBefore(PPCRegister& r3, PPCRegister& r5) { Before(r3.u32, r5.u32); }

// 0x838A6564: r3 = result, r25 = node, r26 = ctx, r30 = plugin, r31 = plugin index.
void Ao2dcCentreFallbackAfter(PPCRegister& r3, PPCRegister& r25, PPCRegister& r26,
                              PPCRegister& r30, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_centre_rec) || !membase) return;
  After(kFallback, r3.u32, r26.u32, r25.u32, r30.u32, 0x838A6320, r31.u32);
}

// 0x838A6654 (end of sub_838A63E8, after the assembled block was swapped into ctx+30860):
// r21 = result, r25 = node, r26 = ctx.
void Ao2dcCentreNodeOut(PPCRegister& r21, PPCRegister& r25, PPCRegister& r26) {
  if (!REXCVAR_GET(ao2dc_centre_rec) || !membase) return;
  EnsureStarted();
  tl_nch_before = 0;
  std::memset(tl_peak_before, 0, sizeof(tl_peak_before));
  After(kNodeOut, r21.u32, r26.u32, r25.u32, 0, 0x838A63E8, 0);
}

// 0x838A64A8: r3 = samples generated, r25 = node, r26 = ctx, r27 = &vtable entry, r30 = plugin,
// r31 = plugin index.
void Ao2dcCentreGenAfter(PPCRegister& r3, PPCRegister& r25, PPCRegister& r26, PPCRegister& r27,
                         PPCRegister& r30, PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_centre_rec) || !membase) return;
  const uint32_t entry = L32(r27.u32);
  After(kGen, r3.u32, r26.u32, r25.u32, r30.u32, entry ? L32(entry + 4) : 0, r31.u32);
}

// 0x838A68F4 (process bctrl in sub_838A6780): r3 = plugin, r4 = ctx.
void Ao2dcCentreProcBefore(PPCRegister& r3, PPCRegister& r4) { Before(r4.u32, r3.u32); }

// 0x838A68F8: r3 = result, r25 = vtable entries, r28 = ctx, r29 = plugin, r30 = index,
// r31 = node.
void MaybeMuteVoice(uint32_t ctx, uint32_t node, uint32_t plugin, uint32_t fn) {
  static int key_word = -2;
  static uint32_t key_value = 0;
  static std::atomic<uint64_t> muted_blocks{0};
  if (key_word == -2) {
    const std::string k = REXCVAR_GET(ao2dc_mute_voice_key);
    unsigned w = 0, v = 0;
    if (!k.empty() && std::sscanf(k.c_str(), "gain:%x", &v) == 1) {
      key_word = 1000;  // match the gain plugin's own +52 target bits on a SndPlayer voice
      key_value = v;
      REXLOG_INFO("AO2DC voice mute: ON for SndPlayer voices whose gain plugin target == {:08X} "
                  "(diagnostic)", v);
    } else if (!k.empty() && std::sscanf(k.c_str(), "%u:%x", &w, &v) == 2 && w < 64) {
      key_word = int(w);
      key_value = v;
      REXLOG_INFO("AO2DC voice mute: ON for generator #0 word {} == {:08X} (diagnostic)", w, v);
    } else {
      key_word = -1;
    }
  }
  if (key_word < 0 || fn != 0x83887E78) return;
  const uint32_t gen0 = L32(node + 80);
  if (!gen0) return;
  if (key_word == 1000) {
    // gain plugin target bits, and generator #0 is a SndPlayer (descriptor 0x8408C750)
    if (L32(plugin + 52) != key_value || L32(gen0 + 20) != 0x8408C750) return;
  } else if (L32(gen0 + 4 * key_word) != key_value) {
    return;
  }
  const uint32_t desc = L32(ctx + 30860);
  const uint32_t data = L32(desc + 4);
  const uint16_t stride = L16(desc + 14);
  const uint32_t nch = std::min<uint32_t>(membase[plugin + 42], kCh);
  for (uint32_t c = 0; c < nch; ++c) std::memset(membase + data + 4 * c * stride, 0, kN * 4);
  const uint64_t n = muted_blocks.fetch_add(1) + 1;
  if (n == 1 || n % 2000 == 0)
    REXLOG_INFO("AO2DC voice mute: {} blocks muted (node {:08X}, gain plugin {:08X})", n, node,
                plugin);
}

void Ao2dcCentreProcAfter(PPCRegister& r3, PPCRegister& r25, PPCRegister& r28, PPCRegister& r29,
                          PPCRegister& r30, PPCRegister& r31) {
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  {
    const uint32_t e = L32(r25.u32 + 8 * r30.u32);
    MaybeMuteVoice(r28.u32, r31.u32, r29.u32, e ? L32(e + 8) : 0);
  }
  if (!REXCVAR_GET(ao2dc_centre_rec) || !membase) return;
  const uint32_t entry = L32(r25.u32 + 8 * r30.u32);
  After(kProc, r3.u32, r28.u32, r31.u32, r29.u32, entry ? L32(entry + 8) : 0, r30.u32);
}


// Entry of sub_83887878 (AudioCore hard clip): r3 = plugin (+52 threshold, +42 channels),
// r4 = ctx (ctx+30860 = the bus buffer the clip reads). Optional pre-clip trim + statistics.
void Ao2dcPreClip(PPCRegister& r3, PPCRegister& r4) {
  const double trim = REXCVAR_GET(ao2dc_preclip_trim);
  const bool stats = REXCVAR_GET(ao2dc_preclip_stats);
  if (trim >= 1.0 && !stats) return;
  if (!membase) membase = rex::Runtime::instance()->memory()->TranslateVirtual(0);
  const float threshold = Bits(L32(r3.u32 + 52));
  if (threshold != 1.0f) return;  // only the game's 1.0 hard clip
  const uint32_t desc = L32(r4.u32 + 30860);
  const uint32_t data = desc ? L32(desc + 4) : 0;
  const uint16_t stride = desc ? L16(desc + 14) : 0;
  const uint32_t nch = std::min<uint32_t>(membase[r3.u32 + 42], kCh);
  if (!data || stride < kN || !nch) return;
  const float t = float(trim);
  float peak = 0.0f;
  double energy = 0.0;
  for (uint32_t c = 0; c < nch; ++c) {
    uint8_t* base = membase + data + 4 * c * stride;
    for (uint32_t i = 0; i < kN; ++i) {
      uint32_t b;
      std::memcpy(&b, base + 4 * i, 4);
      b = __builtin_bswap32(b);
      float v;
      std::memcpy(&v, &b, 4);
      const float a = std::fabs(v);
      if (a > peak) peak = a;
      energy += double(v) * double(v);
      if (t < 1.0f) {
        v *= t;
        std::memcpy(&b, &v, 4);
        b = __builtin_bswap32(b);
        std::memcpy(base + 4 * i, &b, 4);
      }
    }
  }
  if (!stats) return;
  static std::mutex m;
  static uint64_t frames = 0, over = 0, over_after = 0, t0 = 0;
  static float maxpk = 0.0f;
  static double esum = 0.0;
  static uint64_t esamples = 0;
  std::lock_guard<std::mutex> lock(m);
  const uint64_t now = NowNs();
  if (!t0) t0 = now;
  ++frames;
  if (peak > 1.0f) ++over;
  if (peak * t > 1.0f) ++over_after;
  maxpk = std::max(maxpk, peak);
  esum += energy;
  esamples += uint64_t(nch) * kN;
  if (now - t0 >= 5000000000ull) {
    const double rms = std::sqrt(esum / double(std::max<uint64_t>(esamples, 1)));
    REXLOG_INFO("AO2DC pre-clip: trim {:.3f} | {} frames, {} over 1.0 before trim, {} still over "
                "after trim | input peak {:.3f}, mean input level {:.1f} dBFS (after trim {:.1f})",
                t, frames, over, over_after, maxpk, 20.0 * std::log10(rms + 1e-12),
                20.0 * std::log10(rms * t + 1e-12));
    frames = over = over_after = 0;
    maxpk = 0.0f;
    esum = 0.0;
    esamples = 0;
    t0 = now;
  }
}

// 0x8388DB1C in sub_8388D980, before the clamp call: r3 = interleaved client frame,
// r6 = count (1536), f1 = lower bound, f2 = upper bound.
void Ao2dcCentreClampBefore(PPCRegister& r3, PPCRegister& r6, PPCRegister& f1, PPCRegister& f2) {
  tl_out_slot = UINT32_MAX;
  if (!REXCVAR_GET(ao2dc_centre_rec)) return;
  EnsureStarted();
  const uint64_t frame = ac_frame.fetch_add(1) + 1;
  if (frozen.load(std::memory_order_acquire)) return;
  const uint64_t seq = out_next.fetch_add(1) + 1;
  const uint32_t slot = uint32_t(seq % kOutRing);
  out_pub[slot].store(0, std::memory_order_release);
  OutRec& o = out_ring[slot];
  o.seq = seq;
  o.t_ns = NowNs();
  o.submit_seq = rex::audio::Ao2dcSubmitSeq();
  o.ac_frame = frame;
  o.thread = GetCurrentThreadId();
  o.buf = r3.u32;
  const float lo = float(f1.f64), hi = float(f2.f64);
  std::memcpy(&o.lo_bits, &lo, 4);
  std::memcpy(&o.hi_bits, &hi, 4);
  o.count = r6.u32;
  const uint32_t n = std::min<uint32_t>(r6.u32, kN * 6);
  uint32_t rails = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t bits = L32(r3.u32 + 4 * i);
    o.pre[i] = bits;
    if (i % 6 == 2) {
      const float v = Bits(bits);
      if (!(v > lo && v < hi)) ++rails;
    }
  }
  o.centre_at_or_beyond = rails;
  o.ctx = tl_out_ctx;
  o.route_flag = membase[0x8423F742];
  for (int b = 0; b < 2; ++b) {
    uint32_t* dstp = b == 0 ? o.a_planar : o.b_planar;
    const uint32_t desc = tl_out_ctx ? L32(tl_out_ctx + 30860 + 4 * b) : 0;
    const uint32_t data = desc ? L32(desc + 4) : 0;
    const uint16_t stride = desc ? L16(desc + 14) : 0;
    for (uint32_t c = 0; c < 6; ++c)
      for (uint32_t i = 0; i < kN; ++i)
        dstp[c * kN + i] = (data && stride >= kN) ? L32(data + 4 * (c * stride + i)) : 0;
  }
  tl_out_slot = slot;
  if (rails && !trigger_t.load()) {
    trigger_frame.store(frame);
    trigger_t.store(o.t_ns);
  }
}

// 0x8388D990 in sub_8388D980 (AudioCore output plugin), after the prologue: r4 = ctx.
void Ao2dcCentreOutputEntry(PPCRegister& r4) { tl_out_ctx = r4.u32; }

// 0x8388DB20, after the clamp: r31 = the same client frame.
void Ao2dcCentreClampAfter(PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_centre_rec) || tl_out_slot == UINT32_MAX) return;
  OutRec& o = out_ring[tl_out_slot];
  for (uint32_t i = 0; i < kN; ++i) o.post_centre[i] = L32(r31.u32 + 4 * (6 * i + 2));
  out_pub[tl_out_slot].store(o.seq, std::memory_order_release);
  tl_out_slot = UINT32_MAX;
}
