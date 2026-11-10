// AO2DC diagnostic: per-instance capture of loud (gain >= 2.5) and normal (gain 0.35) sound
// instances at the AudioCore voice gain calculation (sub_82E2A040, 0x82E2B41C / 0x82E2B44C).
//
// For the first sighting of each (patch instance, voice, sample entry) it writes one JSON line to
// logs/ping_capture.jsonl with: wall clock and steady time, the centre recorder's audio frame,
// thread, both gain values, the definition / voice / sample entry / bank words, the patch
// instance words, its variable array, every node and plugin of the patch (descriptor, +52/+56
// parameters and raw words), the guest call stack (walked through the r1 back chain and the
// saved LR at -8), the audio system object (r3 of sub_82E218B8) and its sub-objects, and the
// objects the patch instance points to. Follow-up snapshots of the variables, patch words and
// plugin parameters are written at +50, +200, +500 and +1000 ms so distance/attenuation changes
// over the sound's life are visible. Also records the first guest stack of each patch instance
// at the parameter staging entry (sub_82E2CFC0). Diagnostic only: nothing is changed.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_ping_capture, false, "AO2DC",
                    "Diagnostic: write logs/ping_capture.jsonl with per-instance data (gain, "
                    "definition, sample entry, patch variables and plugin parameters, guest call "
                    "stack, audio system objects) for loud (gain >= 2.5) and gain-0.35 sound "
                    "instances, with follow-up snapshots. Restart required.");
REXCVAR_DEFINE_INT32(ao2dc_ping_capture_quiet_max, 80, "AO2DC",
                     "Diagnostic: maximum number of gain-0.35 instances ao2dc_ping_capture records.");

uint64_t Ao2dcAcFrame();
void Ao2dcCentreRequest(uint64_t delay_ns);

namespace {
uint8_t* pmem = nullptr;
std::atomic<uint32_t> audio_sys{0};
std::mutex file_m;
FILE* out = nullptr;
std::chrono::steady_clock::time_point t0;

uint32_t W(uint32_t a) { return rex::memory::load_and_swap<uint32_t>(pmem + a); }

bool Ok(uint32_t a, uint32_t len = 4) {
  if (a < 0x40000000u || a >= 0xA0000000u) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(pmem + a, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  const uint8_t* end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  return pmem + a + len <= end;
}
bool Image(uint32_t a) { return a >= 0x82000000u && a < 0x84400000u; }
bool Heap(uint32_t a) { return a >= 0x40000000u && a < 0x80000000u && Ok(a, 64); }

std::string Hex(uint32_t a, int n) {
  std::string s = "\"";
  if (Ok(a, 4u * n) || (Image(a) && Ok(a, 4u * n)))
    for (int i = 0; i < n; ++i) s += fmt::format("{}{:08X}", i ? " " : "", W(a + 4 * i));
  return s + "\"";
}

double Fv(uint32_t a) {
  const uint32_t b = W(a);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

std::string GuestStack(uint32_t r1) {
  std::string s = "[";
  uint32_t sp = r1;
  for (int i = 0; i < 28; ++i) {
    if (!Ok(sp, 4)) break;
    const uint32_t back = W(sp);
    if (back <= sp || !Ok(back - 8, 8)) break;
    const uint32_t lr = W(back - 8);
    s += fmt::format("{}\"{:08X}\"", i ? "," : "", lr);
    sp = back;
  }
  return s + "]";
}

std::string Plugins(uint32_t patch) {
  std::string s = "[";
  const uint32_t table = Ok(patch + 232) ? W(patch + 232) : 0;
  if (!Heap(table)) return "[]";
  bool first = true;
  for (uint32_t slot = 0; slot < 24; ++slot) {
    const uint32_t node = W(table + 24 * slot + 4);
    if (!Heap(node)) continue;
    std::string ps = "[";
    bool pf = true;
    for (uint32_t k = 0; k < 10; ++k) {
      const uint32_t pl = W(node + 80 + 4 * k);
      if (!Heap(pl)) continue;
      const uint32_t desc = W(pl + 20);
      if (!Image(desc)) continue;
      ps += fmt::format("{}{{\"k\":{},\"plugin\":\"{:08X}\",\"desc\":\"{:08X}\",\"p52\":{:.6g},"
                        "\"p56\":{:.6g},\"words\":{}}}",
                        pf ? "" : ",", k, pl, desc, Fv(pl + 52), Fv(pl + 56), Hex(pl, 40));
      pf = false;
    }
    ps += "]";
    s += fmt::format("{}{{\"slot\":{},\"entry\":{},\"node\":\"{:08X}\",\"node_words\":{},\"plugins\":{}}}",
                     first ? "" : ",", slot, Hex(table + 24 * slot, 6), node, Hex(node, 32), ps);
    first = false;
  }
  return s + "]";
}

std::string Pointed(uint32_t base, int nwords, int maxobj) {
  std::string s = "{";
  std::unordered_set<uint32_t> seen;
  int n = 0;
  for (int i = 0; i < nwords && n < maxobj; ++i) {
    const uint32_t p = W(base + 4 * i);
    if (!Heap(p) || seen.count(p) || (p >= base && p < base + 4u * nwords)) continue;
    seen.insert(p);
    s += fmt::format("{}\"+{}:{:08X}\":{}", n ? "," : "", 4 * i, p, Hex(p, 48));
    ++n;
  }
  return s + "}";
}

std::string Sys() {
  const uint32_t sys = audio_sys.load();
  if (!Heap(sys)) return "null";
  std::string s = fmt::format("{{\"sys\":\"{:08X}\",\"words\":{},\"objs\":{}", sys, Hex(sys, 64),
                              Pointed(sys, 64, 24));
  for (uint32_t g : {0x8412E03Cu, 0x8412DEF8u, 0x8412EE90u}) {
    const uint32_t p = Ok(g) ? W(g) : 0;
    s += fmt::format(",\"g{:08X}\":{{\"p\":\"{:08X}\",\"words\":{}}}", g, p,
                     Heap(p) ? Hex(p, 128) : std::string("\"\""));
  }
  return s + "}";
}

void Emit(const std::string& line) {
  std::lock_guard<std::mutex> lock(file_m);
  if (!out) out = std::fopen("logs/ping_capture.jsonl", "w");
  if (!out) return;
  std::fputs(line.c_str(), out);
  std::fputc('\n', out);
  std::fflush(out);
}

double Ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string WallClock() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  return fmt::format("{:02}:{:02}:{:02}.{:03}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

struct Pending {
  uint64_t id;
  uint32_t patch, voice, entry;
  double due_ms;
  int stage;
};
std::mutex pend_m;
std::deque<Pending> pending;

void Snapshot(const Pending& p) {
  const bool alive = Ok(p.voice, 64) && W(p.voice) == 0x82A15D88u && W(p.voice + 48) == p.entry;
  const uint32_t vals = Ok(p.patch + 8) ? W(p.patch + 8) : 0;
  Emit(fmt::format(
      "{{\"type\":\"snap\",\"id\":{},\"stage_ms\":{},\"t_ms\":{:.1f},\"wall\":\"{}\",\"ac_frame\":{},"
      "\"alive\":{},\"voice_words\":{},\"patch_words\":{},\"values_ptr\":\"{:08X}\",\"values\":{},"
      "\"plugins\":{},\"patch_objs\":{}}}",
      p.id, p.stage, Ms(), WallClock(), Ao2dcAcFrame(), alive ? "true" : "false",
      Hex(p.voice, 64), Hex(p.patch, 128), vals, Heap(vals) ? Hex(vals, 192) : std::string("\"\""),
      Plugins(p.patch), Pointed(p.patch, 64, 16)));
}

void ZoomPoll();

void SnapThread() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<Pending> due;
    {
      std::lock_guard<std::mutex> lock(pend_m);
      const double now = Ms();
      for (auto it = pending.begin(); it != pending.end();) {
        if (it->due_ms <= now) {
          due.push_back(*it);
          it = pending.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const Pending& p : due) Snapshot(p);
    ZoomPoll();
  }
}

// ---------------------------------------------------------------------------------------------
// Zoom foley (SoundPatchAsset AO4_Assets/Sound/Foley/Zoom/FL_Wpn_Zoom): the x4 "ping" event.
// Its patch instances are recognised by the asset name (patch+216 -> asset, asset+8 -> name), so
// nothing here depends on one session's heap addresses.

std::atomic<int> z_budget{60000};
std::atomic<int> sp_budget{300000};      // SndPlayer records per run
std::atomic<double> sp_until_ms{0.0};     // SndPlayer records are written until this time  // total zoom records per run
bool Budget() { return z_budget.fetch_sub(1, std::memory_order_relaxed) > 0; }

bool IsZoomPatch(uint32_t patch) {
  if (!Ok(patch + 216, 4)) return false;
  const uint32_t asset = W(patch + 216);
  if (!Heap(asset)) return false;
  static std::mutex m;
  static std::unordered_map<uint32_t, bool> cache;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(asset);
  if (it != cache.end()) return it->second;
  bool zoom = false;
  const uint32_t name = W(asset + 8);
  if (Ok(name, 96)) {
    char buf[97];
    std::memcpy(buf, pmem + name, 96);
    buf[96] = 0;
    for (char* c = buf; *c; ++c) *c = char(std::tolower(static_cast<unsigned char>(*c)));
    zoom = std::strstr(buf, "foley/zoom/fl_wpn_zoom") != nullptr;
  }
  if (cache.size() < 100000) cache[asset] = zoom;
  return zoom;
}

// Host input at the moment of a record: right mouse button and pad 0's left trigger (the zoom
// controls), read straight from Windows so they are independent of the game's own input path.
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
XInputGetStateFn xinput_get = nullptr;
int LeftTrigger() {
  if (!xinput_get) return -1;
  uint8_t state[16] = {};  // XINPUT_STATE: dwPacketNumber, wButtons, bLeftTrigger at offset 6
  if (xinput_get(0, state) != 0) return -1;
  return state[6];
}
bool Rmb() { return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0; }
std::string Input() { return fmt::format("\"rmb\":{},\"lt\":{}", Rmb() ? 1 : 0, LeftTrigger()); }

std::string Head(const char* type) {
  return fmt::format("{{\"type\":\"{}\",\"t_ms\":{:.1f},\"wall\":\"{}\",\"ac_frame\":{},\"thread\":{},{},",
                     type, Ms(), WallClock(), Ao2dcAcFrame(), GetCurrentThreadId(), Input());
}

// Graph values (patch+8), plugin parameter block (patch+224: 112 tag/value pairs for this asset),
// the voice table (patch+232: 5 entries of 24 bytes; +8 = plugin records, byte +20 = count) and
// every plugin record (32 bytes: +4 parameter pointer, +8 descriptor).
std::string ZoomState(uint32_t patch) {
  const uint32_t vals = Ok(patch + 8) ? W(patch + 8) : 0;
  const uint32_t params = Ok(patch + 224) ? W(patch + 224) : 0;
  const uint32_t table = Ok(patch + 232) ? W(patch + 232) : 0;
  const uint32_t xform = Ok(patch + 220) ? W(patch + 220) : 0;
  std::string recs = "[";
  if (Heap(table) && Ok(table, 120)) {
    for (uint32_t v = 0; v < 5; ++v) {
      const uint32_t rec = W(table + 24 * v + 8);
      const uint32_t n = pmem[table + 24 * v + 20];
      recs += fmt::format("{}{{\"voice\":{},\"rec\":\"{:08X}\",\"n\":{},\"words\":{}}}", v ? "," : "", v,
                          rec, n, Heap(rec) && n <= 12 ? Hex(rec, int(8 * n)) : std::string("\"\""));
    }
  }
  recs += "]";
  return fmt::format(
      "\"values_ptr\":\"{:08X}\",\"values\":{},\"params_ptr\":\"{:08X}\",\"params\":{},\"table\":{},"
      "\"plugrecs\":{},\"patch_words\":{},\"p_ec\":{:.6g},\"p_f0\":{:.6g},\"p_f4\":{:.6g},"
      "\"xform_ptr\":\"{:08X}\",\"xform\":{}",
      vals, Heap(vals) ? Hex(vals, 20) : std::string("\"\""), params,
      Heap(params) ? Hex(params, 224) : std::string("\"\""),
      Heap(table) ? Hex(table, 30) : std::string("\"\""), recs, Hex(patch, 128),
      Ok(patch + 0xEC, 12) ? Fv(patch + 0xEC) : 0.0, Ok(patch + 0xEC, 12) ? Fv(patch + 0xF0) : 0.0,
      Ok(patch + 0xEC, 12) ? Fv(patch + 0xF4) : 0.0, xform,
      Heap(xform) ? Hex(xform, 48) : std::string("\"\""));
}

struct ZPending {
  uint64_t id;
  uint32_t patch, voice;
  double due_ms;
  int stage;
};
std::mutex zpend_m;
std::deque<ZPending> zpending;

void ZoomSnapshot(const ZPending& p) {
  if (!Ok(p.patch, 512) || W(p.patch) != 0x82A16518u || !IsZoomPatch(p.patch)) {
    Emit(fmt::format("{{\"type\":\"zsnap\",\"id\":{},\"stage_ms\":{},\"t_ms\":{:.1f},\"gone\":true}}", p.id,
                     p.stage, Ms()));
    return;
  }
  Emit(fmt::format("{}\"id\":{},\"stage_ms\":{},\"patch\":\"{:08X}\",\"voice\":\"{:08X}\",\"voice_words\":{},{}}}",
                   Head("zsnap"), p.id, p.stage, p.patch, p.voice, Hex(p.voice, 32), ZoomState(p.patch)));
}

// Real play starts. voice+64 is a float play counter that goes up by one each time the sampler
// starts the voice (1, 2, 3 ... over the life of a patch instance), so every change is one play;
// no per-variant dedupe. Called from the gain calculation, once per voice per audio update.
void ZoomVoice(uint32_t r1, uint32_t def, uint32_t patch, uint32_t voice, double f30, double f31) {
  if (!Ok(voice, 128)) return;
  const uint32_t count_bits = W(voice + 64);
  static std::mutex m;
  static std::unordered_map<uint64_t, uint32_t> last;
  static uint64_t next_id = 0;
  uint64_t id;
  bool first;
  {
    const uint64_t key = uint64_t(patch) << 32 | voice;
    std::lock_guard<std::mutex> lock(m);
    auto it = last.find(key);
    first = it == last.end();
    if (!first && it->second == count_bits) return;
    if (last.size() > 50000) last.clear();
    last[key] = count_bits;
    id = ++next_id;
  }
  if (!Budget()) return;
  uint64_t pos_bits = uint64_t(W(voice + 16)) << 32 | W(voice + 20);
  uint64_t len_bits = uint64_t(W(voice + 24)) << 32 | W(voice + 28);
  double pos, len;
  std::memcpy(&pos, &pos_bits, 8);
  std::memcpy(&len, &len_bits, 8);
  Emit(fmt::format(
      "{}\"id\":{},\"first_seen\":{},\"patch\":\"{:08X}\",\"def\":\"{:08X}\",\"voice\":\"{:08X}\","
      "\"entry\":\"{:08X}\",\"wave\":\"{:08X}\",\"count\":{:.6g},\"pos_s\":{:.6g},\"seg_s\":{:.6g},"
      "\"gain_f30\":{:.6g},\"gain_f31\":{:.6g},\"stack\":{},\"voice_words\":{},\"def_words\":{},{}}}",
      Head("play"), id, first ? "true" : "false", patch, def, voice, W(voice + 48), W(voice + 40),
      Fv(voice + 64), pos, len, f30, f31, GuestStack(r1), Hex(voice, 32), Hex(def, 41),
      ZoomState(patch)));
  std::lock_guard<std::mutex> lock(zpend_m);
  const double now = Ms();
  for (int st : {30, 100, 250}) zpending.push_back({id, patch, voice, now + st, st});
}

// Zoom snapshots that are due, and edges of the host zoom inputs (5 ms resolution).
void ZoomPoll() {
  std::vector<ZPending> due;
  {
    std::lock_guard<std::mutex> lock(zpend_m);
    const double now = Ms();
    for (auto it = zpending.begin(); it != zpending.end();) {
      if (it->due_ms <= now) {
        due.push_back(*it);
        it = zpending.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const ZPending& p : due)
    if (Budget()) ZoomSnapshot(p);
  static int last_rmb = -1, last_lt = -1;
  const int rmb = Rmb() ? 1 : 0;
  const int lt_raw = LeftTrigger();
  const int lt = lt_raw > 30 ? 1 : 0;
  if (rmb != last_rmb || lt != last_lt) {
    last_rmb = rmb;
    last_lt = lt;
    Emit(fmt::format("{{\"type\":\"input\",\"t_ms\":{:.1f},\"wall\":\"{}\",\"ac_frame\":{},\"rmb\":{},"
                     "\"lt\":{},\"lt_pressed\":{}}}",
                     Ms(), WallClock(), Ao2dcAcFrame(), rmb, lt_raw, lt));
  }
}

void Init() {
  static std::once_flag once;
  std::call_once(once, [] {
    pmem = rex::Runtime::instance()->memory()->TranslateVirtual(0);
    t0 = std::chrono::steady_clock::now();
    for (const char* dll : {"xinput1_4.dll", "xinput9_1_0.dll"}) {
      if (HMODULE h = LoadLibraryA(dll)) {
        xinput_get = reinterpret_cast<XInputGetStateFn>(GetProcAddress(h, "XInputGetState"));
        if (xinput_get) break;
      }
    }
    std::thread(SnapThread).detach();
    REXLOG_INFO("AO2DC ping capture: writing logs/ping_capture.jsonl");
  });
}

// One zoom request from the game (the callers of sub_82F5D2E0). site "A" = 0x82FF7F28 in
// sub_82FF7D80 (weapon+468 zoom level changed against owner+684); site "B" = 0x83074094 in
// sub_83073FC0 (a weapon component's zoom flag changed against owner+160).
void ZoomSet(const char* site, uint32_t r1, uint32_t snd, uint32_t value, uint32_t owner, uint32_t other) {
  sp_until_ms.store(Ms() + 700.0, std::memory_order_relaxed);
  Ao2dcCentreRequest(300000000ull);  // capture window = request -200 ms .. +500 ms
  if (!Budget()) return;
  static std::mutex m;
  static std::unordered_set<uint64_t> seen;
  bool first;
  {
    std::lock_guard<std::mutex> lock(m);
    first = seen.size() < 4096 && seen.insert(uint64_t(owner) << 32 | snd).second;
  }
  std::string extra;
  if (first)
    extra = fmt::format(",\"owner_words\":{},\"snd_words\":{},\"other_words\":{}", Hex(owner, 192),
                        Hex(snd, 20), Hex(other, 128));
  Emit(fmt::format("{}\"site\":\"{}\",\"snd\":\"{:08X}\",\"value\":{},\"owner\":\"{:08X}\","
                   "\"other\":\"{:08X}\",\"snd_fire\":\"{:08X}\",\"snd_foley\":\"{:08X}\",\"stack\":{}{}}}",
                   Head("zoomset"), site, snd, value, owner, other, Ok(snd, 64) ? W(snd + 0x14) : 0,
                   Ok(snd, 64) ? W(snd + 0x1C) : 0, GuestStack(r1), extra));
}
}  // namespace

// 0x82FF7F28 (before "bl sub_82F5D2E0"): r3 = weapon sound state, r4 = new zoom level,
// r20 = weapon, r28 = owner (cached level at +684).
void Ao2dcPingZoomA(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r20,
                    PPCRegister& r28) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  Init();
  ZoomSet("A", r1.u32, r3.u32, r4.u32, r28.u32, r20.u32);
}

// 0x83074094 (before "bl sub_82F5D2E0"): r3 = weapon sound state, r4 = new zoom flag,
// r30 = owner (cached flag at +160), r31 = the weapon component whose flag changed.
void Ao2dcPingZoomB(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r30,
                    PPCRegister& r31) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  Init();
  ZoomSet("B", r1.u32, r3.u32, r4.u32, r30.u32, r31.u32);
}

// Entry of sub_82F5D030 (posts the pending foley event to the foley sound at +0x1C): r3 = weapon
// sound state; +0x30 = event kind (1 = zoom), +0x34 = argument (zoom: 0 = ZoomOut, else ZoomIn).
void Ao2dcPingFoley(PPCRegister& r1, PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  Init();
  const uint32_t s = r3.u32;
  if (!Ok(s, 64) || W(s + 0x30) != 1 || !Budget()) return;
  const uint32_t foley = W(s + 0x1C);
  Emit(fmt::format("{}\"snd\":\"{:08X}\",\"kind\":{},\"arg\":{},\"event\":\"{}\",\"snd_foley\":\"{:08X}\","
                   "\"flags\":\"{:02X} {:02X}\",\"snd_words\":{},\"foley_words\":{},\"stack\":{}}}",
                   Head("foley"), s, W(s + 0x30), W(s + 0x34),
                   !foley ? "none (no foley sound)" : W(s + 0x34) ? "ZoomIn" : "ZoomOut", foley,
                   pmem[s + 0x3C], pmem[s + 0x3D], Hex(s, 20), Heap(foley) ? Hex(foley, 32) : std::string("\"\""),
                   GuestStack(r1.u32)));
}

// Entry of sub_82E39440 (emitter update): r3 = emitter. +0x60/+0x64 = source entries (0xB0 bytes
// each), +0x70 = definition, +0x78 = patch instance, +0x84..+0x8C distance values. The patch's
// +0xEC/+0xF0/+0xF4 read here are the results of the previous update.
void Ao2dcPingEmitter(PPCRegister& r1, PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  Init();
  const uint32_t em = r3.u32;
  if (!Ok(em, 256)) return;
  const uint32_t patch = W(em + 0x78);
  if (!Ok(patch, 512) || !IsZoomPatch(patch) || !Budget()) return;
  const uint32_t src0 = W(em + 0x60), src1 = W(em + 0x64);
  const uint32_t n = Heap(src0) && src1 >= src0 ? (src1 - src0) / 0xB0 : 0;
  static std::mutex m;
  static std::unordered_set<uint32_t> seen;
  bool first;
  {
    std::lock_guard<std::mutex> lock(m);
    first = seen.size() < 4096 && seen.insert(patch).second;
  }
  const uint32_t xform = W(patch + 220);
  Emit(fmt::format(
      "{}\"emitter\":\"{:08X}\",\"patch\":\"{:08X}\",\"def\":\"{:08X}\",\"sources\":{},"
      "\"e_84\":{:.6g},\"e_88\":{:.6g},\"e_8c\":{:.6g},\"p_ec\":{:.6g},\"p_f0\":{:.6g},\"p_f4\":{:.6g},"
      "\"emitter_words\":{},\"source0\":{},\"xform_ptr\":\"{:08X}\",\"xform\":{}{}}}",
      Head("emitter"), em, patch, W(em + 0x70), n, Fv(em + 0x84), Fv(em + 0x88), Fv(em + 0x8C),
      Fv(patch + 0xEC), Fv(patch + 0xF0), Fv(patch + 0xF4), Hex(em, 64),
      n ? Hex(src0, 44) : std::string("\"\""), xform, Heap(xform) ? Hex(xform, 48) : std::string("\"\""),
      first ? fmt::format(",\"stack\":{},\"def_words\":{},\"audio\":{}", GuestStack(r1.u32),
                          Hex(W(em + 0x70), 64), Sys())
            : std::string()));
}

// Entry of sub_82E218B8 (audio system per-frame update): r3 = audio system object.
void Ao2dcPingAudioSys(PPCRegister& r3) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  audio_sys.store(r3.u32, std::memory_order_relaxed);
}

// Called from Ao2dcGainCalc (0x82E2B41C / 0x82E2B44C in sub_82E2A040): r1 = stack, r14 = instance
// context, r16 = definition, r20 = patch instance, r31 = voice, f30/f31 = gains.
void Ao2dcPingCapture(uint32_t r1, uint32_t ctxp, uint32_t def, uint32_t patch, uint32_t voice,
                      double f30, double f31) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  Init();
  if (IsZoomPatch(patch)) ZoomVoice(r1, def, patch, voice, f30, f31);
  const float g = float(f31);
  const bool loud = g >= 2.5f || float(f30) >= 2.5f;
  const bool quiet = std::fabs(g - 0.35f) < 0.004f;
  if (!loud && !quiet) return;
  if (!Ok(voice, 128)) return;
  const uint32_t entry = W(voice + 48);
  static std::mutex m;
  static std::unordered_set<uint64_t> seen;
  static uint64_t next_id = 0;
  static int quiet_n = 0;
  uint64_t id;
  {
    const uint64_t key = (uint64_t(patch) << 32 | voice) * 1000003ull ^ entry;
    std::lock_guard<std::mutex> lock(m);
    if (!seen.insert(key).second) return;
    if (quiet && !loud) {
      if (quiet_n >= REXCVAR_GET(ao2dc_ping_capture_quiet_max)) return;
      ++quiet_n;
    }
    id = ++next_id;
  }
  const uint32_t bank = W(voice + 40);
  const uint32_t vals = Ok(patch + 8) ? W(patch + 8) : 0;
  Emit(fmt::format(
      "{{\"type\":\"start\",\"id\":{},\"t_ms\":{:.1f},\"wall\":\"{}\",\"ac_frame\":{},\"thread\":{},"
      "\"gain_f30\":{:.6g},\"gain_f31\":{:.6g},\"ctx\":\"{:08X}\",\"def\":\"{:08X}\",\"patch\":\"{:08X}\","
      "\"voice\":\"{:08X}\",\"entry\":\"{:08X}\",\"bank\":\"{:08X}\",\"stack\":{},\"def_words\":{},"
      "\"voice_words\":{},\"entry_words\":{},\"bank_words\":{},\"ctx_words\":{},\"patch_words\":{},"
      "\"values_ptr\":\"{:08X}\",\"values\":{},\"plugins\":{},\"patch_objs\":{},\"def_objs\":{},"
      "\"audio\":{}}}",
      id, Ms(), WallClock(), Ao2dcAcFrame(), GetCurrentThreadId(), f30, f31, ctxp, def, patch,
      voice, entry, bank, GuestStack(r1), Hex(def, 64), Hex(voice, 64), Hex(entry, 32),
      Hex(bank, 32), Hex(ctxp, 32), Hex(patch, 128), vals,
      Heap(vals) ? Hex(vals, 192) : std::string("\"\""), Plugins(patch), Pointed(patch, 64, 16),
      Pointed(def, 64, 8), Sys()));
  std::lock_guard<std::mutex> lock(pend_m);
  const double now = Ms();
  for (int st : {50, 200, 500, 1000}) pending.push_back({id, patch, voice, entry, now + st, st});
}

// Entry of sub_82E2CFC0 (parameter staging): r1 = stack, r3 = patch instance. Records the first
// guest stack seen for each patch instance (and the thread), to separate game-thread parameter
// writes from the audio update.
void Ao2dcPingStage(uint32_t r1, uint32_t patch, uint32_t ref, uint32_t index, double value) {
  if (!REXCVAR_GET(ao2dc_ping_capture)) return;
  Init();
  // Every staged parameter write to a zoom foley patch instance (ref = kind, voice, plugin).
  if (IsZoomPatch(patch) && Budget()) {
    const uint8_t* zr = Ok(ref, 4) ? pmem + ref : nullptr;
    Emit(fmt::format("{{\"type\":\"zstage\",\"t_ms\":{:.1f},\"ac_frame\":{},\"thread\":{},\"patch\":\"{:08X}\","
                     "\"ref\":\"{:02X} {:02X} {:02X} {:02X}\",\"index\":{},\"value\":{:.6g}}}",
                     Ms(), Ao2dcAcFrame(), GetCurrentThreadId(), patch, zr ? zr[0] : 0, zr ? zr[1] : 0,
                     zr ? zr[2] : 0, zr ? zr[3] : 0, index, value));
  }
  static std::mutex m;
  static std::unordered_set<uint64_t> seen;
  {
    const uint64_t key = (uint64_t(patch) << 32) ^ (uint64_t(GetCurrentThreadId()) << 8);
    std::lock_guard<std::mutex> lock(m);
    if (seen.size() > 20000 || !seen.insert(key).second) return;
  }
  const uint8_t* rf = Ok(ref, 4) ? pmem + ref : nullptr;
  Emit(fmt::format(
      "{{\"type\":\"stage\",\"t_ms\":{:.1f},\"ac_frame\":{},\"thread\":{},\"patch\":\"{:08X}\","
      "\"ref\":\"{:02X} {:02X} {:02X}\",\"index\":{},\"value\":{:.6g},\"stack\":{}}}",
      Ms(), Ao2dcAcFrame(), GetCurrentThreadId(), patch, rf ? rf[0] : 0, rf ? rf[1] : 0,
      rf ? rf[2] : 0, index, value, GuestStack(r1)));
}

// Exits of the SndPlayer process function sub_83876660 (called from the hooks in
// mix_writer_probe.cpp): exit 0 = returned 0 (no data), 1 = 0x838768A8, 2 = 0x83876CD4 (both
// return 1). Written for every player for 700 ms after each zoom request, so clean and burst play
// starts can be compared. player: +0x1A0..+0x1DF state (decoder handle +0x1A8, position +0x1B4,
// rate +0x1CC, block size +0x1D0, segment index +0x1D9, slot ring read/write +0x1DE/+0x1DD),
// slots at +0x70 + 16k (state byte +0x75). Segment = player + u16(player+0x1D4) + 48 * index:
// +0x00 start time (double), +0x08 data, +0x10 rate, +0x14 length, +0x1C skip, +0x20/+0x24
// position base, +0x2E state, +0x2F channels.
namespace {
// EA-XAS blocks decoded on this thread since the last SndPlayer exit (they belong to the player
// that exits next): input address, the 76 input bytes, output peak and first samples.
struct XasBlock {
  uint32_t in;
  uint8_t bytes[76];
  float peak;
  float first[4];
};
thread_local XasBlock tl_xas[8];
thread_local int tl_xas_n = 0;
}  // namespace

// Exit of the EA-XAS block decoder sub_8388FE88 (called from mix_writer_probe.cpp).
void Ao2dcPingXas(uint32_t in, uint32_t out) {
  if (!REXCVAR_GET(ao2dc_ping_capture) || !pmem) return;
  if (Ms() > sp_until_ms.load(std::memory_order_relaxed)) return;
  if (tl_xas_n >= 8 || !Ok(in, 76) || !Ok(out, 512)) return;
  XasBlock& b = tl_xas[tl_xas_n++];
  b.in = in;
  std::memcpy(b.bytes, pmem + in, 76);
  b.peak = 0;
  for (int i = 0; i < 128; ++i) {
    const float v = float(Fv(out + 4 * i));
    if (i < 4) b.first[i] = v;
    b.peak = std::max(b.peak, std::fabs(v));
  }
}

void Ao2dcPingSndPlayer(uint32_t player, int exit_id) {
  const int nx = tl_xas_n;
  tl_xas_n = 0;
  if (!REXCVAR_GET(ao2dc_ping_capture) || !pmem) return;
  if (Ms() > sp_until_ms.load(std::memory_order_relaxed)) return;
  if (!Ok(player, 0x1E0) || sp_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
  const uint32_t idx = pmem[player + 0x1D9];
  const uint32_t seg = player + rex::memory::load_and_swap<uint16_t>(pmem + player + 0x1D4) + 48 * idx;
  std::string slots;
  for (int k = 0; k < 20; ++k) slots += char('0' + std::min<int>(pmem[player + 0x75 + 16 * k], 9));
  // At the start of a play (position below 4500 samples): the slot table (20 x 16 bytes at +0x70)
  // and the XAS blocks just decoded for this player.
  std::string extra;
  if (W(player + 0x1B4) < 4500u) {
    extra = fmt::format(",\"slot_words\":{},\"xas\":[", Hex(player + 0x70, 80));
    for (int i = 0; i < nx; ++i) {
      const XasBlock& b = tl_xas[i];
      std::string hex;
      for (uint8_t c : b.bytes) hex += fmt::format("{:02X}", c);
      extra += fmt::format("{}{{\"in\":\"{:08X}\",\"bytes\":\"{}\",\"peak\":{:.4g},\"first\":[{:.4g},{:.4g},{:.4g},{:.4g}]}}",
                           i ? "," : "", b.in, hex, b.peak, b.first[0], b.first[1], b.first[2], b.first[3]);
    }
    extra += "]";
  }
  Emit(fmt::format("{{\"type\":\"sndp\",\"t_ms\":{:.1f},\"ac_frame\":{},\"thread\":{},\"player\":\"{:08X}\","
                   "\"exit\":{},\"segidx\":{},\"seg\":\"{:08X}\",\"slots\":\"{}\",\"state\":{},"
                   "\"seg_words\":{}{}}}",
                   Ms(), Ao2dcAcFrame(), GetCurrentThreadId(), player, exit_id, idx, seg, slots,
                   Hex(player + 0x1A0, 16), Ok(seg, 48) ? Hex(seg, 12) : std::string("\"\""), extra));
}
