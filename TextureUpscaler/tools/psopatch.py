"""Turns off the D3D12 driver PSO cache (pipeline libraries) in a freshly started ff7rebirth_.exe, for RenderDoc.

RenderDoc refuses ID3D12Device1::CreatePipelineLibrary with existing data (D3D12_ERROR_DRIVER_VERSION_MISMATCH),
and the game asserts when it re-creates its library (WindowsD3D12PipelineState.cpp:746). The game turns the cache on
in its [WindowsNoEditor DeviceProfile] (D3D12.PSO.DriverOptimizedDiskCache=1) and ignores user ini overrides, so this
patches FD3D12PipelineStateCache::Init in memory: `setne sil` (bUseAPILibaries and DiskBinaryCache.Init's enable flag)
becomes `xor esi, esi`, which is the engine's stock default (cvar 0). Nothing on disk changes.

Run it while RenderDoc holds the new process before its main thread starts (renderdoccmd capture
--opt-delay-for-debugger N). Waits up to 60 s for the process; if the patch cannot be applied it ends the process,
which has not run any game code yet.
Usage: psopatch.py <log file>
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

EXE = "ff7rebirth_.exe"
SITE_RVA = 0x46D6DB8  # cmp dword ptr [rax+r10*4], 0 ; setne sil ; mov [r13+208h], sil
ORIGINAL = bytes.fromhex("42833c9000 400f95c6 4188b508020000".replace(" ", ""))
PATCH_OFFSET = 5
PATCH = bytes.fromhex("31f66690")  # xor esi, esi ; nop

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
LOG = open(sys.argv[1], "a", encoding="utf-8")


def log(text):
    LOG.write(time.strftime("%H:%M:%S ") + text + "\n")
    LOG.flush()


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class PROCESS_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("ExitStatus", ctypes.c_long), ("PebBaseAddress", ctypes.c_void_p),
                ("AffinityMask", ctypes.c_void_p), ("BasePriority", ctypes.c_long),
                ("UniqueProcessId", ctypes.c_void_p), ("InheritedFromUniqueProcessId", ctypes.c_void_p)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualProtectEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.FlushInstructionCache.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t]
k32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]


def find_pid():
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pid = None
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == EXE:
            pid = entry.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return pid


def read(handle, address, size):
    buf = ctypes.create_string_buffer(size)
    done = ctypes.c_size_t()
    if not k32.ReadProcessMemory(handle, address, buf, size, ctypes.byref(done)) or done.value != size:
        raise OSError("ReadProcessMemory failed: %d" % ctypes.get_last_error())
    return buf.raw


def main():
    deadline = time.time() + 60
    pid = None
    while time.time() < deadline and pid is None:
        pid = find_pid()
        if pid is None:
            time.sleep(0.05)
    if pid is None:
        log("no %s within 60 s" % EXE)
        return 1
    # VM_OPERATION | VM_READ | VM_WRITE | QUERY_INFORMATION | TERMINATE
    handle = k32.OpenProcess(0x0008 | 0x0010 | 0x0020 | 0x0400 | 0x0001, False, pid)
    if not handle:
        log("OpenProcess(%d) failed: %d" % (pid, ctypes.get_last_error()))
        return 1
    try:
        pbi = PROCESS_BASIC_INFORMATION()
        status = ntdll.NtQueryInformationProcess(handle, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), None)
        if status != 0:
            raise OSError("NtQueryInformationProcess: 0x%x" % (status & 0xFFFFFFFF))
        base = int.from_bytes(read(handle, pbi.PebBaseAddress + 0x10, 8), "little")
        site = base + SITE_RVA
        current = read(handle, site, len(ORIGINAL))
        if current[PATCH_OFFSET:PATCH_OFFSET + len(PATCH)] == PATCH:
            log("pid %d base %x: already patched" % (pid, base))
            return 0
        if current != ORIGINAL:
            raise OSError("unexpected bytes at %x: %s (game updated?)" % (site, current.hex(" ")))
        target = site + PATCH_OFFSET
        old = wt.DWORD()
        if not k32.VirtualProtectEx(handle, target, len(PATCH), 0x40, ctypes.byref(old)):
            raise OSError("VirtualProtectEx failed: %d" % ctypes.get_last_error())
        done = ctypes.c_size_t()
        ok = k32.WriteProcessMemory(handle, target, PATCH, len(PATCH), ctypes.byref(done))
        k32.VirtualProtectEx(handle, target, len(PATCH), old.value, ctypes.byref(wt.DWORD()))
        k32.FlushInstructionCache(handle, target, len(PATCH))
        if not ok or read(handle, target, len(PATCH)) != PATCH:
            raise OSError("WriteProcessMemory failed: %d" % ctypes.get_last_error())
        log("pid %d base %x: PSO pipeline library disabled at %x" % (pid, base, target))
        return 0
    except Exception as exc:
        log("pid %d: %s; ending the process before it runs" % (pid, exc))
        k32.TerminateProcess(handle, 1)
        return 1
    finally:
        k32.CloseHandle(handle)


sys.exit(main())
