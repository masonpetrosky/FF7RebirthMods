// Checks the dynamic resolution menu patch (src/resolution_menu.hpp) against the game's executable on disk: the lists
// and the four loads are found exactly once, the original loops read 33, 50, 66 and 100, and the redirected loads read
// the add-on's list. Also that a doubled list or a changed loop is refused.
// usage: resolution_menu_test.exe [path to ff7rebirth_.exe]
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "resolution_menu.hpp"

namespace menu = npcmotion::resolution_menu;

namespace {

struct Section {
  uintptr_t address = 0;  // ImageBase + VirtualAddress
  std::vector<uint8_t> bytes;
};

int g_failures = 0;

void Expect(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

// The int32 values a loop from `begin` to `end` (exclusive, step 4) reads, if both lie in `data`.
std::vector<int32_t> Walk(const Section& data, uintptr_t begin, uintptr_t end) {
  std::vector<int32_t> values;
  if (begin < data.address || end > data.address + data.bytes.size() || end < begin || (end - begin) % 4) return values;
  for (uintptr_t at = begin; at < end; at += 4) {
    int32_t v;
    memcpy(&v, data.bytes.data() + (at - data.address), 4);
    values.push_back(v);
  }
  return values;
}

std::string Text(const std::vector<int32_t>& values) {
  std::string s;
  for (int32_t v : values) s += (s.empty() ? "" : ", ") + std::to_string(v);
  return s.empty() ? "(none)" : s;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1]
                              : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\FINAL FANTASY VII "
                                "REBIRTH\\End\\Binaries\\Win64\\ff7rebirth_.exe";
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (image.size() < 0x1000) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
  const uintptr_t base = nt->OptionalHeader.ImageBase;
  Section text, rdata;
  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const IMAGE_SECTION_HEADER& s = sections[i];
    Section* out = strncmp(reinterpret_cast<const char*>(s.Name), ".text", 8) == 0    ? &text
                   : strncmp(reinterpret_cast<const char*>(s.Name), ".rdata", 8) == 0 ? &rdata
                                                                                       : nullptr;
    if (out == nullptr) continue;
    const size_t size = std::min<size_t>(s.Misc.VirtualSize, s.SizeOfRawData);
    out->address = base + s.VirtualAddress;
    out->bytes.assign(image.begin() + s.PointerToRawData, image.begin() + s.PointerToRawData + size);
  }
  std::printf("%s: .text %zu bytes, .rdata %zu bytes\n", path, text.bytes.size(), rdata.bytes.size());

  uintptr_t lists = 0;
  const bool have_lists = menu::FindLists(rdata.bytes.data(), rdata.bytes.size(), rdata.address, &lists);
  Expect(have_lists, "the two lists of limits occur once in .rdata");
  if (!have_lists) return 1;
  std::printf("      lists at rva 0x%zX\n", static_cast<size_t>(lists - base));

  menu::Sites sites;
  const bool have_sites = menu::FindSites(text.bytes.data(), text.bytes.size(), text.address, lists, &sites);
  Expect(have_sites, "each of the four loads occurs once, end after begin");
  if (!have_sites) return 1;
  const size_t at[4] = {sites.upper_begin, sites.upper_end, sites.lower_begin, sites.lower_end};
  const char* names[4] = {"upper begin", "upper end", "lower begin", "lower end"};
  uintptr_t original[4];
  for (int i = 0; i < 4; ++i) {
    original[i] = menu::Target(text.bytes.data() + at[i], text.address + at[i]);
    std::printf("      %-11s rva 0x%zX -> rva 0x%zX\n", names[i], static_cast<size_t>(text.address + at[i] - base),
                static_cast<size_t>(original[i] - base));
  }
  const std::vector<int32_t> game(menu::kGameLists, menu::kGameLists + 4);
  const std::vector<int32_t> upper = Walk(rdata, original[0], original[1]), lower = Walk(rdata, original[2], original[3]);
  std::printf("      the game's upper limits: %s; lower limits: %s\n", Text(upper).c_str(), Text(lower).c_str());
  Expect(upper == game && lower == game, "the original loops read 33, 50, 66, 100");

  // Redirect to a list where the add-on would put it: the free region just below the executable.
  Section ours;
  ours.address = base - 0x10000;
  ours.bytes.assign(reinterpret_cast<const uint8_t*>(menu::kSteps),
                    reinterpret_cast<const uint8_t*>(menu::kSteps) + sizeof(menu::kSteps));
  const uintptr_t targets[4] = {ours.address, ours.address + sizeof(menu::kSteps), ours.address,
                                ours.address + sizeof(menu::kSteps)};
  std::vector<uint8_t> patched = text.bytes;
  bool reach = true;
  for (int i = 0; i < 4; ++i) {
    int32_t disp = 0;
    reach = reach && menu::Displacement(text.address + at[i], targets[i], &disp);
    memcpy(patched.data() + at[i] + 3, &disp, 4);
  }
  Expect(reach, "a list just below the executable is in reach of all four loads");
  uintptr_t now[4];
  for (int i = 0; i < 4; ++i) now[i] = menu::Target(patched.data() + at[i], text.address + at[i]);
  const std::vector<int32_t> steps(menu::kSteps, menu::kSteps + 5);
  const std::vector<int32_t> new_upper = Walk(ours, now[0], now[1]), new_lower = Walk(ours, now[2], now[3]);
  std::printf("      redirected upper limits: %s; lower limits: %s\n", Text(new_upper).c_str(), Text(new_lower).c_str());
  Expect(new_upper == steps && new_lower == steps, "the redirected loops read 33, 50, 58, 66, 100");
  bool rest_unchanged = true;
  for (size_t i = 0; i < patched.size(); ++i) {
    bool in_disp = false;
    for (size_t a : at) in_disp = in_disp || (i >= a + 3 && i < a + 7);
    if (!in_disp && patched[i] != text.bytes[i]) rest_unchanged = false;
  }
  Expect(rest_unchanged, "only the four displacements change");
  int32_t far_disp = 0;
  Expect(!menu::Displacement(text.address, text.address + 0x90000000ull, &far_disp), "a list 2.25 GB away is refused");

  // Refusals.
  menu::Sites again;
  Expect(!menu::FindSites(patched.data(), patched.size(), text.address, lists, &again),
         "an already redirected executable is not patched again");
  std::vector<uint8_t> doubled = rdata.bytes;
  const size_t first = lists - rdata.address;
  memcpy(doubled.data() + (doubled.size() / 2 & ~size_t{3}), doubled.data() + first, sizeof(menu::kGameLists));
  uintptr_t unused = 0;
  Expect(!menu::FindLists(doubled.data(), doubled.size(), rdata.address, &unused), "doubled lists are refused");
  std::vector<uint8_t> changed = text.bytes;
  changed[sites.lower_end + menu::kLeaSize + 3] = 0x08;  // add r14, 8: a different loop
  Expect(!menu::FindSites(changed.data(), changed.size(), text.address, lists, &again), "a changed loop is refused");

  // In memory, the way the add-on does it: the executable mapped as an image (none of its code runs), the add-on's
  // list allocated near it, the four displacements rewritten, and the loops' bounds read back from the changed code.
  HMODULE module = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
  Expect(module != nullptr, "the executable maps as an image");
  if (module != nullptr) {
    auto* const mbase = reinterpret_cast<uint8_t*>(module);
    const auto* mnt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(mbase + reinterpret_cast<const IMAGE_DOS_HEADER*>(mbase)->e_lfanew);
    uint8_t* mtext = nullptr;
    size_t mtext_size = 0;
    const uint8_t* mrdata = nullptr;
    size_t mrdata_size = 0;
    const IMAGE_SECTION_HEADER* msec = IMAGE_FIRST_SECTION(mnt);
    for (int i = 0; i < mnt->FileHeader.NumberOfSections; ++i) {
      if (strncmp(reinterpret_cast<const char*>(msec[i].Name), ".text", 8) == 0)
        mtext = mbase + msec[i].VirtualAddress, mtext_size = msec[i].Misc.VirtualSize;
      if (strncmp(reinterpret_cast<const char*>(msec[i].Name), ".rdata", 8) == 0)
        mrdata = mbase + msec[i].VirtualAddress, mrdata_size = msec[i].Misc.VirtualSize;
    }
    const ULONGLONG t0 = GetTickCount64();
    uintptr_t mlists = 0;
    menu::Sites msites;
    const bool found = mtext != nullptr && mrdata != nullptr &&
                       menu::FindLists(mrdata, mrdata_size, reinterpret_cast<uintptr_t>(mrdata), &mlists) &&
                       menu::FindSites(mtext, mtext_size, reinterpret_cast<uintptr_t>(mtext), mlists, &msites);
    const ULONGLONG t1 = GetTickCount64();
    Expect(found, "lists and loads found in the mapped image");
    std::printf("      search took %llu ms\n", static_cast<unsigned long long>(t1 - t0));
    if (found) {
      uint8_t* const s[4] = {mtext + msites.upper_begin, mtext + msites.upper_end, mtext + msites.lower_begin,
                             mtext + msites.lower_end};
      auto* const list = static_cast<uint8_t*>(menu::AllocateNear(s[0], sizeof(menu::kSteps)));
      const ULONGLONG t2 = GetTickCount64();
      Expect(list != nullptr, "memory allocated near the menu code");
      if (list != nullptr) {
        std::printf("      image at %p, list at %p (%+lld MB from the code), allocation took %llu ms\n",
                    static_cast<void*>(mbase), static_cast<void*>(list),
                    static_cast<long long>((reinterpret_cast<intptr_t>(list) - reinterpret_cast<intptr_t>(s[0])) >> 20),
                    static_cast<unsigned long long>(t2 - t1));
        memcpy(list, menu::kSteps, sizeof(menu::kSteps));
        const uintptr_t b = reinterpret_cast<uintptr_t>(list), e = b + sizeof(menu::kSteps);
        const uintptr_t t[4] = {b, e, b, e};
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
          int32_t d = 0;
          DWORD old;
          ok = ok && menu::Displacement(reinterpret_cast<uintptr_t>(s[i]), t[i], &d) &&
               VirtualProtect(s[i] + 3, 4, PAGE_EXECUTE_READWRITE, &old);
          if (!ok) break;
          memcpy(s[i] + 3, &d, 4);
          VirtualProtect(s[i] + 3, 4, old, &old);
        }
        Expect(ok, "the four displacements rewritten in memory");
        const auto walk = [](uintptr_t begin, uintptr_t end) {
          std::vector<int32_t> v;
          for (uintptr_t a = begin; a < end && v.size() < 16; a += 4) v.push_back(*reinterpret_cast<const int32_t*>(a));
          return v;
        };
        const std::vector<int32_t> up = walk(menu::Target(s[0], reinterpret_cast<uintptr_t>(s[0])),
                                             menu::Target(s[1], reinterpret_cast<uintptr_t>(s[1])));
        const std::vector<int32_t> lo = walk(menu::Target(s[2], reinterpret_cast<uintptr_t>(s[2])),
                                             menu::Target(s[3], reinterpret_cast<uintptr_t>(s[3])));
        std::printf("      in memory: upper limits %s; lower limits %s\n", Text(up).c_str(), Text(lo).c_str());
        Expect(up == steps && lo == steps, "the changed code's loops read 33, 50, 58, 66, 100 in memory");
      }
      // The search below the image keeps finding room: 64 more blocks, all in reach.
      int count = 0;
      bool in_reach = true;
      for (; count < 64; ++count) {
        void* p = menu::AllocateNear(s[0], 4096);
        if (p == nullptr) break;
        int32_t d;
        in_reach = in_reach && menu::Displacement(reinterpret_cast<uintptr_t>(s[0]), reinterpret_cast<uintptr_t>(p), &d) &&
                   menu::Displacement(reinterpret_cast<uintptr_t>(s[3]), reinterpret_cast<uintptr_t>(p) + 4096, &d);
      }
      Expect(count == 64 && in_reach, "64 more allocations near the code, all in reach");
    }
    FreeLibrary(module);
  }

  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "CHECKS FAILED");
  return g_failures == 0 ? 0 : 1;
}
