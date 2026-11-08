// Checks the Frame Limit add-on's searches and changes (src/game_code.hpp) on the game's executable, mapped as an image
// the way Windows maps it for the game (none of its code runs): the three console variables, the engine limiter's load
// of t.MaxFPS, its redirection to the add-on's value and back, and the Streamline handler's vtable entry. Then the call
// counter routine on a vtable of its own: arguments and result pass through, and calls from several threads are all
// counted. Also that a changed limiter is refused.
// usage: game_code_test.exe [path to ff7rebirth_.exe]
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "game_code.hpp"

namespace code = framelimit::game_code;

namespace {

int g_failures = 0;

void Expect(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; ++i) {
    snprintf(b, sizeof(b), "%02X ", p[i]);
    s += b;
  }
  return s;
}

// A stand-in for HandleMaxTickRate: rcx = the handler, xmm1 = the desired tick rate, al = the result.
struct FakeHandler {
  void** vtable;
};
FakeHandler* g_seen_self = nullptr;
float g_seen_rate = 0.0f;
bool __fastcall FakeHandleMaxTickRate(FakeHandler* self, float rate) {
  g_seen_self = self;
  g_seen_rate = rate;
  return rate > 100.0f;
}
std::atomic<uint64_t> g_fake_calls{0};
bool __fastcall CountingHandleMaxTickRate(FakeHandler*, float) {
  g_fake_calls.fetch_add(1, std::memory_order_relaxed);
  return false;
}
using HandleFn = bool(__fastcall*)(FakeHandler*, float);

double Ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
  return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1]
                              : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\FINAL FANTASY VII "
                                "REBIRTH\\End\\Binaries\\Win64\\ff7rebirth_.exe";
  HMODULE module = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (module == nullptr) {
    std::fprintf(stderr, "cannot map %s (error %lu)\n", path, GetLastError());
    return 2;
  }
  const code::Image image = code::Sections(module);
  std::printf("%s mapped at %p: .text %zu bytes, .rdata %zu, .data %zu\n", path, static_cast<void*>(image.base),
              image.text.size, image.rdata.size, image.data.size);
  LARGE_INTEGER f, t0, t1, t2, t3;
  QueryPerformanceFrequency(&f);

  // Console variables.
  QueryPerformanceCounter(&t0);
  const wchar_t* const names[3] = {L"t.MaxFPS", L"r.FrameGeneration.Method", L"rhi.SyncInterval"};
  uint8_t* objects[3];
  code::FindAutoVariables(image, names, 3, objects);
  QueryPerformanceCounter(&t1);
  for (int i = 0; i < 3; ++i)
    std::printf("      %-26ls object rva 0x%X\n", names[i], objects[i] ? image.Rva(objects[i]) : 0u);
  Expect(objects[0] && objects[1] && objects[2], "each console variable has exactly one registration");
  if (!objects[0]) return 1;
  float* const* max_fps = reinterpret_cast<float* const*>(objects[0] + code::kAutoVariableData);

  // The limiter's load.
  const std::vector<uint8_t*> loads = code::FindLoads(image, max_fps);
  for (uint8_t* p : loads) std::printf("      read of t.MaxFPS at rva 0x%X: %s\n", image.Rva(p), Hex(p, 18).c_str());
  Expect(loads.size() == 2, "t.MaxFPS's values are read in two places (GetMaxFPS, GetMaxTickRate)");
  uint8_t* const site = code::FindLimiterLoad(image, loads);
  QueryPerformanceCounter(&t2);
  Expect(site != nullptr, "exactly one of them is the limiter (vmovss xmm1, [rax + r9*4]; 0 < t.MaxFPS)");
  if (site == nullptr) return 1;
  std::printf("      limiter load at rva 0x%X\n", image.Rva(site));

  // The Streamline handler.
  void** const vtable = code::FindVtable(image, ".?AVFStreamlineMaxTickRateHandler@@");
  QueryPerformanceCounter(&t3);
  Expect(vtable != nullptr, "FStreamlineMaxTickRateHandler's vtable found from its type information");
  if (vtable != nullptr) {
    void* const handle = vtable[code::kHandleMaxTickRateSlot];
    std::printf("      vtable rva 0x%X, HandleMaxTickRate (slot %zu) rva 0x%X: %s\n", image.Rva(vtable),
                code::kHandleMaxTickRateSlot, image.Rva(handle), Hex(static_cast<uint8_t*>(handle), 12).c_str());
    Expect(image.text.Contains(handle), "the slot points into the game's code");
  }
  std::printf("      searches took %.0f ms (console variables), %.0f ms (loads), %.0f ms (vtable)\n", Ms(t0, t1, f),
              Ms(t1, t2, f), Ms(t2, t3, f));

  // Redirect the limiter's load to a value of the add-on's, and back.
  code::LimitValue value;
  Expect(code::MakeLimitValue(site, 118.5f, &value), "the add-on's value allocated near the limiter");
  int32_t disp = 0, original = 0;
  memcpy(&original, site + 3, 4);
  const bool reach = code::Displacement(site, code::kLimiterLoadSize, value.pointer, &disp);
  Expect(reach, "it is in reach of the load");
  if (reach) {
    std::printf("      value page at %p (%+lld MB from the load)\n", static_cast<void*>(value.pointer),
                static_cast<long long>((reinterpret_cast<intptr_t>(value.pointer) - reinterpret_cast<intptr_t>(site)) >> 20));
    Expect(code::WriteDisplacement(site, code::kLimiterLoadSize, disp), "the load's displacement rewritten");
    const auto* loaded = reinterpret_cast<float* const*>(code::RipTarget(site, code::kLimiterLoadSize));
    Expect(loaded == value.pointer && *loaded == value.values && (*loaded)[0] == 118.5f && (*loaded)[1] == 118.5f,
           "the redirected load reads the add-on's value (118.5 for both threads)");
    value.Set(237.0f);
    Expect((*loaded)[0] == 237.0f && (*loaded)[1] == 237.0f, "a new value is read the same way");
    Expect(code::FindLimiterLoad(image, code::FindLoads(image, max_fps)) == nullptr,
           "a redirected load is no longer found as t.MaxFPS's (no second redirection)");
    Expect(code::WriteDisplacement(site, code::kLimiterLoadSize, original) &&
               code::RipTarget(site, code::kLimiterLoadSize) == reinterpret_cast<const uint8_t*>(max_fps),
           "the load put back reads t.MaxFPS again");
  }

  // A changed limiter is refused.
  {
    DWORD old;
    uint8_t* const tail = site + code::kLimiterLoadSize + 10;  // vcmpltss's imm8 predicate: 1 = less than
    VirtualProtect(tail, 1, PAGE_EXECUTE_READWRITE, &old);
    *tail = 2;  // less or equal
    Expect(code::FindLimiterLoad(image, loads) == nullptr, "a different comparison after the load is refused");
    *tail = 1;
    VirtualProtect(tail, 1, old, &old);
    Expect(code::FindLimiterLoad(image, loads) == site, "and found again once it is the game's");
  }

  // The real vtable entry can be replaced and put back.
  if (vtable != nullptr) {
    void* const handle = vtable[code::kHandleMaxTickRateSlot];
    code::CallCounter counter;
    Expect(code::MakeCallCounter(handle, &counter) && *counter.original == handle && counter.Count() == 0,
           "a call counter made for the game's HandleMaxTickRate");
    Expect(code::WritePointer(&vtable[code::kHandleMaxTickRateSlot], counter.code) &&
               vtable[code::kHandleMaxTickRateSlot] == counter.code,
           "the vtable entry (read-only data) points at the counter");
    Expect(code::WritePointer(&vtable[code::kHandleMaxTickRateSlot], handle) &&
               vtable[code::kHandleMaxTickRateSlot] == handle,
           "and back at the game's function");
  }

  // The counter on a vtable of its own: called the way the engine calls the handler.
  {
    void* fake_vtable[8] = {};
    fake_vtable[code::kHandleMaxTickRateSlot] = reinterpret_cast<void*>(&FakeHandleMaxTickRate);
    FakeHandler handler{fake_vtable};
    code::CallCounter counter;
    Expect(code::MakeCallCounter(fake_vtable[code::kHandleMaxTickRateSlot], &counter), "a counter for a stand-in");
    code::WritePointer(&fake_vtable[code::kHandleMaxTickRateSlot], counter.code);
    const auto call = [&](float rate) {
      return reinterpret_cast<HandleFn>(handler.vtable[code::kHandleMaxTickRateSlot])(&handler, rate);
    };
    const bool high = call(118.5f), low = call(60.0f);
    Expect(high && !low && g_seen_self == &handler && g_seen_rate == 60.0f && counter.Count() == 2,
           "the handler, the rate and the result pass through, and both calls are counted");

    void* counting_vtable[8] = {};
    counting_vtable[code::kHandleMaxTickRateSlot] = reinterpret_cast<void*>(&CountingHandleMaxTickRate);
    code::CallCounter threaded;
    code::MakeCallCounter(counting_vtable[code::kHandleMaxTickRateSlot], &threaded);
    code::WritePointer(&counting_vtable[code::kHandleMaxTickRateSlot], threaded.code);
    FakeHandler h{counting_vtable};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
      threads.emplace_back([&] {
        for (int i = 0; i < 250000; ++i)
          reinterpret_cast<HandleFn>(h.vtable[code::kHandleMaxTickRateSlot])(&h, 0.0f);
      });
    for (std::thread& t : threads) t.join();
    std::printf("      %llu calls counted, %llu reached the function\n",
                static_cast<unsigned long long>(threaded.Count()),
                static_cast<unsigned long long>(g_fake_calls.load()));
    Expect(threaded.Count() == 1000000 && g_fake_calls.load() == 1000000,
           "a million calls from four threads: every one counted and passed on");
  }

  FreeLibrary(module);
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "CHECKS FAILED");
  return g_failures == 0 ? 0 : 1;
}
