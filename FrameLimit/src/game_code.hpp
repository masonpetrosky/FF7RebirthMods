// SPDX-License-Identifier: MIT
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// What the add-on finds and changes in the game's executable, kept apart from ReShade so that tests/game_code_test.cpp
// can run the same searches on the executable mapped as an image (none of its code runs there).
//
// - Console variables: each TAutoConsoleVariable is registered by code that loads the name into rdx and the static
//   object into rcx with the very next instruction. The object keeps the pointer to the variable's values (game-thread,
//   then render-thread copy) at +0x10.
// - The engine's frame-rate limiter: UEngine::GetMaxTickRate loads t.MaxFPS's value pointer (mov rax, [rip+X]) and
//   reads it with vmovss xmm1, [rax + r9*4], then compares it with 0 (vcmpltss xmm0, xmm7, xmm1). Pointing that load at
//   a pointer of the add-on's makes the engine wait for the add-on's value; everything else still reads t.MaxFPS.
// - Frames: the engine calls each IMaxTickRateHandlerModule's HandleMaxTickRate (vtable slot 5) once per game frame.
//   The game's one handler is FStreamlineMaxTickRateHandler (its vtable found by its MSVC run-time type information);
//   a small routine in that slot counts the calls and jumps on to the game's function.
namespace framelimit::game_code {

// A section of a mapped image: where it is in memory, its size, and how much of it the file initializes.
struct Section {
  uint8_t* begin = nullptr;
  size_t size = 0;
  size_t initialized = 0;
  bool Contains(const void* p, size_t bytes = 1) const {
    const auto* q = static_cast<const uint8_t*>(p);
    return begin != nullptr && bytes <= size && q >= begin && q <= begin + (size - bytes);
  }
};

struct Image {
  uint8_t* base = nullptr;
  Section text, rdata, data;
  uint32_t Rva(const void* p) const { return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base); }
};

inline Image Sections(HMODULE module) {
  Image image;
  image.base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    char name[9] = {};
    memcpy(name, sections[i].Name, 8);
    Section* out = strcmp(name, ".text") == 0    ? &image.text
                   : strcmp(name, ".rdata") == 0 ? &image.rdata
                   : strcmp(name, ".data") == 0  ? &image.data
                                                 : nullptr;
    if (out == nullptr) continue;
    out->begin = image.base + sections[i].VirtualAddress;
    out->size = sections[i].Misc.VirtualSize;
    out->initialized = std::min<size_t>(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
  }
  return image;
}

// Where a RIP-relative instruction of `length` bytes, ending with its displacement, points.
inline uint8_t* RipTarget(const uint8_t* instruction, size_t length) {
  int32_t disp;
  memcpy(&disp, instruction + length - 4, 4);
  return const_cast<uint8_t*>(instruction) + length + disp;
}

// ---------------------------------------------------------------------------------------------
// Console variables

inline constexpr uint8_t kLeaRdx[3] = {0x48, 0x8D, 0x15};  // lea rdx, [rip+disp32]
inline constexpr uint8_t kLeaRcx[3] = {0x48, 0x8D, 0x0D};  // lea rcx, [rip+disp32]
inline constexpr size_t kLeaSize = 7;
inline constexpr size_t kAutoVariableData = 0x10;  // TAutoConsoleVariable: the pointer to the values

// The static TAutoConsoleVariable objects of `count` console variables, found in one pass over the code; an entry
// stays null unless exactly one registration of that exact name (with its terminator) is found.
inline void FindAutoVariables(const Image& image, const wchar_t* const* names, size_t count, uint8_t** objects) {
  std::vector<std::vector<const uint8_t*>> strings(count);
  for (size_t n = 0; n < count; ++n) {
    objects[n] = nullptr;
    const size_t bytes = (wcslen(names[n]) + 1) * sizeof(wchar_t);
    for (size_t i = 0; i + bytes <= image.rdata.initialized; i += 2)
      if (memcmp(image.rdata.begin + i, names[n], bytes) == 0) strings[n].push_back(image.rdata.begin + i);
  }
  std::vector<int> found(count, 0);
  const Section& text = image.text;
  for (size_t i = 0; i + 2 * kLeaSize <= text.initialized; ++i) {
    const uint8_t* p = text.begin + i;
    if (p[1] != 0x8D || memcmp(p, kLeaRdx, 3) != 0 || memcmp(p + kLeaSize, kLeaRcx, 3) != 0) continue;
    const uint8_t* name = RipTarget(p, kLeaSize);
    for (size_t n = 0; n < count; ++n) {
      if (std::find(strings[n].begin(), strings[n].end(), name) == strings[n].end()) continue;
      objects[n] = RipTarget(p + kLeaSize, kLeaSize);
      ++found[n];
    }
  }
  for (size_t n = 0; n < count; ++n)
    if (found[n] != 1 || !image.data.Contains(objects[n], kAutoVariableData + 8)) objects[n] = nullptr;
}

// The 7-byte loads of the qword at `field` into a 64-bit register (mov r64, [rip+disp32]).
inline std::vector<uint8_t*> FindLoads(const Image& image, const void* field) {
  std::vector<uint8_t*> loads;
  const Section& text = image.text;
  for (size_t i = 0; i + 7 <= text.initialized; ++i) {
    uint8_t* p = text.begin + i;
    if (p[1] != 0x8B || (p[0] != 0x48 && p[0] != 0x4C) || (p[2] & 0xC7) != 0x05) continue;
    if (RipTarget(p, 7) == field) loads.push_back(p);
  }
  return loads;
}

// The limiter's load of t.MaxFPS: mov rax, [rip+data pointer]; vmovss xmm1, dword ptr [rax + r9*4];
// vcmpltss xmm0, xmm7, xmm1 (0 < t.MaxFPS).
inline constexpr uint8_t kLimiterLoad[3] = {0x48, 0x8B, 0x05};
inline constexpr uint8_t kLimiterTail[11] = {0xC4, 0xA1, 0x7A, 0x10, 0x0C, 0x88, 0xC5, 0xC2, 0xC2, 0xC1, 0x01};
inline constexpr size_t kLimiterLoadSize = 7;

// The limiter's load among the loads of t.MaxFPS's value pointer, if exactly one matches.
inline uint8_t* FindLimiterLoad(const Image& image, const std::vector<uint8_t*>& loads) {
  uint8_t* found = nullptr;
  for (uint8_t* p : loads) {
    if (!image.text.Contains(p, kLimiterLoadSize + sizeof(kLimiterTail))) continue;
    if (memcmp(p, kLimiterLoad, 3) != 0 || memcmp(p + kLimiterLoadSize, kLimiterTail, sizeof(kLimiterTail)) != 0)
      continue;
    if (found != nullptr) return nullptr;
    found = p;
  }
  return found;
}

// The displacement that makes a RIP-relative instruction of `length` bytes at `instruction` point at `target`.
inline bool Displacement(const uint8_t* instruction, size_t length, const void* target, int32_t* disp) {
  const intptr_t d = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(instruction + length);
  if (d < INT32_MIN || d > INT32_MAX) return false;
  *disp = static_cast<int32_t>(d);
  return true;
}

// Rewrites the displacement of the instruction (its last 4 bytes) in the game's code.
inline bool WriteDisplacement(uint8_t* instruction, size_t length, int32_t disp) {
  uint8_t* const at = instruction + length - 4;
  DWORD old;
  if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(at, &disp, 4);
  VirtualProtect(at, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, 4);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Run-time type information

// The vtable of a class, from its MSVC run-time type information: the type descriptor (whose decorated name, such as
// ".?AVFStreamlineMaxTickRateHandler@@", starts 16 bytes into it), the complete object locator that names it (signature
// 1, offset 0, its own RVA last) and the vtable, which starts right after a pointer to that locator. Null unless each
// is found exactly once.
inline void** FindVtable(const Image& image, const char* decorated_name) {
  const size_t bytes = strlen(decorated_name) + 1;
  const uint8_t* descriptor = nullptr;
  int descriptors = 0;
  for (const Section* s : {&image.data, &image.rdata}) {
    for (size_t i = 16; i + bytes <= s->initialized; i += 8) {
      if (s->begin[i] != static_cast<uint8_t>(decorated_name[0]) || memcmp(s->begin + i, decorated_name, bytes) != 0)
        continue;
      descriptor = s->begin + i - 16;
      ++descriptors;
    }
  }
  if (descriptors != 1) return nullptr;
  const uint32_t descriptor_rva = image.Rva(descriptor);

  const Section& rdata = image.rdata;
  const uint8_t* locator = nullptr;
  int locators = 0;
  for (size_t i = 0; i + 24 <= rdata.initialized; i += 4) {
    uint32_t col[6];
    memcpy(col, rdata.begin + i, sizeof(col));
    if (col[3] != descriptor_rva || col[0] != 1 || col[1] != 0 || col[5] != image.Rva(rdata.begin + i)) continue;
    locator = rdata.begin + i;
    ++locators;
  }
  if (locators != 1) return nullptr;

  void** vtable = nullptr;
  int vtables = 0;
  const uint64_t locator_address = reinterpret_cast<uint64_t>(locator);
  for (size_t i = 0; i + 16 <= rdata.initialized; i += 8) {
    uint64_t q;
    memcpy(&q, rdata.begin + i, 8);
    if (q != locator_address) continue;
    vtable = reinterpret_cast<void**>(rdata.begin + i + 8);
    ++vtables;
  }
  return vtables == 1 ? vtable : nullptr;
}

// IMaxTickRateHandlerModule::HandleMaxTickRate: the engine calls [vtable + 0x28] once per frame.
inline constexpr size_t kHandleMaxTickRateSlot = 5;

// Replaces a vtable entry (in read-only data).
inline bool WritePointer(void** slot, void* value) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  std::atomic_ref<void*>(*slot).store(value);
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

// ---------------------------------------------------------------------------------------------
// The call counter

// A routine that counts the calls of a function and passes each on unchanged, registers and stack untouched:
//   lock inc qword ptr [rip + count]
//   jmp qword ptr [rip + original]
// On a page of its own (read/execute), with the count and the original's address on the page after it (read/write).
// Never freed: a thread could still be in it when the vtable entry is put back.
struct CallCounter {
  uint8_t* code = nullptr;
  uint64_t* count = nullptr;
  void** original = nullptr;
  uint64_t Count() const { return std::atomic_ref<uint64_t>(*count).load(std::memory_order_relaxed); }
};

inline constexpr uint8_t kLockIncRip[4] = {0xF0, 0x48, 0xFF, 0x05};  // lock inc qword ptr [rip+disp32]
inline constexpr uint8_t kJmpRip[2] = {0xFF, 0x25};                  // jmp qword ptr [rip+disp32]

inline bool MakeCallCounter(void* original, CallCounter* counter) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  auto* const code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (code == nullptr) return false;
  uint8_t* const data = code + page;
  int32_t disp;
  memcpy(code, kLockIncRip, sizeof(kLockIncRip));
  Displacement(code, 8, data, &disp);
  memcpy(code + 4, &disp, 4);
  memcpy(code + 8, kJmpRip, sizeof(kJmpRip));
  Displacement(code + 8, 6, data + 8, &disp);
  memcpy(code + 10, &disp, 4);
  memset(code + 14, 0xCC, 2);
  memcpy(data + 8, &original, sizeof(original));
  DWORD old;
  if (!VirtualProtect(code, page, PAGE_EXECUTE_READ, &old)) {
    VirtualFree(code, 0, MEM_RELEASE);
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), code, 16);
  counter->code = code;
  counter->count = reinterpret_cast<uint64_t*>(data);
  counter->original = reinterpret_cast<void**>(data + 8);
  return true;
}

// ---------------------------------------------------------------------------------------------
// NVIDIA Reflex's mode

// Each game frame, HandleMaxTickRate (the handler's slot 5) builds sl::ReflexOptions for slReflexSetOptions. Their mode
// comes from a call to a check that is true only while DLSS frame generation is available and enabled:
//   xor al, al; cmp [available], al; je end; cmp [enabled], al; je end; mov al, 1; end: ret
// Low Latency then, Off otherwise. The game's renderer sets "enabled" every frame from r.FrameGeneration.Method, and the
// same check also gates DLSS frame generation's own work, so only this one call is changed. It is followed by the other
// option stores: xor ecx, ecx; mov dword ptr [rsp+0x4C], 0 (frameLimitUs); test al, al.
inline constexpr uint8_t kModeCallTail[12] = {0x33, 0xC9, 0xC7, 0x44, 0x24, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x84, 0xC0};
inline constexpr size_t kCallSize = 5;      // E8 rel32
inline constexpr size_t kModeCallReach = 0x200;  // the call lies this close to the start of HandleMaxTickRate
inline constexpr size_t kCheckSize = 21;

// The check's code with its two RIP-relative flag reads left out (bytes 4-7 and 12-15).
inline bool IsReflexCheck(const uint8_t* p) {
  static constexpr uint8_t head[4] = {0x32, 0xC0, 0x38, 0x05};
  static constexpr uint8_t mid[4] = {0x74, 0x0A, 0x38, 0x05};
  static constexpr uint8_t tail[5] = {0x74, 0x02, 0xB0, 0x01, 0xC3};
  return memcmp(p, head, 4) == 0 && memcmp(p + 8, mid, 4) == 0 && memcmp(p + 16, tail, 5) == 0;
}

struct ReflexSite {
  uint8_t* call = nullptr;         // the call in HandleMaxTickRate
  uint8_t* check = nullptr;        // the game's check it calls
  const uint8_t* available = nullptr;  // the check's flags: DLSS frame generation available (Reflex supported)...
  const uint8_t* enabled = nullptr;    // ...and enabled
};

// The mode's call in HandleMaxTickRate (`function`), if exactly one matches and calls the check.
inline bool FindReflexModeCall(const Image& image, const uint8_t* function, ReflexSite* site) {
  if (!image.text.Contains(function, kModeCallReach + kCallSize + sizeof(kModeCallTail))) return false;
  uint8_t* found = nullptr;
  for (size_t i = 0; i < kModeCallReach; ++i) {
    const uint8_t* p = function + i;
    if (p[0] != 0xE8 || memcmp(p + kCallSize, kModeCallTail, sizeof(kModeCallTail)) != 0) continue;
    if (found != nullptr) return false;
    found = const_cast<uint8_t*>(p);
  }
  if (found == nullptr) return false;
  uint8_t* const check = RipTarget(found, kCallSize);
  if (!image.text.Contains(check, kCheckSize) || !IsReflexCheck(check)) return false;
  site->call = found;
  site->check = check;
  site->available = RipTarget(check + 2, 6);
  site->enabled = RipTarget(check + 10, 6);
  return image.data.Contains(site->available) && image.data.Contains(site->enabled);
}

// The routine the mode's call goes to instead: true (Low Latency) while `on` is set, else the game's check.
//   cmp byte ptr [rip + on], 0
//   je game
//   mov al, 1
//   ret
//   game: jmp qword ptr [rip + check]
// Code on one page (read/execute) within reach of the call, `on` and the check's address on the page after it
// (read/write). Never freed, like the call counter.
struct ReflexSwitch {
  uint8_t* code = nullptr;
  uint8_t* on = nullptr;
  void** check = nullptr;
  void Set(bool value) { std::atomic_ref<uint8_t>(*on).store(value ? 1 : 0, std::memory_order_relaxed); }
  bool Get() const { return std::atomic_ref<uint8_t>(*on).load(std::memory_order_relaxed) != 0; }
};

inline void* AllocateNear(const void* near_address, size_t size);  // below

inline bool MakeReflexSwitch(const void* near_code, void* check, bool on, ReflexSwitch* out) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  auto* const code = static_cast<uint8_t*>(AllocateNear(near_code, 2 * page));
  if (code == nullptr) return false;
  uint8_t* const data = code + page;
  int32_t disp;
  const uint8_t cmp[2] = {0x80, 0x3D};  // cmp byte ptr [rip+disp32], imm8 (7 bytes)
  memcpy(code, cmp, 2);
  Displacement(code, 7, data, &disp);
  memcpy(code + 2, &disp, 4);
  code[6] = 0x00;
  const uint8_t rest[5] = {0x74, 0x03, 0xB0, 0x01, 0xC3};  // je +3; mov al, 1; ret
  memcpy(code + 7, rest, 5);
  memcpy(code + 12, kJmpRip, sizeof(kJmpRip));  // jmp qword ptr [rip+disp32] (6 bytes)
  Displacement(code + 12, 6, data + 8, &disp);
  memcpy(code + 14, &disp, 4);
  memset(code + 18, 0xCC, 2);
  data[0] = on ? 1 : 0;
  memcpy(data + 8, &check, sizeof(check));
  DWORD old;
  if (!VirtualProtect(code, page, PAGE_EXECUTE_READ, &old)) return false;
  FlushInstructionCache(GetCurrentProcess(), code, 20);
  out->code = code;
  out->on = data;
  out->check = reinterpret_cast<void**>(data + 8);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Memory near the code

// Committed read-write memory within reach of a RIP-relative displacement from `near_address` (a little under 2 GB):
// the free region closest below it, else above it.
inline void* AllocateNear(const void* near_address, size_t size) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const uintptr_t granularity = info.dwAllocationGranularity;
  const uintptr_t center = reinterpret_cast<uintptr_t>(near_address);
  const uintptr_t reach = 0x7FF00000;
  const uintptr_t lowest = std::max<uintptr_t>(center > reach ? center - reach : 0,
                                               reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress));
  const uintptr_t highest =
      std::min<uintptr_t>(center + reach, reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress));
  MEMORY_BASIC_INFORMATION mbi;
  for (uintptr_t at = center & ~(granularity - 1); at >= lowest + granularity;) {
    at -= granularity;
    if (VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == 0) break;
    if (mbi.State == MEM_FREE) {
      if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return p;
    } else if (mbi.AllocationBase != nullptr) {
      at = reinterpret_cast<uintptr_t>(mbi.AllocationBase) & ~(granularity - 1);  // go on below that allocation
    }
  }
  for (uintptr_t at = (center + granularity) & ~(granularity - 1); at + size <= highest;) {
    if (VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == 0) break;
    if (mbi.State == MEM_FREE) {
      if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return p;
      at += granularity;
    } else {
      const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      at = (end + granularity - 1) & ~(granularity - 1);
    }
  }
  return nullptr;
}

// What the redirected load reads: a pointer to two floats (game-thread and render-thread value, as Unreal keeps a
// console variable), on one page near the code. Never freed: a thread could be between the load and the read when the
// load is put back.
struct LimitValue {
  float** pointer = nullptr;
  float* values = nullptr;
  void Set(float value) {
    std::atomic_ref<float>(values[0]).store(value, std::memory_order_relaxed);
    std::atomic_ref<float>(values[1]).store(value, std::memory_order_relaxed);
  }
  float Get() const { return std::atomic_ref<float>(values[0]).load(std::memory_order_relaxed); }
};

inline bool MakeLimitValue(const void* near_code, float initial, LimitValue* value) {
  auto* const page = static_cast<uint8_t*>(AllocateNear(near_code, 64));
  if (page == nullptr) return false;
  value->pointer = reinterpret_cast<float**>(page);
  value->values = reinterpret_cast<float*>(page + 16);
  value->Set(initial);
  *value->pointer = value->values;
  return true;
}

}  // namespace framelimit::game_code
