// stale_bind_guard.cpp - keep the renderer from calling through a freed texture object.
//
// Evidence (Chapter 35 "Rooftop Rampage", Narco Hell, reproduced repeatedly): a render job on a worker
// thread reaches sub_82E73770 (binds a texture object into a shader slot, called from the material
// parameter step sub_82E9FF88). The object it is handed has been freed: its first word is the heap
// free list's "next" pointer (a 0x6Dxxxxxx heap address) instead of a vtable, so the virtual call
// `object->vtable[14]` (offset 56) lands on 0x1A200154 and the runtime stops with "Call to invalid or
// unregistered function". The 8-byte-offset layout of the block is intact, which is what a freed block
// that is still referenced from a material's parameter table looks like.
//
// This mid-asm hook runs at the top of sub_82E73770 (after `mr r29,r5`). A live object always starts
// with a vtable pointer into the game image; if the first word is anything else the object is treated
// as "no texture" (r29 = 0), which takes the function's existing null path (the engine's default
// texture). Live objects are untouched. Turn the guard off with ao2dc_guard_stale_bind = false.
//
// Compiled into the AO4_Main module target (see CMakeLists.txt); declared by codegen from the
// [[midasm_hook]] entry in config/ao4_main_functions_tu1.toml (Title Update 1 addresses).

#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

REXCVAR_DEFINE_BOOL(ao2dc_guard_stale_bind, true, "Game",
                    "Treat a freed texture object in the renderer's bind step as no texture instead of "
                    "crashing (see src/main_hooks/stale_bind_guard.cpp)");

namespace {
bool InImage(uint32_t a) {
  return (a >= 0x82000000u && a < 0x84400000u) || (a >= 0x89000000u && a < 0x89100000u);
}

// Code ranges (Title Update 1: AO4.Main and Engine.BuildInfo): where a virtual method can live.
bool InMainCode(uint32_t a) {
  return ((a >= 0x82CB0000u && a < 0x84060260u) || (a >= 0x89010000u && a < 0x8901FD63u)) &&
         (a & 3u) == 0;
}
}  // namespace

// sub_82E73770 (TU1) at 0x82E73794: r29 = the texture object handed to the bind.
void Ao2dcGuardStaleBind(PPCRegister& r29) {
  const uint32_t obj = r29.u32;
  if (obj < 0x10000u || obj >= 0xA0000000u || !REXCVAR_GET(ao2dc_guard_stale_bind))
    return;
  auto* mem = rex::Runtime::instance()->memory();
  const uint32_t first = rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual(obj));
  if (InImage(first)) {
    // A freed block can be reused by an object whose first word is a code address (seen in a
    // 60 FPS test: first word 0x83552090 was code, so vtable[14] was an instruction word and the
    // call crashed). The bind calls vtable[14] (offset 56): require that to be a code address.
    const uint32_t method = rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual(first + 56u));
    if (InMainCode(method))
      return;
  }
  static std::atomic<uint32_t> seen{0};
  const uint32_t n = seen.fetch_add(1) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)  // the first few, then each power of two
    REXLOG_WARN("Stale texture bound: object 0x{:08X} starts with 0x{:08X}, not a vtable; using the default texture (occurrence {})",
                obj, first, n);
  r29.u64 = 0;
}
