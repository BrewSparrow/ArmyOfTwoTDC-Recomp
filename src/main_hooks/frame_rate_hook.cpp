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

#include <atomic>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

REXCVAR_DEFINE_INT32(ao2dc_frame_rate, 0, "Game",
                     "AO2DC: frame rate for the game's own frame pacer (0 = the game's normal 30; "
                     "60 = experimental 60 FPS). Needs a restart")
    .range(0, 120);

namespace {

// double, 1/30: calibration reference. Title Update 1 (0.0.1.11) moved .data by +0x10000.
#if AO2DC_TITLE_UPDATE
constexpr uint32_t kExpectedFrameTime = 0x84086CC0u;
#else
constexpr uint32_t kExpectedFrameTime = 0x84076CC0u;
#endif

bool Active() {
  return REXCVAR_GET(ao2dc_frame_rate) > 30;
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
void StoreDoubleReadOnly(uint8_t* p, double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  DWORD old_protect = 0;
  if (VirtualProtect(p, 8, PAGE_READWRITE, &old_protect)) {
    rex::memory::store_and_swap<uint64_t>(p, bits);
    VirtualProtect(p, 8, old_protect, &old_protect);
  }
}

}  // namespace

// sub_82EED220 at 0x82EED238, right after `lwz r29,44(r3)`: r3 = pacer, r29 = its settings.
// Runs every frame for each pacer, before the pacer reads its settings or calibrates.
void Ao2dcFramePacer(PPCRegister& r3, PPCRegister& r29) {
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true))
    REXLOG_INFO("Frame rate: ao2dc_frame_rate = {}", REXCVAR_GET(ao2dc_frame_rate));
  if (!r3.u32 || !r29.u32 || !Active())
    return;
  auto* memory = rex::Runtime::instance()->memory();
  const int32_t target = REXCVAR_GET(ao2dc_frame_rate);

  // Calibration reference: 1/target instead of 1/30 (see Ao2dcCalibrationStep).
  uint8_t* expected = memory->TranslateVirtual(kExpectedFrameTime);
  const double want_expected = 1.0 / double(target);
  const double current = LoadDouble(expected);
  // Never write unless the address holds the reference (1/30) or our own value: a build whose
  // address does not match the game version must leave the game untouched.
  if (std::fabs(current - 1.0 / 30.0) > 1e-9 && std::fabs(current - want_expected) > 1e-9) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      REXLOG_WARN("Frame rate: 0x{:08X} holds {} instead of 1/30; 60 FPS calibration skipped "
                  "(game version does not match this build)", kExpectedFrameTime, current);
    return;
  }
  if (std::fabs(current - want_expected) > 1e-9) {
    StoreDoubleReadOnly(expected, want_expected);
    REXLOG_INFO("Frame rate {}: calibration expects {:.6f} s", target, want_expected);
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
}
