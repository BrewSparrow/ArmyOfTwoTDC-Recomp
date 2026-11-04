// stack_sampler.cpp - periodic dump of every thread's call stack in guest terms (diagnostics).
//
// Every AO2DC_SAMPLE_SECONDS seconds (unset or 0: off) a host thread suspends each other
// thread, unwinds its x64 stack and logs it to logs/stacks.txt. Frames inside this DLL are named
// after the nearest recompiled guest function (from PPCFuncMappings); other frames are given as
// module+offset. Used to see what the game waits on when it stops making progress.
// Also installs an unhandled-exception filter that writes the crashing thread's stack, named the
// same way, to logs/crash.txt.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "ao2dc_pch.h"

extern PPCFuncMapping PPCFuncMappings[];

namespace {

constexpr int kMaxFrames = 48;

struct Sample {
  DWORD thread_id;
  int count;
  DWORD64 frames[kMaxFrames];
};

std::vector<std::pair<uintptr_t, uint32_t>> g_hostToGuest;  // sorted by host address
uintptr_t g_moduleBegin = 0, g_moduleEnd = 0;

void BuildMap() {
  for (auto* m = PPCFuncMappings; m->host; ++m)
    g_hostToGuest.emplace_back(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));
  std::sort(g_hostToGuest.begin(), g_hostToGuest.end());
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(&BuildMap), &self);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(self) + dos->e_lfanew);
  g_moduleBegin = reinterpret_cast<uintptr_t>(self);
  g_moduleEnd = g_moduleBegin + nt->OptionalHeader.SizeOfImage;
}

// Walks a suspended thread's stack. No allocation here: the thread may hold the heap lock.
int Unwind(CONTEXT ctx, DWORD64* frames) {
  int count = 0;
  while (count < kMaxFrames && ctx.Rip) {
    frames[count++] = ctx.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!fn) {
      // Leaf function: the return address is on top of the stack.
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher,
                     nullptr);
  }
  return count;
}

void Describe(DWORD64 address, char* out, size_t size) {
  if (address >= g_moduleBegin && address < g_moduleEnd && !g_hostToGuest.empty()) {
    auto it = std::upper_bound(g_hostToGuest.begin(), g_hostToGuest.end(),
                               std::make_pair(uintptr_t(address), uint32_t(0xFFFFFFFF)));
    if (it != g_hostToGuest.begin()) {
      --it;
      std::snprintf(out, size, "sub_%08X+0x%llX", it->second,
                    static_cast<unsigned long long>(address - it->first));
      return;
    }
  }
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, name, sizeof(name));
  }
  const char* base_name = std::max(std::strrchr(name, '\\'), static_cast<char*>(name - 1)) + 1;
  std::snprintf(out, size, "%s+0x%llX", base_name,
                static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
}

void SampleAll(FILE* file, int index) {
  std::vector<DWORD> ids;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
      ids.push_back(entry.th32ThreadID);
  }
  CloseHandle(snapshot);

  std::vector<Sample> samples(ids.size());
  size_t n = 0;
  for (DWORD id : ids) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!thread)
      continue;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &ctx)) {
        samples[n].thread_id = id;
        samples[n].count = Unwind(ctx, samples[n].frames);
        ++n;
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }

  std::fprintf(file, "==== sample %d (%zu threads)\n", index, n);
  char text[MAX_PATH + 64];
  for (size_t i = 0; i < n; ++i) {
    std::fprintf(file, "-- thread %lu\n", samples[i].thread_id);
    for (int f = 0; f < samples[i].count; ++f) {
      Describe(samples[i].frames[f], text, sizeof(text));
      std::fprintf(file, "   %s\n", text);
    }
  }
  std::fflush(file);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  static DWORD64 frames[kMaxFrames];
  if (g_hostToGuest.empty())
    BuildMap();
  int count = Unwind(*info->ContextRecord, frames);
  if (FILE* file = std::fopen("logs/crash.txt", "w")) {
    char text[MAX_PATH + 64];
    Describe(DWORD64(info->ExceptionRecord->ExceptionAddress), text, sizeof(text));
    std::fprintf(file, "exception 0x%08lX at %s, thread %lu\n", info->ExceptionRecord->ExceptionCode, text,
                 GetCurrentThreadId());
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
      std::fprintf(file, "access %s 0x%llX\n", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                   static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    for (int f = 0; f < count; ++f) {
      Describe(frames[f], text, sizeof(text));
      std::fprintf(file, "   %s\n", text);
    }
    std::fclose(file);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Starter {
  Starter() {
    SetUnhandledExceptionFilter(CrashFilter);
    int seconds = 0;
    if (const char* env = std::getenv("AO2DC_SAMPLE_SECONDS"))
      seconds = std::atoi(env);
    if (seconds <= 0)
      return;
    std::thread([seconds] {
      BuildMap();
      FILE* file = std::fopen("logs/stacks.txt", "w");
      if (!file)
        return;
      for (int i = 0;; ++i) {
        Sleep(DWORD(seconds) * 1000);
        SampleAll(file, i);
      }
    }).detach();
  }
} g_starter;

}  // namespace
