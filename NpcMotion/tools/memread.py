"""Read values from the running ff7rebirth_.exe: memread.py <rva hex>:<i|f>[:label] ...  (read-only)"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class PBI(ctypes.Structure):
    _fields_ = [("ExitStatus", ctypes.c_long), ("PebBaseAddress", ctypes.c_void_p), ("AffinityMask", ctypes.c_void_p),
                ("BasePriority", ctypes.c_long), ("UniqueProcessId", ctypes.c_void_p), ("Inherited", ctypes.c_void_p)]


def find_pid(name="ff7rebirth_.exe"):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == name:
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError("read failed at %x" % addr)
    return buf.raw


pid = find_pid()
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
pbi = PBI()
ntdll.NtQueryInformationProcess(h, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), None)
base = struct.unpack("<Q", read(h, pbi.PebBaseAddress + 0x10, 8))[0]
print("pid %d base %x" % (pid, base))
for arg in sys.argv[1:]:
    parts = arg.split(":")
    rva, kind = int(parts[0], 16), parts[1]
    label = parts[2] if len(parts) > 2 else ""
    raw = read(h, base + rva, 4)
    value = struct.unpack("<i" if kind == "i" else "<f", raw)[0]
    print("%-55s %x = %s" % (label, rva, value))
