// xas_check_hook.cpp - diagnostics for the game's EA-XAS v1 decoder, sub_8388FE88.
//
// sub_8388FE88(r4 = 0x4C-byte input block, r5 = 128 float output samples) decodes one channel
// block: four subframes, each with a 4-byte header (two start samples, a coefficient index into
// the 4 pairs at 0x82B3C144 and a shift) and 30 nibbles. Valid data only uses coefficient
// indexes 0-3, so a block whose headers name 4-15 is not XAS data (stale or wrong buffer).
//
// With ao2dc_xas_check on, the entry hook (0x8388FE9C) checks the headers and the exit hook
// (0x83890268) the decoded peak at the output pointer saved on entry (r5 is reused in the loop); a block with a bad header or a peak above 1.2 is logged with
// its bytes. Every 5 seconds a summary line gives the block counts.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_xas_check, false, "Audio",
                    "AO2DC diagnostic: check every EA-XAS block the game decodes and log bad ones");

namespace rex::audio {
uint32_t Ao2dcFullScaleCount();  // rexruntime (sdl_audio_driver.cpp)
}

namespace {
// Recent XAS blocks (time, input pointer, peak), to see at a full-scale moment which XAS sounds
// were playing and how loud they left the decoder.
struct XasRecent {
  int64_t ms;
  uint32_t input;
  float peak;
};
std::mutex g_recent_lock;
std::vector<XasRecent> g_recent(8192);
size_t g_recent_next = 0;
uint32_t g_seen_full_scale = 0;
thread_local uint32_t t_input = 0;
thread_local uint32_t t_output = 0;
thread_local uint32_t t_bad_headers = 0;
std::atomic<uint32_t> g_blocks{0}, g_bad_header_blocks{0}, g_loud_blocks{0}, g_logged{0};
std::atomic<int64_t> g_last_report_ms{0};
// Activity of the two Layer III decoders: antialias calls (one per granule and channel) of
// decoder A (sub_838B9418) and B (sub_838DB708), and samples through B's output stage.
std::atomic<uint32_t> g_l3a_granules{0}, g_l3b_granules{0}, g_l3b_samples{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string HexBytes(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
    if ((i & 3) == 3)
      s += ' ';
  }
  return s;
}
}  // namespace

void Ao2dcXasBlockStart(PPCRegister& r4, PPCRegister& r5) {
  if (!REXCVAR_GET(ao2dc_xas_check))
    return;
  t_input = r4.u32;
  t_output = r5.u32;
  const uint8_t* in = rex::Runtime::instance()->memory()->TranslateVirtual(r4.u32);
  uint32_t bad = 0;
  for (int sub = 0; sub < 4; ++sub) {
    if ((in[sub * 4] & 0x0F) > 3)
      bad |= 1u << sub;
  }
  t_bad_headers = bad;
}

void Ao2dcXasBlockEnd() {
  if (!REXCVAR_GET(ao2dc_xas_check) || !t_input)
    return;
  auto* memory = rex::Runtime::instance()->memory();
  const uint8_t* out = memory->TranslateVirtual(t_output);
  float peak = 0;
  bool non_finite = false;
  for (int i = 0; i < 128; ++i) {
    uint32_t bits = rex::memory::load_and_swap<uint32_t>(out + i * 4);
    float v;
    std::memcpy(&v, &bits, 4);
    if (!std::isfinite(v))
      non_finite = true;
    else
      peak = std::max(peak, std::fabs(v));
  }
  ++g_blocks;
  if (t_bad_headers)
    ++g_bad_header_blocks;
  if (peak > 1.2f || non_finite)
    ++g_loud_blocks;
  if ((t_bad_headers || peak > 1.2f || non_finite) && g_logged.fetch_add(1) < 200) {
    const uint8_t* in = memory->TranslateVirtual(t_input);
    REXLOG_INFO("XAS block {:08X} -> {:08X}: peak {:.3f}{}{} bytes {}", t_input, t_output, peak,
                non_finite ? " NON-FINITE" : "",
                t_bad_headers ? fmt::format(" bad coef index in subframes 0x{:X}", t_bad_headers)
                              : std::string(),
                HexBytes(in, 0x4C));
  }
  const int64_t now = NowMs();
  {
    std::lock_guard<std::mutex> guard(g_recent_lock);
    g_recent[g_recent_next++ % g_recent.size()] = {now, t_input, peak};
    const uint32_t full_scale = rex::audio::Ao2dcFullScaleCount();
    if (full_scale != g_seen_full_scale) {
      g_seen_full_scale = full_scale;
      // Group the last 400 ms of blocks by stream: blocks of one stream are consecutive 0x4C
      // records, so key by the input address with the low 12 bits dropped.
      struct Stream {
        uint32_t key, blocks = 0;
        float peak = 0;
        int64_t first = 0, last = 0;
      };
      std::vector<Stream> streams;
      for (const auto& r : g_recent) {
        if (!r.ms || now - r.ms > 400)
          continue;
        const uint32_t key = r.input >> 12;
        Stream* st = nullptr;
        for (auto& x : streams)
          if (x.key == key)
            st = &x;
        if (!st) {
          streams.push_back({key});
          st = &streams.back();
          st->first = r.ms;
        }
        ++st->blocks;
        st->peak = std::max(st->peak, r.peak);
        st->first = std::min(st->first, r.ms);
        st->last = std::max(st->last, r.ms);
      }
      std::sort(streams.begin(), streams.end(),
                [](const Stream& a, const Stream& b) { return a.peak > b.peak; });
      std::string list;
      for (size_t i = 0; i < streams.size() && i < 8; ++i)
        list += fmt::format(" [{:05X}xxx blocks {} peak {:.3f} at -{}..-{} ms]", streams[i].key,
                            streams[i].blocks, streams[i].peak, now - streams[i].first,
                            now - streams[i].last);
      REXLOG_INFO("Full scale #{}: XAS in last 400 ms ({} streams):{}", full_scale,
                  streams.size(), list);
    }
  }
  int64_t last = g_last_report_ms.load();
  if (now - last >= 5000 && g_last_report_ms.compare_exchange_strong(last, now) && last) {
    REXLOG_INFO("XAS blocks: {} decoded, {} with bad headers, {} louder than 1.2 / non-finite; "
                "Layer III granules: decoder A {}, decoder B {} ({} output samples)",
                g_blocks.exchange(0), g_bad_header_blocks.exchange(0), g_loud_blocks.exchange(0),
                g_l3a_granules.exchange(0), g_l3b_granules.exchange(0), g_l3b_samples.exchange(0));
  }
  t_input = 0;
}

// EaLayer3 (decoder B) output stage, sub_838D8668 and sub_838D8BC0: decoded floats are either
// scaled by 1/32767 into a float buffer (no clamp; hook on the store, f13 = value) or clamped to
// [-32768, 32767] and truncated to 16-bit PCM (hooks on the two clamp stores). Normal content
// never needs the clamp, so clamping (or float output above 1.2) means the decoder produced
// over-range samples. Counted per 100 ms; a busy window is logged with its time.
namespace {
std::atomic<uint32_t> g_l3_samples_clamped{0}, g_l3_float_loud{0}, g_l3_window_clamped{0};
std::atomic<int64_t> g_l3_window_ms{0}, g_l3_report_ms{0};
std::atomic<uint32_t> g_l3_logged{0};

void L3Count(bool clamp) {
  if (clamp)
    ++g_l3_samples_clamped;
  else
    ++g_l3_float_loud;
  const int64_t now = NowMs();
  int64_t window = g_l3_window_ms.load();
  if (now - window >= 100 && g_l3_window_ms.compare_exchange_strong(window, now)) {
    const uint32_t n = g_l3_window_clamped.exchange(0);
    if (n >= 16 && g_l3_logged.fetch_add(1) < 200)
      REXLOG_INFO("EaLayer3 output: {} over-range samples in 100 ms", n);
  }
  ++g_l3_window_clamped;
  int64_t last = g_l3_report_ms.load();
  if (now - last >= 5000 && g_l3_report_ms.compare_exchange_strong(last, now) && last)
    REXLOG_INFO("EaLayer3 output: {} samples clamped, {} float samples above 1.2 in the last 5 s",
                g_l3_samples_clamped.exchange(0), g_l3_float_loud.exchange(0));
}
}  // namespace

void Ao2dcL3Clamp() {
  if (REXCVAR_GET(ao2dc_xas_check))
    L3Count(true);
}

void Ao2dcL3Float(PPCRegister& f13) {
  if (!REXCVAR_GET(ao2dc_xas_check))
    return;
  ++g_l3b_samples;
  if (std::fabs(f13.f64) > 1.2)
    L3Count(false);
}

void Ao2dcL3IntSample() {
  if (REXCVAR_GET(ao2dc_xas_check))
    ++g_l3b_samples;
}

void Ao2dcL3GranuleA() {
  if (REXCVAR_GET(ao2dc_xas_check))
    ++g_l3a_granules;
}

void Ao2dcL3GranuleB() {
  if (REXCVAR_GET(ao2dc_xas_check))
    ++g_l3b_granules;
}

// Mixer filter designs: five designers write biquad coefficients.
//   A sub_838A5CC0 (hook 0x838A5DB8), B sub_838A5A80 (0x838A5B18), C sub_838A5C08 (0x838A5CA4):
//     five floats at r31, observed layout [a1 a2 b0 b1 b2] (a pass-through design gave
//     -1.2796 0.4776 1 -1.2796 0.4776).
//   V1 sub_838A3D78 (0x838A3F28), V2 sub_838A3B48 (0x838A3CCC): vector loops, 8 floats per pass
//     at r31.
// A design is logged when the coefficients of that object change (up to 400 lines), flagged
// UNSTABLE if the feedback pair fails |a2| < 1 and |a1| < 1 + a2, or BAD if a value is not finite
// or larger than 8 (ao2dc_xas_check).
namespace {
void LogFilterDesign(const char* designer, uint32_t address, int count, bool biquad) {
  if (!REXCVAR_GET(ao2dc_xas_check))
    return;
  const uint8_t* c = rex::Runtime::instance()->memory()->TranslateVirtual(address);
  float k[8] = {};
  bool bad = false;
  for (int i = 0; i < count; ++i) {
    uint32_t bits = rex::memory::load_and_swap<uint32_t>(c + i * 4);
    std::memcpy(&k[i], &bits, 4);
    if (!std::isfinite(k[i]) || std::fabs(k[i]) > 8.0f)
      bad = true;
  }
  const bool unstable =
      biquad && (std::fabs(k[1]) >= 1.0f || std::fabs(k[0]) >= 1.0f + k[1]);
  static std::mutex lock;
  static std::vector<std::pair<uint64_t, uint64_t>> last;  // (designer^address, coefficient hash)
  // Coarse hash: only changes of more than ~5% (or 0.05 absolute) count as a new design, so a
  // smoothly gliding filter does not flood the log.
  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < count; ++i) {
    const float v = k[i];
    const float step = std::max(0.05f, std::fabs(v) * 0.05f);
    uint32_t q = uint32_t(int32_t(std::floor(v / step))) ^ (uint32_t(std::ilogb(step + 1e-9f)) << 24);
    hash = (hash ^ q) * 1099511628211ull;
  }
  const uint64_t key = (uint64_t(uintptr_t(designer)) << 32) ^ address;
  bool changed = true;
  {
    std::lock_guard<std::mutex> guard(lock);
    for (auto& e : last) {
      if (e.first == key) {
        changed = e.second != hash;
        e.second = hash;
        goto done;
      }
    }
    if (last.size() < 4096)
      last.push_back({key, hash});
  done:;
  }
  static std::atomic<uint32_t> logged_changes{0}, logged_flags{0};
  const bool flagged = bad || unstable;
  if ((flagged && logged_flags.fetch_add(1) < 400) ||
      (!flagged && changed && logged_changes.fetch_add(1) < 400)) {
    std::string values;
    for (int i = 0; i < count; ++i)
      values += fmt::format(" {:.5f}", k[i]);
    REXLOG_INFO("Filter design {} @{:08X}:{}{}{}", designer, address, values,
                unstable ? " UNSTABLE" : "", bad ? " BAD" : "");
  }
}
}  // namespace

// The filter transaction recorder (filter_recorder.cpp) timestamps every designer write.
void Ao2dcFilterRecCoefWrite(uint32_t address, uint32_t designer);

void Ao2dcFilterDesign(PPCRegister& r31, PPCRegister&, PPCRegister&) {
  Ao2dcFilterRecCoefWrite(r31.u32, 1);
  LogFilterDesign("A", r31.u32, 5, true);
}
void Ao2dcFilterDesignB(PPCRegister& r31) {
  Ao2dcFilterRecCoefWrite(r31.u32, 2);
  LogFilterDesign("B", r31.u32, 5, true);
}
void Ao2dcFilterDesignC(PPCRegister& r31) {
  Ao2dcFilterRecCoefWrite(r31.u32, 3);
  LogFilterDesign("C", r31.u32, 5, true);
}
void Ao2dcFilterDesignV1(PPCRegister& r31) {
  Ao2dcFilterRecCoefWrite(r31.u32, 4);
  LogFilterDesign("V1", r31.u32, 8, false);
}
void Ao2dcFilterDesignV2(PPCRegister& r31) {
  Ao2dcFilterRecCoefWrite(r31.u32, 5);
  LogFilterDesign("V2", r31.u32, 8, false);
}

// Audio mix-job threading (sub_82E3B418, Frostbite passing its audio settings to AudioCore):
// 0x82E3B734 follows `lbz r6,73(r28)` (AudioCoreMultipleMixJobsEnabled, copied to the core's
// config), 0x82E3B73C follows `lwz r11,44(r28)` (AudioCoreMaxMixJobThreadCount). With
// ao2dc_audio_single_mix_job on, the mix runs as a single job, so no two threads update one
// voice's filters at once. Takes effect when the audio system starts.
REXCVAR_DEFINE_BOOL(ao2dc_audio_single_mix_job, false, "Audio",
                    "AO2DC diagnostic: run the game's audio mix as one job instead of several "
                    "threads. Needs a restart");

void Ao2dcAudioMultipleMixJobs(PPCRegister& r6) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("AudioCore multiple mix jobs: game setting {}{}", r6.u32 & 0xFF,
                REXCVAR_GET(ao2dc_audio_single_mix_job) ? " -> 0 (single mix job)" : "");
  if (REXCVAR_GET(ao2dc_audio_single_mix_job))
    r6.u64 = 0;
}

void Ao2dcAudioMaxMixJobThreads(PPCRegister& r11) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("AudioCore max mix job threads: game setting {}{}", r11.s32,
                REXCVAR_GET(ao2dc_audio_single_mix_job) ? " -> 1" : "");
  if (REXCVAR_GET(ao2dc_audio_single_mix_job))
    r11.u64 = 1;
}

// Biquad processing (sub_838A58C0, 8 samples per pass: y = b0 x + b1 x1 + b2 x2 + 1e-18
// - a1 y1 - a2 y2, coefficients [a1 a2 b0 b1 b2] at r6, state x1 x2 y1 y2 at r3). At 0x838A5A50,
// after the loop, f6/f3 hold the last two outputs. Normal audio stays within about +-1; a filter
// whose output exceeds 4 or is not finite has run away and is logged with its coefficients
// (ao2dc_xas_check). A 5-second summary gives the largest output seen.
void Ao2dcBiquadEnd(PPCRegister& f6, PPCRegister& f3, PPCRegister& r6, PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_xas_check))
    return;
  static std::atomic<uint32_t> max_milli{0}, runaway{0}, calls{0}, logged{0};
  static std::atomic<int64_t> report_ms{0};
  const double y1 = f6.f64, y2 = f3.f64;
  const double m = std::max(std::fabs(y1), std::fabs(y2));
  const bool bad = !std::isfinite(y1) || !std::isfinite(y2) || m > 4.0;
  ++calls;
  uint32_t milli = uint32_t(std::min(m, 4.0e6) * 1000.0);
  uint32_t prev = max_milli.load();
  while (milli > prev && !max_milli.compare_exchange_weak(prev, milli)) {
  }
  if (bad) {
    ++runaway;
    if (logged.fetch_add(1) < 200) {
      const uint8_t* c = rex::Runtime::instance()->memory()->TranslateVirtual(r6.u32);
      float k[5];
      for (int i = 0; i < 5; ++i) {
        uint32_t bits = rex::memory::load_and_swap<uint32_t>(c + i * 4);
        std::memcpy(&k[i], &bits, 4);
      }
      REXLOG_INFO("Biquad RUNAWAY: state @{:08X} coef @{:08X} [{:.5f} {:.5f} {:.5f} {:.5f} {:.5f}] "
                  "y1 {:.4g} y2 {:.4g}",
                  r3.u32, r6.u32, k[0], k[1], k[2], k[3], k[4], y1, y2);
    }
  }
  const int64_t now = NowMs();
  int64_t last = report_ms.load();
  if (now - last >= 5000 && report_ms.compare_exchange_strong(last, now) && last)
    REXLOG_INFO("Biquads: {} blocks, {} runaway, largest output {:.3f} in the last 5 s",
                calls.exchange(0), runaway.exchange(0), max_milli.exchange(0) / 1000.0);
}
