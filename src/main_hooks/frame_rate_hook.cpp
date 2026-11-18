// frame_rate_hook.cpp - optional 60 FPS for AO4.Main (ao2dc_frame_rate).
//
// The game paces itself rather than relying on the swap interval. Three things hold it at 30:
//
// 1. Frame calibration, sub_82EE7220. While the game starts it times frames of "counter + 2" and
//    keeps the measurement only when it is close to the expected frame time at 0x84076CC0
//    (double, 1/30). The result goes to 0x84076CD0 (frame time, the wait target of pacer mode 2)
//    and 0x84076CC8 (its inverse, mode 2's tick rate).
// 2. Pacer construction, sub_82EED050. Each pacer is built with a fixed rate (30) from which it
//    derives pacer+48 (rate), pacer+432 (the duration of one tick, which tick-driven systems such
//    as movement and menus advance by), pacer+440 and a tick history.
// 3. The pacer settings object (pacer+44): a base rate at +32 (30) and a cap at +36 (200), read by
//    the pacer itself, sub_82EED220.
// 4. The D3D flip interval: each flip is queued by sub_82CB85A0 for "previous flip + interval"
//    vblanks, with the interval packed into its argument from the device's presentation interval
//    (2 = every second 60 Hz vblank).
//
// With ao2dc_frame_rate above 30, the calibration times "counter + 1" against 1/rate, the pacers
// are built at that rate and the base rate and cap are set to it, so the game derives consistent
// timing for the new rate, and flips go out on every vblank. Tested at 60 at the start of the game (normal game speed, sprint, the
// partner's breach, menus). 0 (the default) leaves the game untouched.
//
// Compiled into the AO4_Main module target (see CMakeLists.txt); the hooks are declared by codegen
// from the [[midasm_hook]] entries in config/ao4_main_functions.toml.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

REXCVAR_DEFINE_INT32(ao2dc_frame_rate, 0, "Game",
                     "AO2DC: frame rate for the game's own frame pacer (0 = the game's normal 30; "
                     "60 = experimental 60 FPS; video_mode_refresh_rate must match). Needs a "
                     "restart")
    .range(0, 240);

REXCVAR_DEFINE_BOOL(ao2dc_trace_game_time, false, "Game",
                    "AO2DC: once per real second, log a GameTime line with the player updates "
                    "and the game time they consumed (diagnostic, logging only)");

REXCVAR_DEFINE_INT32(ao2dc_catch_up_steps, 8, "Game",
                     "AO2DC: when the GPU cannot hold the frame rate, run up to this many game "
                     "steps per rendered frame so the game keeps real-time speed (default 8; 0 = the game's "
                     "own behaviour: one step per frame, slow motion under load)")
    .range(0, 8);

REXCVAR_DEFINE_INT32(ao2dc_flip_interval, 0, "Game",
                     "AO2DC diagnostic: with ao2dc_frame_rate above 30, show a frame only every "
                     "Nth display refresh (0 = every refresh)")
    .range(0, 4);

namespace {

// double, 1/30: calibration reference. Title Update 1 (0.0.1.11) moved .data by +0x10000.
#if AO2DC_TITLE_UPDATE
constexpr uint32_t kExpectedFrameTime = 0x84086CC0u;
#else
constexpr uint32_t kExpectedFrameTime = 0x84076CC0u;
#endif

// 60 FPS mode is all or nothing. The pacer constructor, calibration step, flip interval and the
// settings/calibration writes below all follow one decision, made on first use from values that do
// not depend on call order: the requested rate must be above 30 and the display mode the game
// calibrates against (video_mode_refresh_rate) must equal it, so the game's own calibration
// reference is 2/rate (see Ao2dcFramePacer). Otherwise nothing is changed and the game runs at 30.
std::atomic<int> g_mode60{0};  // 0 undecided, 1 on, 2 off

int DecideMode60() {
  const int32_t target = REXCVAR_GET(ao2dc_frame_rate);
  const double refresh = REXCVAR_GET(video_mode_refresh_rate);
  const bool ok = target > 30 && std::fabs(refresh - double(target)) < 0.5;
  int expected = 0;
  if (g_mode60.compare_exchange_strong(expected, ok ? 1 : 2)) {
    if (ok)
      REXLOG_INFO("Frame rate: {} FPS mode on (display mode {:.2f} Hz, pacer, calibration and "
                  "flip hooks together)", target, refresh);
    else
      REXLOG_WARN("Frame rate: {} FPS mode OFF, nothing changed: video_mode_refresh_rate is "
                  "{:.2f} Hz, it must equal ao2dc_frame_rate (above 30). The game runs at 30.",
                  target, refresh);
  }
  return g_mode60.load();
}

bool Active() {
  if (REXCVAR_GET(ao2dc_frame_rate) <= 30)
    return false;
  const int state = g_mode60.load();
  return (state ? state : DecideMode60()) == 1;
}

float LoadFloat(uint8_t* p) {
  uint32_t bits = rex::memory::load_and_swap<uint32_t>(p);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint8_t* p, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  rex::memory::store_and_swap<uint32_t>(p, bits);
}

double LoadDouble(uint8_t* p) {
  uint64_t bits = rex::memory::load_and_swap<uint64_t>(p);
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

// Stores into the image's read-only data: lift the host page protection for this one store.
bool StoreDoubleReadOnly(uint8_t* p, double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  DWORD old_protect = 0;
  if (VirtualProtect(p, 8, PAGE_READWRITE, &old_protect)) {
    rex::memory::store_and_swap<uint64_t>(p, bits);
    VirtualProtect(p, 8, old_protect, &old_protect);
  }
  return rex::memory::load_and_swap<uint64_t>(p) == bits;
}

// One line naming the DLL this code runs from and when it was compiled (ao2dc_trace_game_time).
void LogBuildMarker(const char* where) {
  wchar_t path[MAX_PATH] = L"?";
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&LogBuildMarker), &module))
    GetModuleFileNameW(module, path, MAX_PATH);
  char utf8[MAX_PATH * 3] = "?";
  WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
  REXLOG_INFO("{}: build marker {} {}, loaded from {}", where, __DATE__, __TIME__, utf8);
}

}  // namespace

// sub_82EED220 at 0x82EED238, right after `lwz r29,44(r3)`: r3 = pacer, r29 = its settings.
// Runs every frame for each pacer, before the pacer reads its settings or calibrates.
void Ao2dcFramePacer(PPCRegister& r3, PPCRegister& r29) {
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true))
    REXLOG_INFO("Frame rate: ao2dc_frame_rate = {}", REXCVAR_GET(ao2dc_frame_rate));
  if (REXCVAR_GET(ao2dc_trace_game_time) && r3.u32) {
    // First calls of the pacer tick, before any filter: which pacers tick, their mode and scale.
    static std::atomic<int> entries{0};
    if (entries.fetch_add(1) < 8) {
      uint8_t* pacer = rex::Runtime::instance()->memory()->TranslateVirtual(r3.u32);
      REXLOG_INFO("TickTime: pacer tick entered, pacer 0x{:08X}, mode {}, rate {}, scale {}, "
                  "settings 0x{:08X}",
                  r3.u32, rex::memory::load_and_swap<uint32_t>(pacer + 40),
                  rex::memory::load_and_swap<uint32_t>(pacer + 48), LoadDouble(pacer + 96),
                  r29.u32);
    }
  }
  if (REXCVAR_GET(ao2dc_trace_game_time)) {
    // One-time diagnostic: where the image keeps 1/60 s constants (candidate frame-time floors).
    static std::atomic<bool> scanned{false};
    if (!scanned.exchange(true)) {
      auto* mem = rex::Runtime::instance()->memory();
      int hits = 0;
      for (uint32_t a = 0x82000000u; a < 0x82CB0000u && hits < 60; a += 4) {
        uint8_t* p = mem->TranslateVirtual(a);
        if ((a & 0xFFF) == 0) {
          MEMORY_BASIC_INFORMATION info;
          if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
              (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            a += 0xFFC;
            continue;
          }
        }
        const uint32_t w = rex::memory::load_and_swap<uint32_t>(p);
        const char* what = w == 0x3C888889u   ? "float 1/60"
                           : w == 0x41855555u ? "float 16.67"
                           : w == 0x42700000u ? "float 60"
                           : w == 0x3F911111u && rex::memory::load_and_swap<uint32_t>(p + 4) ==
                                                     0x11111111u
                               ? "double 1/60"
                               : nullptr;
        if (what) {
          REXLOG_INFO("ConstScan: {} at 0x{:08X}", what, a);
          ++hits;
        }
      }
      REXLOG_INFO("ConstScan: done, {} hits", hits);
    }
  }
  if (REXCVAR_GET(ao2dc_trace_game_time) && r3.u32) {
    // Per pacer and real second: tick calls, and the elapsed time (pacer+80) the previous ticks
    // left for the game to read. "delivered" far from 1.0 s means that pacer's clients run
    // fast or slow.
    using Clock = std::chrono::steady_clock;
    struct Seen {
      uint32_t pacer = 0, calls = 0;
      double delivered = 0;
      Clock::time_point start;
    };
    static std::mutex mutex;
    static Seen seen[4];
    std::lock_guard lock(mutex);
    for (Seen& s : seen) {
      if (s.pacer && s.pacer != r3.u32)
        continue;
      uint8_t* pacer = rex::Runtime::instance()->memory()->TranslateVirtual(r3.u32);
      const auto now = Clock::now();
      if (!s.pacer) {
        s.pacer = r3.u32;
        s.start = now;
      } else {
        s.calls++;
        s.delivered += LoadDouble(pacer + 80);
      }
      const double wall = std::chrono::duration<double>(now - s.start).count();
      if (wall >= 1.0) {
        REXLOG_INFO("TickTime: pacer 0x{:08X} mode {}, tick calls {}, delivered {:.3f} s in "
                    "{:.3f} s", r3.u32, rex::memory::load_and_swap<uint32_t>(pacer + 40),
                    s.calls, s.delivered, wall);
        s.calls = 0;
        s.delivered = 0;
        s.start = now;
      }
      break;
    }
  }
  if (!r3.u32 || !r29.u32 || !Active())
    return;
  auto* memory = rex::Runtime::instance()->memory();
  const int32_t target = REXCVAR_GET(ao2dc_frame_rate);

  // Calibration reference: 1/target instead of 1/30 (see Ao2dcCalibrationStep).
  uint8_t* expected = memory->TranslateVirtual(kExpectedFrameTime);
  const double want_expected = 1.0 / double(target);
  const double current = LoadDouble(expected);
  // The game sets the reference to 2 / (display refresh rate) at startup. Only write when it
  // holds that value for a display running at the target rate, or our own value: with any other
  // refresh rate (or game version) the calibration cannot succeed, so leave the game untouched.
  if (std::fabs(current - 2.0 * want_expected) > 1e-6 &&
      std::fabs(current - want_expected) > 1e-9) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      REXLOG_ERROR("Frame rate: calibration reference is {} s (display refresh {:.0f} Hz), not "
                   "2/{}, although the {} FPS hooks are active: timing is inconsistent. Check "
                   "kExpectedFrameTime for this game revision.",
                   current, 2.0 / current, target, target);
    return;
  }
  if (std::fabs(current - want_expected) > 1e-9) {
    if (StoreDoubleReadOnly(expected, want_expected))
      REXLOG_INFO("Frame rate {}: calibration expects {:.6f} s (verified)", target, want_expected);
    else
      REXLOG_ERROR("Frame rate {}: calibration reference write at 0x{:08X} did not take effect; "
                   "the other 60 FPS hooks are active, expect a mixed timing", target,
                   kExpectedFrameTime);
  }

  // Mode 2 pacer (the main client pacer, sub_82EED420 at 0x82EED4A0) waits for a frame time read
  // from the doubles next to the calibration reference: kExpectedFrameTime+8 (the rate it substitutes
  // when the display is syncing, 30.0) and +16 (the wait target, 1/30 s). Left alone they hold the
  // render rate at 30 whatever the pacer rate is. Each is rewritten only when it still holds its
  // 30 Hz value.
  struct WaitConstant {
    uint32_t address;
    double from, to;
    const char* what;
  };
  const WaitConstant constants[] = {
      {kExpectedFrameTime + 8, 30.0, double(target), "rate substitute"},
      {kExpectedFrameTime + 16, 1.0 / 30.0, want_expected, "wait target"},
  };
  for (const WaitConstant& c : constants) {
    uint8_t* at = memory->TranslateVirtual(c.address);
    const double now_value = LoadDouble(at);
    static std::atomic<uint32_t> reported_mask{0};
    const uint32_t bit = 1u << ((c.address - kExpectedFrameTime) / 8);
    const bool first = !(reported_mask.fetch_or(bit) & bit);
    if (std::fabs(now_value - c.from) < 1e-9) {
      const bool ok = StoreDoubleReadOnly(at, c.to);
      REXLOG_INFO("Frame rate {}: {} at 0x{:08X} {:.6f} -> {:.6f} ({})", target, c.what, c.address,
                  now_value, c.to, ok ? "verified" : "WRITE FAILED");
    } else if (first && std::fabs(now_value - c.to) > 1e-9) {
      REXLOG_WARN("Frame rate {}: {} at 0x{:08X} holds {:.6f}, expected {:.6f} or {:.6f}; left "
                  "unchanged, the frame wait may still cap the render rate", target, c.what,
                  c.address, now_value, c.from, c.to);
    }
  }

  // Base rate and cap of the pacer settings.
  uint8_t* settings = memory->TranslateVirtual(r29.u32);
  const float rate = float(target);
  if (LoadFloat(settings + 32) > 0.0f && LoadFloat(settings + 32) != rate)
    StoreFloat(settings + 32, rate);
  if (LoadFloat(settings + 36) > 0.0f && LoadFloat(settings + 36) != rate)
    StoreFloat(settings + 36, rate);
}

// sub_82EE7220 at 0x82EE72F4, right after `addi r11,r11,2`: r11 is the counter value the
// calibration waits for. Time one counter step per frame instead of two.
void Ao2dcCalibrationStep(PPCRegister& r11) {
  if (Active())
    r11.u64 = r11.u32 - 1;
}

// sub_82EED050 at 0x82EED064, before `mr r30,r5`: r5 is the new pacer's fixed rate. Build the
// pacers at the target rate so their tick duration and history match it.
void Ao2dcPacerConstruct(PPCRegister& r5) {
  if (!Active() || r5.u32 != 30)
    return;
  const uint32_t rate = uint32_t(REXCVAR_GET(ao2dc_frame_rate));
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4)
    REXLOG_INFO("Frame pacer created at {} instead of 30", rate);
  r5.u64 = rate;
}

// sub_82CB85A0 (D3D flip callback) at 0x82CB85EC, right after `rlwinm r29,r30,24,28,31`: r29 is
// the vblank interval of this flip. Flip on every vblank instead of every second one.
void Ao2dcFlipInterval(PPCRegister& r29) {
  if (Active() && r29.u32 > 1)
    r29.u64 = 1;
  // Diagnostic: show a frame only every Nth display refresh, leaving the refresh rate alone.
  if (Active() && REXCVAR_GET(ao2dc_flip_interval) > 0)
    r29.u64 = uint32_t(REXCVAR_GET(ao2dc_flip_interval));
}

// Game-time diagnostic (ao2dc_trace_game_time). Path of the tick duration (pacer+432 = 1/rate s):
// sub_82EEA6E8 returns it (times pacer+96) to the main loop sub_82CD6860, which hands the world a
// time block (+4 elapsed, +8 tick count, +20 tick duration). sub_82F72AD8 then runs one simulation
// step per tick through sub_82F5FBE0, whose dt is 1/rate of the simulation clock (sub_82EE7260).
// Every step passes that dt (seconds) to the two player slots (world+36, +40) with
// sub_830127C0(r3 = player, f1 = dt); a null slot is skipped. Nothing at this call tells the local
// player from the partner, so each slot is logged on its own line.
// Hooked at that call, 0x82F5FCB8 (r30 = 2 for slot 0, 1 for slot 1).
void Ao2dcGameTimeStep(PPCRegister& r3, PPCRegister& r30, PPCRegister& f1) {
  if (!REXCVAR_GET(ao2dc_trace_game_time))
    return;
  using Clock = std::chrono::steady_clock;
  struct Slot {
    uint32_t player = 0;
    uint64_t updates = 0;
    double game = 0, dt_min = 0, dt_max = 0, scale_min = 0, scale_max = 0;
  };
  static std::mutex mutex;
  static Clock::time_point start;
  static bool started = false;
  static Slot slots[2];
  static std::atomic<bool> marked{false};
  if (!marked.exchange(true))
    LogBuildMarker("GameTime");
  // The step multiplies dt by this float for the systems it scales (`lfs f0,6272(r24)` in
  // sub_82F5FBE0, read on every step just before this call). Title Update 1 address.
  constexpr uint32_t kStepTimeScale = 0x840D1880u;
  const double scale =
      LoadFloat(rex::Runtime::instance()->memory()->TranslateVirtual(kStepTimeScale));
  std::lock_guard lock(mutex);
  const auto now = Clock::now();
  if (!started) {
    start = now;
    started = true;
  }
  Slot& slot = slots[r30.u32 == 2 ? 0 : 1];
  const double dt = f1.f64;
  slot.dt_min = slot.updates ? std::min(slot.dt_min, dt) : dt;
  slot.dt_max = slot.updates ? std::max(slot.dt_max, dt) : dt;
  slot.scale_min = slot.updates ? std::min(slot.scale_min, scale) : scale;
  slot.scale_max = slot.updates ? std::max(slot.scale_max, scale) : scale;
  slot.player = r3.u32;
  slot.updates++;
  slot.game += dt;
  const double wall = std::chrono::duration<double>(now - start).count();
  if (wall < 1.0)
    return;
  const int32_t target = REXCVAR_GET(ao2dc_frame_rate) > 30 ? REXCVAR_GET(ao2dc_frame_rate) : 30;
  for (int i = 0; i < 2; ++i) {
    const Slot& s = slots[i];
    if (!s.updates) {
      REXLOG_INFO("GameTime: F4 target {} FPS, slot {}, no updates in {:.3f} s", target, i, wall);
      continue;
    }
    REXLOG_INFO("GameTime: F4 target {} FPS, slot {} (player 0x{:08X}), updates {}, game "
                "{:.4f} s, wall {:.3f} s, ratio {:.3f}, dt {:.5f}-{:.5f} s, step scale "
                "{:.3f}-{:.3f}",
                target, i, s.player, s.updates, s.game, wall, s.game / wall, s.dt_min, s.dt_max,
                s.scale_min, s.scale_max);
    slots[i] = {};
  }
  start = now;
}

// Tick-count producer trace (ao2dc_trace_game_time). sub_82EED420 (the pacer's per-frame tick) at
// loc_82EED8D0: pacer+52 already holds the new tick counter and pacer+56 the previous one, and the
// clamp that follows limits the difference to settings+40 ticks (settings = pacer+44) and sets the
// frame's elapsed time (pacer+80) to that many ticks. The counter comes from the wall clock
// (sub_82EE7720: (now - pacer+176) * pacer+96 * rate + pacer+168), except in mode 1 at scale 1.0,
// where it is the previous counter + 1. pacer+88 is the wall time the pacer measured this frame.
// One TickTime line per pacer per real second. r31 = pacer.
void Ao2dcTickCount(PPCRegister& r31) {
  static std::atomic<bool> entered{false};
  if (!entered.exchange(true)) {
    REXLOG_INFO("TickTime: producer hook entered, pacer 0x{:08X}, ao2dc_trace_game_time {}",
                r31.u32, REXCVAR_GET(ao2dc_trace_game_time));
    LogBuildMarker("TickTime");
  }
  if (!REXCVAR_GET(ao2dc_trace_game_time) || !r31.u32)
    return;
  using Clock = std::chrono::steady_clock;
  struct Pacer {
    uint32_t address = 0;
    Clock::time_point start;
    uint32_t frames = 0, wanted = 0, granted = 0, capped = 0, modes = 0;
    double elapsed = 0;
  };
  static std::mutex mutex;
  static Pacer pacers[4];
  auto* memory = rex::Runtime::instance()->memory();
  uint8_t* pacer = memory->TranslateVirtual(r31.u32);
  const uint32_t settings_address = rex::memory::load_and_swap<uint32_t>(pacer + 44);
  if (!settings_address)
    return;
  uint8_t* settings = memory->TranslateVirtual(settings_address);
  const uint32_t mode = rex::memory::load_and_swap<uint32_t>(pacer + 40);
  const uint32_t rate = rex::memory::load_and_swap<uint32_t>(pacer + 48);
  // Signed: with catch-up steps the stored counter can be ahead of the wall-clock one.
  const uint32_t wanted =
      uint32_t(std::max(0, int32_t(rex::memory::load_and_swap<uint32_t>(pacer + 52) -
                                   rex::memory::load_and_swap<uint32_t>(pacer + 56))));
  const uint32_t cap = rex::memory::load_and_swap<uint32_t>(settings + 40);
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  Pacer* p = nullptr;
  for (Pacer& candidate : pacers) {
    if (candidate.address == r31.u32 || !candidate.address) {
      p = &candidate;
      break;
    }
  }
  if (!p)
    return;  // more than four pacers alive: the rest are not traced
  if (!p->address) {
    p->address = r31.u32;
    p->start = now;
  }
  p->frames++;
  p->wanted += wanted;
  p->granted += std::min(wanted, cap);
  p->capped += wanted > cap;
  p->modes |= 1u << (mode & 7);
  p->elapsed += LoadDouble(pacer + 88);
  const double wall = std::chrono::duration<double>(now - p->start).count();
  if (wall < 1.0)
    return;
  REXLOG_INFO("TickTime: pacer 0x{:08X}, rate {}, modes 0x{:02X}, scale {:.3f}, max ticks/frame "
              "{}, fixed elapsed {}, frames {}, pacer elapsed {:.3f} s, ticks wanted {}, granted "
              "{}, capped frames {}, wall {:.3f} s, catch-up steps {}",
              p->address, rate, p->modes, LoadDouble(pacer + 96), cap, settings[58], p->frames,
              p->elapsed, p->wanted, p->granted, p->capped, wall,
              REXCVAR_GET(ao2dc_catch_up_steps));
  *p = {.address = r31.u32, .start = now};
}

// Steps for one rendered frame worth `worth` steps of real time (ao2dc_catch_up_steps). `carry` is
// the time not yet turned into steps, in steps; it goes negative when the one step every frame
// always runs was more than the frame was worth, so every executed step is paid for.
// Returns 0 for a paused pacer (scale 0): the game's own code handles it and nothing is owed.
constexpr int CatchUpStepCount(double worth, double scale, int limit, double& carry) {
  if (scale <= 0.0) {
    carry = 0.0;  // no backlog across a pause
    return 0;
  }
  // A frame within 2% of the target is exactly one step: a 59.94 Hz display must not add a
  // double-step frame every few seconds.
  if (worth > 0.98 && worth < 1.02)
    worth = 1.0;
  const double total = worth + carry;
  int steps = total >= 2.0 ? int(total + 1e-9) : 1;
  if (steps > limit)
    steps = limit;
  carry = total - steps;
  // Time beyond the per-frame limit is dropped, as the game's clamp does; debt is bounded too.
  carry = carry > 1.0 ? 1.0 : carry < -1.0 ? -1.0 : carry;
  return steps;
}

// Regression checks, at compile time: steps run over `frames` frames at a steady frame rate.
constexpr int CatchUpStepsOver(double fps, double rate, int frames, int limit, double carry = 0) {
  int steps = 0;
  for (int i = 0; i < frames; ++i)
    steps += CatchUpStepCount(rate / fps, 1.0, limit, carry);
  return steps;
}
constexpr int CatchUpStepsAfterPause(double carry) {
  CatchUpStepCount(300.0, 0.0, 6, carry);  // paused frame, however long
  return CatchUpStepsOver(60, 60, 60, 6, carry);
}
static_assert(CatchUpStepsOver(45, 60, 45, 6) >= 59 && CatchUpStepsOver(45, 60, 45, 6) <= 60);
static_assert(CatchUpStepsOver(45, 30, 45, 6) >= 45 && CatchUpStepsOver(45, 30, 45, 6) <= 46);
static_assert(CatchUpStepsOver(60, 60, 60, 6) == 60);
static_assert(CatchUpStepsOver(59.94, 60, 600, 6) == 600);
static_assert(CatchUpStepsOver(11, 60, 11, 6) >= 59 && CatchUpStepsOver(11, 60, 11, 6) <= 60);
static_assert(CatchUpStepsOver(11, 30, 11, 6) >= 29 && CatchUpStepsOver(11, 30, 11, 6) <= 30);
static_assert(CatchUpStepsAfterPause(0.9) == 60);              // resume: no backlog
static_assert(CatchUpStepsOver(60, 60, 60, 6, 1.0) <= 61);     // after a stall: at most one step

// Catch-up steps (ao2dc_catch_up_steps), at the end of the pacer tick sub_82EED420. With the
// settings+58 flag set (it is, in game), the tick ends by resetting the counter to "previous + 1"
// and the frame's elapsed time to one tick, so the game runs one step per rendered frame and goes
// into slow motion whenever the GPU is below the target rate. (The clamp before it also keeps at
// most settings+40 = 2 ticks, and at 60 the wall-clock counter itself still runs at the settings'
// base rate, so neither can supply the missing steps.)
// Instead, give the frame the steps its measured time (pacer+88) is worth at the pacer's own
// rate, carrying the remainder to the next frame (CatchUpStepCount). The step size (1/rate) is
// untouched and a paused pacer is left to the game's code.
// 0x82EED97C, before `lbz r8,58(r9)`: r31 = pacer, r9 = settings, r10 = previous counter
// (pacer+56). Returns true to skip the game's reset.
bool Ao2dcCatchUpSteps(PPCRegister& r31, PPCRegister& r9, PPCRegister& r10) {
  const int32_t limit = REXCVAR_GET(ao2dc_catch_up_steps);
  if (limit <= 0)
    return false;
  auto* memory = rex::Runtime::instance()->memory();
  uint8_t* pacer = memory->TranslateVirtual(r31.u32);
  if (!memory->TranslateVirtual(r9.u32)[58])
    return false;
  const double scale = LoadDouble(pacer + 96);
  const double rate = double(rex::memory::load_and_swap<uint32_t>(pacer + 48));
  // Carry per pacer (the game thread ticks them).
  struct Carry {
    uint32_t pacer = 0;
    double steps = 0;
  };
  static std::mutex mutex;
  static Carry carries[4];
  int steps;
  {
    std::lock_guard lock(mutex);
    Carry* carry = nullptr;
    for (Carry& candidate : carries) {
      if (candidate.pacer == r31.u32 || !candidate.pacer) {
        carry = &candidate;
        break;
      }
    }
    double none = 0;  // a fifth pacer gets no carry
    if (carry)
      carry->pacer = r31.u32;
    steps = CatchUpStepCount(LoadDouble(pacer + 88) * rate, scale, limit, carry ? carry->steps : none);
  }
  if (steps <= 0)
    return false;
  rex::memory::store_and_swap<uint32_t>(pacer + 52, r10.u32 + uint32_t(steps));
  const double elapsed = steps / rate * scale;
  uint64_t bits;
  std::memcpy(&bits, &elapsed, 8);
  rex::memory::store_and_swap<uint64_t>(pacer + 80, bits);
  return true;
}
