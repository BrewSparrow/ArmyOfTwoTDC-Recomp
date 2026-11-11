// launcher_hook.cpp - sends default.xex's launcher straight to AO4.Main.xex.
//
// sub_82011520 in default.xex builds the engine module name "AO4.<Splash|Main>.xex" and loads
// it: the byte at this+8 selects "Main" (non-zero, string at 0x82000744) or "Splash" (zero,
// 0x8200073C). Splash and Main share the image base 0x82A00000, so only Main is recompiled;
// Main has its own front end (title, menus), so the game runs without Splash.
//
// The launcher also owns the package scan (sub_820126C8 on a thread started by sub_82012A60):
// it enumerates marketplace content (DLC) and probes UPDATE:\package.mft, so the engine learns
// the title update's and each DLC's data roots. On the console the loaded module starts it
// through the launcher interface (vtable slot 18, sub_82012C90, with the signed-in user index);
// AO4.Main never does, because Splash used to. Start it here, once, for user 0, before Main is
// loaded.
//
// sub_82011520 overrides the weak recompiled symbol; __imp__sub_82011520 is the original.

#include <rex/hook.h>
#include <rex/logging.h>

#include "ao2dc_pch.h"

REX_EXTERN(__imp__sub_82011520);
REX_EXTERN(sub_82012C90);  // launcher vtable[18]: start the package scan for a user (r4)

REX_HOOK_RAW(sub_82011520) {
  if (REX_LOAD_U8(ctx.r3.u32 + 8) == 0) {
    REXLOG_INFO("Launcher: loading AO4.Main.xex instead of AO4.Splash.xex");
    REX_STORE_U8(ctx.r3.u32 + 8, 1);
  }
  static bool scan_started = false;
  if (!scan_started) {
    scan_started = true;
    PPCContext saved = ctx;  // the call is made at this function's entry; restore everything after
    ctx.r4.u64 = 0;          // user index 0
    ctx.r5.u64 = 0;          // not a forced rescan
    sub_82012C90(ctx, base);
    REXLOG_INFO("Launcher: package scan started for user 0 (result {})", ctx.r3.u32);
    ctx = saved;
  }
  __imp__sub_82011520(ctx, base);
}
