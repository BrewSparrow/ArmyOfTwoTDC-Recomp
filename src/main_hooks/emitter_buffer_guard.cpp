// emitter_buffer_guard.cpp - keep a sound emitter's scratch text buffer alive until the sound instance has used it.
//
// Evidence (Chapter 18 stream open, run c18-x7 with the ring probes): sub_82E59620 builds a request's
// "fb:<guid>,1,<size>,<path>" text into the emitter's 512-byte scratch buffer ([emitter+36]) and gives the request
// object a pointer to it. sub_82E29390 then stores that raw pointer (no copy, no refcount) in a sound instance's
// property list via sub_82E37510. The emitter's destructor sub_82E55868 frees the buffer (it also nulls
// [emitter+36]); the traced free came from that destructor 0.03 ms after the property was stored, on the same
// thread, and the free list link then overwrote the first 8 bytes of the text. The per-frame update sub_82E385F8
// later copied that dangling pointer into the audio command, so the stream open arrived with a corrupt path.
//
// Fix: the destructor's free of the buffer (sub_82D68DF0(allocator, buffer) at 0x82E558DC) is deferred. The buffer
// goes into a FIFO; when it must shrink the oldest buffer is freed in its place. Nothing leaks (every
// buffer is freed after 64 later destructions and 250 ms, or 256 destructions) and the text stays intact long enough for the update.
// Turn it off with ao2dc_guard_emitter_buffer = false.
//
// Declared by codegen from the [[midasm_hook]] entry in config/ao4_main_functions_tu1.toml.

#include <chrono>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_guard_emitter_buffer, true, "Game",
                    "Delay the free of a sound emitter's scratch text buffer so a sound instance that still points at "
                    "it reads intact text (see src/main_hooks/emitter_buffer_guard.cpp)");

namespace {
// Hold at least kMinDepth buffers, and keep each one at least kMinHoldUs (one 30 FPS frame is 33 ms; the update that
// reads the text runs within a frame or two) as long as fewer than kMax are held. A burst of more than kMax
// destructions inside the hold time falls back to freeing the oldest (the unguarded behaviour) and is logged.
constexpr uint32_t kMinDepth = 64, kMax = 256;
constexpr int64_t kMinHoldUs = 250000;
struct Held { uint32_t p; int64_t us; };
std::mutex g_mu;
Held g_q[kMax];
uint32_t g_head = 0, g_count = 0;
uint32_t g_deferred = 0, g_young = 0;
int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

// True while `p` is held back by the guard (used by the request diagnostics).
bool Ao2dcEmitterBufferHeld(uint32_t p) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < g_count; ++i)
    if (g_q[(g_head + i) % kMax].p == p) return true;
  return false;
}

// sub_82E55868 at 0x82E558DC (the `bl sub_82D68DF0` that frees [this+36]): r4 = the buffer (non-null here).
// Returns true to skip the call; otherwise r4 now holds the oldest held buffer, which the call frees instead.
bool Ao2dcDeferEmitterBufferFree(PPCRegister& r4) {
  if (!REXCVAR_GET(ao2dc_guard_emitter_buffer))
    return false;
  std::lock_guard<std::mutex> lk(g_mu);
  const int64_t now = NowUs();
  if (++g_deferred <= 5)
    REXLOG_INFO("Emitter scratch buffer 0x{:08X} free deferred (guard, occurrence {})", r4.u32, g_deferred);
  if (g_count < kMax && (g_count < kMinDepth || now - g_q[g_head].us < kMinHoldUs)) {
    g_q[(g_head + g_count++) % kMax] = {r4.u32, now};
    return true;
  }
  const Held oldest = g_q[g_head];
  if (now - oldest.us < kMinHoldUs && ++g_young <= 5)
    REXLOG_WARN("Emitter buffer guard: queue of {} full, freeing buffer 0x{:08X} held only {} ms (occurrence {})", kMax,
                oldest.p, (now - oldest.us) / 1000, g_young);
  g_q[g_head] = {r4.u32, now};
  g_head = (g_head + 1) % kMax;
  r4.u32 = oldest.p;
  return false;
}
