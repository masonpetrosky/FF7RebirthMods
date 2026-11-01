// Samples the call stacks of a running process's busiest threads, to see where its CPU time goes: in the game's own
// code, in ReShade, in an add-on or Shader Injector, or in the driver. Frames are module + offset (no symbols needed);
// tools/cpu_profile.py turns the output into a per-thread, per-module table.
//
// usage: cpu_sampler <exe name or pid> <seconds> <output prefix> [--hz N] [--threads N] [--stack KB]
// Writes <prefix>_threads.tsv (every thread: CPU time, samples), <prefix>_modules.tsv and <prefix>_stacks.tsv (one
// sample per line: thread id, weight, frames leaf first as module index:offset) and <prefix>_symbols.tsv (export names
// for the leaf frames in system DLLs, so waits can be told apart from work).
//
// A sample suspends the thread only to read its registers and copy the top of its stack (some tens of microseconds);
// the stacks are walked from those copies after the sampling window. Frame times measured meanwhile are still a little
// worse, so measure frame rates separately.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Module {
  uint64_t base = 0;
  uint64_t size = 0;
  std::wstring path;
};

struct Thread {
  DWORD id = 0;
  HANDLE handle = nullptr;
  std::wstring name;
  uint64_t stack_base = 0;    // the top of its stack (NT_TIB::StackBase); 0 if unknown
  uint64_t cycles_start = 0;  // at the start of sampling
  uint64_t cycles_last = 0;   // at its last sample
  uint64_t cycles_end = 0;
  uint64_t cpu_start = 0;  // kernel + user time, 100 ns units
  uint64_t cpu_end = 0;
  bool sampled = false;
  uint32_t samples = 0;
  uint32_t copied = 0;
};

// Registers and the top of the stack, copied while the thread was suspended.
struct Snapshot {
  uint32_t thread = 0;  // index into the thread list
  float weight = 0;     // share of the interval the thread ran, from its cycle count
  CONTEXT context;
  uint64_t stack_address = 0;  // = context.Rsp
  uint32_t stack_offset = 0;   // into the shared stack buffer
  uint32_t stack_size = 0;
};

uint64_t Cycles(HANDLE thread) {
  ULONG64 cycles = 0;
  QueryThreadCycleTime(thread, &cycles);
  return cycles;
}

uint64_t CpuTime(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
         (static_cast<uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}

// Cycle counter ticks per second, measured on this thread.
double CyclesPerSecond() {
  HANDLE self = GetCurrentThread();
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t c0 = Cycles(self);
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const uint64_t c1 = Cycles(self);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return static_cast<double>(c1 - c0) / seconds;
}

DWORD FindProcess(const std::wstring& name_or_id) {
  if (!name_or_id.empty() && std::all_of(name_or_id.begin(), name_or_id.end(), iswdigit))
    return static_cast<DWORD>(std::wcstoul(name_or_id.c_str(), nullptr, 10));
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W entry = {sizeof(entry)};
  DWORD pid = 0;
  for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
    if (_wcsicmp(entry.szExeFile, name_or_id.c_str()) == 0) pid = entry.th32ProcessID;
  CloseHandle(snap);
  return pid;
}

std::vector<Module> ListModules(HANDLE process) {
  std::vector<Module> modules;
  std::vector<HMODULE> handles(4096);
  DWORD needed = 0;
  if (!EnumProcessModulesEx(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed,
                            LIST_MODULES_64BIT))
    return modules;
  handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
  for (HMODULE h : handles) {
    MODULEINFO info = {};
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleInformation(process, h, &info, sizeof(info))) continue;
    GetModuleFileNameExW(process, h, path, MAX_PATH);
    modules.push_back({reinterpret_cast<uint64_t>(info.lpBaseOfDll), info.SizeOfImage, path});
  }
  std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
  return modules;
}

// The module an address lies in, or -1.
int FindModule(const std::vector<Module>& modules, uint64_t address) {
  auto it = std::upper_bound(modules.begin(), modules.end(), address,
                             [](uint64_t a, const Module& m) { return a < m.base; });
  if (it == modules.begin()) return -1;
  --it;
  return address < it->base + it->size ? static_cast<int>(it - modules.begin()) : -1;
}

// The top of a thread's stack, from its TEB.
uint64_t StackBase(HANDLE process, HANDLE thread) {
  using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  struct {
    NTSTATUS exit_status;
    PVOID teb;
    CLIENT_ID client;
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
  } info = {};
  if (query == nullptr || query(thread, 0 /*ThreadBasicInformation*/, &info, sizeof(info), nullptr) < 0) return 0;
  NT_TIB tib = {};
  SIZE_T read = 0;
  if (!ReadProcessMemory(process, info.teb, &tib, sizeof(tib), &read)) return 0;
  return reinterpret_cast<uint64_t>(tib.StackBase);
}

std::vector<Thread> ListThreads(HANDLE process, DWORD pid) {
  std::vector<Thread> threads;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry = {sizeof(entry)};
  for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                          entry.th32ThreadID);
    if (h == nullptr) continue;
    Thread t;
    t.id = entry.th32ThreadID;
    t.handle = h;
    t.stack_base = StackBase(process, h);
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
      t.name = desc;
      LocalFree(desc);
    }
    threads.push_back(std::move(t));
  }
  CloseHandle(snap);
  return threads;
}

std::string Utf8(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  for (char& c : out)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return out;
}

bool IsSystemModule(const std::wstring& path) {
  std::wstring lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
  return lower.find(L"\\windows\\system32\\") != std::wstring::npos ||
         lower.find(L"\\windows\\winsxs\\") != std::wstring::npos ||
         lower.find(L"\\driverstore\\") != std::wstring::npos;
}

// Memory reads of the stack walk: the stack from the snapshot being walked, anything else (code, for epilogue checks)
// from the live process.
HANDLE g_process = nullptr;
const Snapshot* g_walking = nullptr;
const uint8_t* g_walking_stack = nullptr;

BOOL CALLBACK ReadMemory(HANDLE, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
  const Snapshot& s = *g_walking;
  if (address >= s.stack_address && address + size <= s.stack_address + s.stack_size) {
    std::memcpy(buffer, g_walking_stack + (address - s.stack_address), size);
    if (read) *read = size;
    return TRUE;
  }
  if (address >= s.stack_address && address < s.stack_address + (1u << 20)) return FALSE;  // deeper than the copy
  SIZE_T n = 0;
  const BOOL ok = ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(address), buffer, size, &n);
  if (read) *read = static_cast<DWORD>(n);
  return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    std::fwprintf(stderr,
                  L"usage: cpu_sampler <exe name or pid> <seconds> <output prefix> [--hz N] [--threads N] [--stack KB]\n");
    return 2;
  }
  const double seconds = std::wcstod(argv[2], nullptr);
  const std::wstring prefix = argv[3];
  int hz = 250;
  size_t max_threads = 12;
  uint32_t stack_bytes = 48 * 1024;
  for (int i = 4; i + 1 < argc; i += 2) {
    if (wcscmp(argv[i], L"--hz") == 0) hz = std::max(1, _wtoi(argv[i + 1]));
    if (wcscmp(argv[i], L"--threads") == 0) max_threads = std::max(1, _wtoi(argv[i + 1]));
    if (wcscmp(argv[i], L"--stack") == 0) stack_bytes = std::max(4, _wtoi(argv[i + 1])) * 1024u;
  }

  const DWORD pid = FindProcess(argv[1]);
  if (pid == 0) {
    std::fwprintf(stderr, L"process %ls not found\n", argv[1]);
    return 1;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (process == nullptr) {
    std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
    return 1;
  }
  g_process = process;
  // No symbol path: only module headers and unwind data are read, nothing is downloaded. The modules are registered
  // one by one (letting SymInitialize enumerate them fails on some processes).
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_UNDNAME);
  if (!SymInitializeW(process, L".", FALSE)) {
    std::fwprintf(stderr, L"SymInitialize failed: %lu\n", GetLastError());
    return 1;
  }
  std::set<uint64_t> registered;
  auto register_modules = [&](const std::vector<Module>& list) {
    for (const Module& m : list)
      if (registered.insert(m.base).second)
        SymLoadModuleExW(process, nullptr, m.path.c_str(), nullptr, m.base, static_cast<DWORD>(m.size), nullptr, 0);
  };
  const double cps = CyclesPerSecond();
  std::vector<Thread> threads = ListThreads(process, pid);

  // Pick the busiest threads over one second.
  for (auto& t : threads) t.cycles_last = Cycles(t.handle);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::vector<std::pair<uint64_t, size_t>> busy;
  for (size_t i = 0; i < threads.size(); ++i) busy.push_back({Cycles(threads[i].handle) - threads[i].cycles_last, i});
  std::sort(busy.rbegin(), busy.rend());
  for (size_t k = 0; k < busy.size() && k < max_threads; ++k) threads[busy[k].second].sampled = true;
  std::vector<size_t> sampled;
  for (size_t i = 0; i < threads.size(); ++i)
    if (threads[i].sampled) sampled.push_back(i);

  // Every snapshot gets its stack copy in one big buffer, sized for the whole window up front.
  const size_t max_snapshots = static_cast<size_t>(seconds * hz * sampled.size()) + sampled.size();
  std::vector<Snapshot> snapshots;
  snapshots.reserve(max_snapshots);
  // Reserved and committed at once but only touched as filled, so a long window costs only the memory it uses.
  const size_t stack_capacity = std::min<size_t>(max_snapshots * stack_bytes, size_t{1536} << 20);
  auto* const stacks =
      static_cast<uint8_t*>(VirtualAlloc(nullptr, stack_capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (stacks == nullptr) {
    std::fwprintf(stderr, L"out of memory\n");
    return 1;
  }
  size_t stack_used = 0;

  for (auto& t : threads) {
    t.cycles_start = t.cycles_last = Cycles(t.handle);
    t.cpu_start = CpuTime(t.handle);
  }
  timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto period = std::chrono::duration<double>(1.0 / hz);
  const auto start = std::chrono::steady_clock::now();
  auto next = start;
  auto last_tick = start;
  std::chrono::steady_clock::duration suspended{};
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now - start >= std::chrono::duration<double>(seconds)) break;
    const double interval = std::chrono::duration<double>(now - last_tick).count();
    last_tick = now;
    for (size_t index : sampled) {
      Thread& t = threads[index];
      const uint64_t c = Cycles(t.handle);
      const double ran = interval > 0 ? static_cast<double>(c - t.cycles_last) / (cps * interval) : 0;
      t.cycles_last = c;
      ++t.samples;
      if (ran < 0.02) continue;  // asleep for the whole interval
      if (snapshots.size() == snapshots.capacity() || stack_used + stack_bytes > stack_capacity) continue;
      Snapshot s;
      s.thread = static_cast<uint32_t>(index);
      s.weight = static_cast<float>(std::min(1.0, ran));
      s.context.ContextFlags = CONTEXT_FULL;
      const auto suspended_at = std::chrono::steady_clock::now();
      if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
      bool ok = GetThreadContext(t.handle, &s.context) != 0;
      if (ok) {
        s.stack_address = s.context.Rsp;
        uint64_t size = stack_bytes;
        if (t.stack_base > s.stack_address) size = std::min<uint64_t>(size, t.stack_base - s.stack_address);
        SIZE_T read = 0;
        ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(s.stack_address), stacks + stack_used, size,
                               &read) != 0 ||
             read != 0;
        s.stack_size = static_cast<uint32_t>(read);
      }
      ResumeThread(t.handle);
      suspended += std::chrono::steady_clock::now() - suspended_at;
      if (!ok) continue;
      s.stack_offset = static_cast<uint32_t>(stack_used / 16);  // in 16-byte units, so 3 GB fit
      stack_used += (s.stack_size + 15) & ~size_t{15};
      snapshots.push_back(s);
      ++t.copied;
    }
    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
    const auto wait = next - std::chrono::steady_clock::now();
    if (wait > std::chrono::steady_clock::duration::zero()) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    } else {
      next = std::chrono::steady_clock::now();  // fell behind: skip ticks instead of bursting
    }
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  timeEndPeriod(1);
  for (auto& t : threads) {
    t.cycles_end = Cycles(t.handle);
    t.cpu_end = CpuTime(t.handle);
  }
  const std::vector<Module> modules = ListModules(process);
  register_modules(modules);

  FILE* f = nullptr;
  if (_wfopen_s(&f, (prefix + L"_threads.tsv").c_str(), L"wb") != 0 || f == nullptr) return 1;
  std::fprintf(f, "index\ttid\tname\tsampled\tbusy\tcpu_seconds\tsamples\tcopied\n");
  for (size_t i = 0; i < threads.size(); ++i) {
    const Thread& t = threads[i];
    std::fprintf(f, "%zu\t%lu\t%s\t%d\t%.4f\t%.4f\t%u\t%u\n", i, t.id, Utf8(t.name).c_str(), t.sampled ? 1 : 0,
                 static_cast<double>(t.cycles_end - t.cycles_start) / (cps * elapsed), (t.cpu_end - t.cpu_start) * 1e-7,
                 t.samples, t.copied);
  }
  std::fclose(f);

  if (_wfopen_s(&f, (prefix + L"_modules.tsv").c_str(), L"wb") != 0 || f == nullptr) return 1;
  std::fprintf(f, "index\tbase\tsize\tsystem\tpath\n");
  for (size_t i = 0; i < modules.size(); ++i)
    std::fprintf(f, "%zu\t%llx\t%llx\t%d\t%s\n", i, modules[i].base, modules[i].size,
                 IsSystemModule(modules[i].path) ? 1 : 0, Utf8(modules[i].path).c_str());
  std::fclose(f);

  // The walks, from the copies. Leaf frames in system DLLs get their export name (e.g. NtWaitForSingleObject), so
  // waits are recognisable.
  const auto walk_start = std::chrono::steady_clock::now();
  std::set<uint64_t> leaves;
  size_t frames_total = 0;
  if (_wfopen_s(&f, (prefix + L"_stacks.tsv").c_str(), L"wb") != 0 || f == nullptr) return 1;
  const double suspended_s = std::chrono::duration<double>(suspended).count();
  std::fprintf(f, "# elapsed %.3f s, %d Hz, cycles/s %.0f, threads suspended %.3f s in total\n", elapsed, hz, cps,
               suspended_s);
  for (const Snapshot& s : snapshots) {
    g_walking = &s;
    g_walking_stack = stacks + size_t{s.stack_offset} * 16;
    CONTEXT ctx = s.context;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    std::fprintf(f, "%lu\t%.3f\t", threads[s.thread].id, s.weight);
    for (int depth = 0; depth < 96; ++depth) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, threads[s.thread].handle, &frame, &ctx, ReadMemory,
                       SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        break;
      const uint64_t a = frame.AddrPC.Offset;
      if (a == 0) break;
      const int m = FindModule(modules, a);
      if (m < 0)
        std::fprintf(f, "%s?:%llx", depth ? ";" : "", a);
      else
        std::fprintf(f, "%s%d:%llx", depth ? ";" : "", m, a - modules[m].base);
      if (depth < 2 && m >= 0 && IsSystemModule(modules[m].path)) leaves.insert(a);
      ++frames_total;
    }
    std::fprintf(f, "\n");
  }
  std::fclose(f);
  const double walk_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - walk_start).count();

  if (_wfopen_s(&f, (prefix + L"_symbols.tsv").c_str(), L"wb") != 0 || f == nullptr) return 1;
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
  for (uint64_t a : leaves) {
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 displacement = 0;
    const int m = FindModule(modules, a);
    if (m >= 0 && SymFromAddr(process, a, &displacement, sym))
      std::fprintf(f, "%d:%llx\t%s+%llx\n", m, a - modules[m].base, sym->Name, displacement);
  }
  std::fclose(f);

  for (auto& t : threads) CloseHandle(t.handle);
  SymCleanup(process);
  CloseHandle(process);
  std::printf("%zu samples from %zu threads in %.1f s: %.1f us suspended per sample, %.1f frames per stack, walked in "
              "%.1f s -> %s_*.tsv\n",
              snapshots.size(), sampled.size(), elapsed, snapshots.empty() ? 0.0 : suspended_s * 1e6 / snapshots.size(),
              snapshots.empty() ? 0.0 : static_cast<double>(frames_total) / snapshots.size(), walk_s,
              Utf8(prefix).c_str());
  return 0;
}
