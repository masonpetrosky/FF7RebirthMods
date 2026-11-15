// The game's executable as our add-ons find and change things in it: its sections, RIP-relative instructions, writes
// into its code and data, memory of ours within reach of its code, and where its console variables are. Kept apart
// from ReShade so that tests can run the same code on the executable mapped as an image, none of its code running
// (FrameLimit/tests/game_code_test.cpp).
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
#include <cwchar>
#include <vector>

namespace ff7r::game_code {

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

// The running game's executable.
inline const Image& GameImage() {
  static const Image image = Sections(GetModuleHandleW(nullptr));
  return image;
}

// Where a RIP-relative instruction of `length` bytes, ending with its displacement, points.
inline uint8_t* RipTarget(const uint8_t* instruction, size_t length) {
  int32_t disp;
  memcpy(&disp, instruction + length - 4, 4);
  return const_cast<uint8_t*>(instruction) + length + disp;
}

// The displacement that makes a RIP-relative instruction of `length` bytes at `instruction` point at `target`.
inline bool Displacement(const uint8_t* instruction, size_t length, const void* target, int32_t* disp) {
  const intptr_t d = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(instruction + length);
  if (d < INT32_MIN || d > INT32_MAX) return false;
  *disp = static_cast<int32_t>(d);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Changes

// Writes `size` bytes into the game's code.
inline bool WriteCode(void* at, const void* bytes, size_t size) {
  DWORD old;
  if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(at, bytes, size);
  VirtualProtect(at, size, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, size);
  return true;
}

// Rewrites the displacement of the instruction (its last 4 bytes) in the game's code.
inline bool WriteDisplacement(uint8_t* instruction, size_t length, int32_t disp) {
  return WriteCode(instruction + length - 4, &disp, 4);
}

// Replaces a pointer in read-only data, such as a vtable entry.
inline bool WritePointer(void** slot, void* value) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  std::atomic_ref<void*>(*slot).store(value);
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

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

// ---------------------------------------------------------------------------------------------
// Console variables
//
// The code that registers one loads its name into rdx (lea rdx, [rip+name]). For a TAutoConsoleVariable it loads the
// static object into rcx with the very next instruction; the object keeps the pointer to the variable's values
// (game-thread, then render-thread copy) at +0x10. For an FAutoConsoleVariableRef it loads the variable's address into
// r8 shortly before (lea r8, [rip+variable]).

inline constexpr uint8_t kLeaRdx[3] = {0x48, 0x8D, 0x15};  // lea rdx, [rip+disp32]
inline constexpr uint8_t kLeaRcx[3] = {0x48, 0x8D, 0x0D};  // lea rcx, [rip+disp32]
inline constexpr uint8_t kLeaR8[3] = {0x4C, 0x8D, 0x05};   // lea r8, [rip+disp32]
inline constexpr size_t kLeaSize = 7;
inline constexpr size_t kAutoVariableData = 0x10;  // TAutoConsoleVariable: the pointer to the values
inline constexpr size_t kRefReach = 48;            // FAutoConsoleVariableRef: the lea r8 at most this far before

// The places of the exact name (with its terminator) in read-only data.
inline std::vector<const uint8_t*> FindNames(const Image& image, const wchar_t* name) {
  std::vector<const uint8_t*> strings;
  const size_t bytes = (wcslen(name) + 1) * sizeof(wchar_t);
  for (size_t i = 0; i + bytes <= image.rdata.initialized; i += 2)
    if (memcmp(image.rdata.begin + i, name, bytes) == 0) strings.push_back(image.rdata.begin + i);
  return strings;
}

// The code that loads the exact name into rdx.
inline std::vector<const uint8_t*> FindNameLoads(const Image& image, const wchar_t* name) {
  std::vector<const uint8_t*> loads;
  const std::vector<const uint8_t*> strings = FindNames(image, name);
  if (strings.empty()) return loads;
  const Section& text = image.text;
  for (size_t i = 0; i + kLeaSize <= text.initialized; ++i) {
    const uint8_t* p = text.begin + i;
    if (p[1] == 0x8D && memcmp(p, kLeaRdx, 3) == 0 &&
        std::find(strings.begin(), strings.end(), RipTarget(p, kLeaSize)) != strings.end())
      loads.push_back(p);
  }
  return loads;
}

// The static TAutoConsoleVariable objects of `count` console variables, found in one pass over the code; an entry
// stays null unless exactly one registration of that exact name is found, its object in the data section.
inline void FindAutoVariables(const Image& image, const wchar_t* const* names, size_t count, uint8_t** objects) {
  std::vector<std::vector<const uint8_t*>> strings(count);
  for (size_t n = 0; n < count; ++n) {
    objects[n] = nullptr;
    strings[n] = FindNames(image, names[n]);
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

// The variable of an FAutoConsoleVariableRef: from the first registration of the name with a lea r8 before it.
inline void* FindVariableRef(const Image& image, const wchar_t* name) {
  for (const uint8_t* load : FindNameLoads(image, name)) {
    for (size_t back = kLeaSize; back <= kRefReach && back <= static_cast<size_t>(load - image.text.begin); ++back) {
      const uint8_t* p = load - back;
      if (memcmp(p, kLeaR8, 3) == 0) return RipTarget(p, kLeaSize);
    }
  }
  return nullptr;
}

}  // namespace ff7r::game_code
