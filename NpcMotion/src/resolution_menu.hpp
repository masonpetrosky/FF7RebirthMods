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
#include <cstddef>
#include <cstdint>
#include <cstring>

// The dynamic resolution limits in the game's graphics options. The menu builds its two choice lists (lower limit,
// upper limit) by copying a constant list from the executable's read-only data, element by element: 33, 50, 66 and
// 100%. The code that applies a choice (r.DynamicRes.Min/MaxScreenPercentage) looks the value up in its own table,
// which also has 58 (58.82% = 1/1.7, the Balanced mode) and 75, and the game saves the value, not its place in the
// list. The add-on points both copy loops at a list of its own with 58 added; the game then offers, applies, saves and
// shows 58% like its own entries, and its DLSS code picks Balanced for a render scale between 0.58 and 0.66.
namespace npcmotion::resolution_menu {

// The two lists as the executable has them: the upper limit's, then the lower limit's.
inline constexpr int32_t kGameLists[8] = {33, 50, 66, 100, 33, 50, 66, 100};
// What the add-on gives both lists.
inline constexpr int32_t kSteps[5] = {33, 50, 58, 66, 100};

// The four RIP-relative loads of the lists' bounds, as offsets into the code. Each loop starts with
// lea r14, [rip+begin] (4C 8D 35 disp32) and ends with lea rax, [rip+end]; add r14, 4; cmp r14, rax.
struct Sites {
  size_t upper_begin = 0, upper_end = 0, lower_begin = 0, lower_end = 0;
};
inline constexpr size_t kLeaSize = 7;    // the displacement is the last 4 bytes
inline constexpr size_t kLoopReach = 0x400;  // a loop's end load follows its begin load within this many bytes
inline constexpr uint8_t kBeginLoad[3] = {0x4C, 0x8D, 0x35};
inline constexpr uint8_t kEndLoad[3] = {0x48, 0x8D, 0x05};
inline constexpr uint8_t kEndTail[7] = {0x49, 0x83, 0xC6, 0x04, 0x4C, 0x3B, 0xF0};

// Where the lea at `p` (mapped at `address`) points.
inline uintptr_t Target(const uint8_t* p, uintptr_t address) {
  int32_t disp;
  memcpy(&disp, p + 3, 4);
  return address + kLeaSize + static_cast<intptr_t>(disp);
}

// The lists in read-only data (mapped at `address`), if they occur exactly once (4-byte aligned).
inline bool FindLists(const uint8_t* data, size_t size, uintptr_t address, uintptr_t* lists) {
  bool found = false;
  for (size_t i = 0; i + sizeof(kGameLists) <= size; i += 4) {
    if (memcmp(data + i, kGameLists, sizeof(kGameLists)) != 0) continue;
    if (found) return false;
    found = true;
    *lists = address + i;
  }
  return found;
}

// The four loads in `code` (mapped at `address`) for lists at `lists`, if each occurs exactly once and each loop's
// end load follows its begin load.
inline bool FindSites(const uint8_t* code, size_t size, uintptr_t address, uintptr_t lists, Sites* sites) {
  int upper_begin = 0, upper_end = 0, lower_begin = 0, lower_end = 0;
  for (size_t i = 0; i + kLeaSize + sizeof(kEndTail) <= size; ++i) {
    const uint8_t* p = code + i;
    if (p[1] != 0x8D) continue;
    if (memcmp(p, kBeginLoad, 3) == 0) {
      const uintptr_t t = Target(p, address + i);
      if (t == lists) sites->upper_begin = i, ++upper_begin;
      if (t == lists + 16) sites->lower_begin = i, ++lower_begin;
    } else if (memcmp(p, kEndLoad, 3) == 0 && memcmp(p + kLeaSize, kEndTail, sizeof(kEndTail)) == 0) {
      const uintptr_t t = Target(p, address + i);
      if (t == lists + 16) sites->upper_end = i, ++upper_end;
      if (t == lists + 32) sites->lower_end = i, ++lower_end;
    }
  }
  return upper_begin == 1 && upper_end == 1 && lower_begin == 1 && lower_end == 1 &&
         sites->upper_begin < sites->upper_end && sites->upper_end - sites->upper_begin < kLoopReach &&
         sites->lower_begin < sites->lower_end && sites->lower_end - sites->lower_begin < kLoopReach;
}

// The displacement that makes the lea at `site_address` load `target`, if it is in reach.
inline bool Displacement(uintptr_t site_address, uintptr_t target, int32_t* disp) {
  const intptr_t d = static_cast<intptr_t>(target) - static_cast<intptr_t>(site_address + kLeaSize);
  if (d < INT32_MIN || d > INT32_MAX) return false;
  *disp = static_cast<int32_t>(d);
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

}  // namespace npcmotion::resolution_menu
