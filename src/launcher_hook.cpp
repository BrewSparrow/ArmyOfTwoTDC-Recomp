// launcher_hook.cpp - sends default.xex's launcher straight to AO4.Main.xex.
//
// sub_82011520 in default.xex builds the engine module name "AO4.<Splash|Main>.xex" and loads
// it: the byte at this+8 selects "Main" (non-zero, string at 0x82000744) or "Splash" (zero,
// 0x8200073C). Splash and Main share the image base 0x82A00000, so only Main is recompiled;
// Main has its own front end (title, menus), so the game runs without Splash.
//
// sub_82011520 overrides the weak recompiled symbol; __imp__sub_82011520 is the original.

#include <rex/hook.h>
#include <rex/logging.h>

#include "ao2dc_pch.h"

REX_EXTERN(__imp__sub_82011520);

REX_HOOK_RAW(sub_82011520) {
  if (REX_LOAD_U8(ctx.r3.u32 + 8) == 0) {
    REXLOG_INFO("Launcher: loading AO4.Main.xex instead of AO4.Splash.xex");
    REX_STORE_U8(ctx.r3.u32 + 8, 1);
  }
  __imp__sub_82011520(ctx, base);
}
