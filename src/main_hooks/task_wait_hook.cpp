// task_wait_hook.cpp - AO4.Main's job worker waits for a task's function to be filled in.
//
// The job system's worker (around 0x83240600) claims a task with a 64-bit CAS on its state
// word, then calls the function at task+8 with the argument at task+12. Under the recompiled
// code the worker can claim a task before the creating thread's store of task+8 is visible:
// it read 0 and called address 0, while a dump moments later showed the function set
// (0x8323F850). This mid-asm hook, placed before `mtctr r11` (0x83240658), re-reads task+8 for
// a short while when it is still 0, and skips the call (jumping to the task's completion at
// 0x83240660) if it stays 0 - some tasks are queued with no function at all.
//
// Compiled into the AO4_Main module target (see CMakeLists.txt); declared by codegen from the
// [[midasm_hook]] entry in config/ao4_main_functions.toml.

#include <atomic>
#include <thread>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "ao2dc_pch.h"

bool Ao2dcWaitForTaskFunction(PPCRegister& r11, PPCRegister& r3, PPCRegister& r31) {
  if (r11.u32 != 0)
    return false;
  auto* memory = rex::Runtime::instance()->memory();
  for (int i = 0; i < 2000; ++i) {
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t fn = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(r31.u32 + 8));
    if (fn != 0) {
      r11.u64 = fn;
      r3.u64 = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(r31.u32 + 12));
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 8)
        REXLOG_WARN("Job worker: task 0x{:08X} had no function yet; got 0x{:08X} after {} retries", r31.u32,
                    fn, i + 1);
      return false;
    }
    std::this_thread::yield();
  }
  static std::atomic<int> skipped{0};
  if (skipped.fetch_add(1) < 16)
    REXLOG_WARN("Job worker: task 0x{:08X} has no function; skipping the call", r31.u32);
  return true;  // skip the call
}
