"""One module's share of the game's CPU samples by function and source line, from the module's symbols.

cpu_symbols.py PREFIX BUILD_DLL [--threads N] [--top N] [--function NAME]

PREFIX is build/cpu_sampler.exe's output prefix (perf_session.py's OUT_DIR/cpu or OUT_DIR/cpu_off); BUILD_DLL a build of
the module with its PDB beside it (FF7RebirthRayTracing's build\\FF7RebirthRayTracing.addon64 has one). The samples'
module is the one of the same file name. Its code is checked first against the file that was sampled (its path in
PREFIX_modules.tsv): the same bytes, or at least every instruction at the same offset (a build of the same source with
other data addresses: then a note); anything else stops, as the lines would not be the sampled code's.

Per thread (the N busiest with samples in the module):
- by function, inclusive (a sample counts once for every function of the module on its stack) and self (the first
  frame of the module from the leaf, so it includes the module's calls into Windows, the D3D12 runtime and the driver);
- the calls out of the module by call site: for a call through a vtable, its slot and the ID3D12GraphicsCommandList4
  method of that slot (right when the object is a command list; ReShade's and other objects have other methods);
- with --function NAME: that function's lines, inclusive (each sample at the line of the function's own frame, inlined
  code charged to the line that calls it).
Needs the analysis Python (Documents\\Tools\\Python\\analysis: capstone).
"""
import argparse
import bisect
import collections
import ctypes
import os
import re
import struct
import sys
from ctypes import wintypes

import capstone

HZ = 250.0


def sections(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    image_size = struct.unpack_from("<I", data, pe + 24 + 56)[0]
    out = {}
    for i in range(count):
        name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, pe + 24 + opt + 40 * i)
        out[name.rstrip(b"\0").decode()] = (va, data[raw:raw + min(vsize, rsize)])
    return out, image_size


def same_code(a, b):
    """'identical', 'layout' (every function's instructions at the same offsets) or a reason they differ."""
    if a[".text"] == b[".text"]:
        return "identical"
    def functions(pdata):  # each function's start and end (its unwind data's address may differ)
        return [struct.unpack_from("<II", pdata, i) for i in range(0, len(pdata) - 11, 12)]

    if len(a[".text"][1]) != len(b[".text"][1]) or functions(a[".pdata"][1]) != functions(b[".pdata"][1]):
        return "the code differs in size or functions"
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    va, ta = a[".text"]
    tb = b[".text"][1]
    for start, end in functions(a[".pdata"][1]):
        if start == 0:
            continue
        ia = [(x.address, x.size, x.mnemonic) for x in md.disasm(ta[start - va:end - va], start)]
        ib = [(x.address, x.size, x.mnemonic) for x in md.disasm(tb[start - va:end - va], start)]
        if ia != ib:
            return f"the function at {start:#x} has other instructions"
    return "layout"


# ID3D12GraphicsCommandList4's vtable in order (IUnknown, ID3D12Object, DeviceChild, CommandList, then the lists').
LIST_METHODS = (
    "QueryInterface AddRef Release GetPrivateData SetPrivateData SetPrivateDataInterface SetName GetDevice GetType Close "
    "Reset ClearState DrawInstanced DrawIndexedInstanced Dispatch CopyBufferRegion CopyTextureRegion CopyResource "
    "CopyTiles ResolveSubresource IASetPrimitiveTopology RSSetViewports RSSetScissorRects OMSetBlendFactor "
    "OMSetStencilRef SetPipelineState ResourceBarrier ExecuteBundle SetDescriptorHeaps SetComputeRootSignature "
    "SetGraphicsRootSignature SetComputeRootDescriptorTable SetGraphicsRootDescriptorTable SetComputeRoot32BitConstant "
    "SetGraphicsRoot32BitConstant SetComputeRoot32BitConstants SetGraphicsRoot32BitConstants "
    "SetComputeRootConstantBufferView SetGraphicsRootConstantBufferView SetComputeRootShaderResourceView "
    "SetGraphicsRootShaderResourceView SetComputeRootUnorderedAccessView SetGraphicsRootUnorderedAccessView "
    "IASetIndexBuffer IASetVertexBuffers SOSetTargets OMSetRenderTargets ClearDepthStencilView ClearRenderTargetView "
    "ClearUnorderedAccessViewUint ClearUnorderedAccessViewFloat DiscardResource BeginQuery EndQuery ResolveQueryData "
    "SetPredication SetMarker BeginEvent EndEvent ExecuteIndirect AtomicCopyBufferUINT AtomicCopyBufferUINT64 "
    "OMSetDepthBounds SetSamplePositions ResolveSubresourceRegion SetViewInstanceMask WriteBufferImmediate "
    "SetProtectedResourceSession BeginRenderPass EndRenderPass InitializeMetaCommand ExecuteMetaCommand "
    "BuildRaytracingAccelerationStructure EmitRaytracingAccelerationStructurePostbuildInfo "
    "CopyRaytracingAccelerationStructure SetPipelineState1 DispatchRays").split()


class Line64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wintypes.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]


class SymbolInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", ctypes.c_char * 512)]


class Symbols:
    """Names and lines of a module's addresses (RVAs) from its PDB, through dbghelp (no process is read)."""
    BASE = 0x180000000

    def __init__(self, dll, image_size):
        self.dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
        d = self.dbg
        self.process = wintypes.HANDLE(0x7FEE)  # any value: SymInitialize with invade off reads no process
        d.SymSetOptions(0x10 | 0x2 | 0x200)  # load lines, undecorated names, no error boxes
        d.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
        d.SymLoadModuleExW.restype = ctypes.c_uint64
        d.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                       ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        d.SymFromAddr.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                  ctypes.POINTER(SymbolInfo)]
        d.SymFromInlineContext.argtypes = [wintypes.HANDLE, ctypes.c_uint64, wintypes.ULONG,
                                           ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SymbolInfo)]
        d.SymGetLineFromAddr64.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(wintypes.DWORD),
                                           ctypes.POINTER(Line64)]
        d.SymGetLineFromInlineContext.argtypes = [wintypes.HANDLE, ctypes.c_uint64, wintypes.ULONG, ctypes.c_uint64,
                                                  ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(Line64)]
        d.SymAddrIncludeInlineTrace.restype = wintypes.DWORD
        d.SymAddrIncludeInlineTrace.argtypes = [wintypes.HANDLE, ctypes.c_uint64]
        d.SymQueryInlineTrace.argtypes = [wintypes.HANDLE, ctypes.c_uint64, wintypes.DWORD, ctypes.c_uint64,
                                          ctypes.c_uint64, ctypes.POINTER(wintypes.DWORD),
                                          ctypes.POINTER(wintypes.DWORD)]
        if not d.SymInitializeW(self.process, os.path.dirname(os.path.abspath(dll)), False):
            sys.exit("SymInitialize failed")
        if not d.SymLoadModuleExW(self.process, None, os.path.abspath(dll), None, self.BASE, image_size, None, 0):
            sys.exit(f"no symbols for {dll}")
        self.cache = {}

    def _name(self, addr, inline_context=None):
        sym = SymbolInfo()
        sym.SizeOfStruct = 88  # sizeof(SYMBOL_INFO) in C, its name array of one
        sym.MaxNameLen = 511
        disp = ctypes.c_uint64()
        ok = (self.dbg.SymFromAddr(self.process, addr, ctypes.byref(disp), ctypes.byref(sym)) if inline_context is None
              else self.dbg.SymFromInlineContext(self.process, addr, inline_context, ctypes.byref(disp),
                                                 ctypes.byref(sym)))
        return sym.Name.decode(errors="replace") if ok else "?"

    def where(self, rva):
        """(function, [(function, file:line)] from the innermost inlined frame out to the function itself)."""
        if rva in self.cache:
            return self.cache[rva]
        addr = self.BASE + rva
        function = self._name(addr)
        chain = []
        n = self.dbg.SymAddrIncludeInlineTrace(self.process, addr)
        context, index = wintypes.DWORD(), wintypes.DWORD()
        if n and self.dbg.SymQueryInlineTrace(self.process, addr, 0, addr, addr, ctypes.byref(context),
                                              ctypes.byref(index)):
            for i in range(n + 1):
                line, disp = Line64(), wintypes.DWORD()
                line.SizeOfStruct = ctypes.sizeof(Line64)
                if self.dbg.SymGetLineFromInlineContext(self.process, addr, context.value + i, 0, ctypes.byref(disp),
                                                        ctypes.byref(line)):
                    chain.append((self._name(addr, context.value + i),
                                  f"{os.path.basename(line.FileName.decode())}:{line.LineNumber}"))
        if not chain:
            line, disp = Line64(), wintypes.DWORD()
            line.SizeOfStruct = ctypes.sizeof(Line64)
            if self.dbg.SymGetLineFromAddr64(self.process, addr, ctypes.byref(disp), ctypes.byref(line)):
                chain.append((function, f"{os.path.basename(line.FileName.decode())}:{line.LineNumber}"))
        self.cache[rva] = (function, chain)
        return self.cache[rva]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix")
    parser.add_argument("build_dll")
    parser.add_argument("--threads", type=int, default=4, help="busiest threads with samples in the module (4)")
    parser.add_argument("--top", type=int, default=12, help="functions, call sites and lines listed (12)")
    parser.add_argument("--function", default="", help="also this function's lines (a part of its name)")
    args = parser.parse_args()

    module_name = os.path.basename(args.build_dll).lower()
    modules, sampled_path = {}, None
    for line in open(args.prefix + "_modules.tsv", encoding="utf-8").read().splitlines()[1:]:
        index, base, size, system, path = line.split("\t", 4)
        if os.path.basename(path).lower() == module_name:
            modules[index] = path
            sampled_path = path
    if not modules:
        sys.exit(f"{module_name} is not among the sampled modules")
    built, image_size = sections(args.build_dll)
    if not os.path.exists(sampled_path):
        sys.exit(f"the sampled file is gone ({sampled_path}): its code cannot be checked against the build")
    verdict = same_code(built, sections(sampled_path)[0])
    if verdict not in ("identical", "layout"):
        sys.exit(f"{args.build_dll} is not the sampled build: {verdict}")
    if verdict == "layout":
        print("Note: the build's code has every instruction at the sampled build's offsets, but other data addresses.")
    symbols = Symbols(args.build_dll, image_size)
    text_va, text = built[".text"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def call_before(ret):
        for back in (2, 3, 5, 6, 7):  # call reg, call [reg+disp8], call rel32, call [reg+disp32] (with REX)
            code = text[ret - back - text_va:ret - text_va]
            insns = list(md.disasm(code, ret - back))
            if len(insns) == 1 and insns[0].mnemonic == "call" and insns[0].address + insns[0].size == ret:
                m = re.search(r"\[r\w+ \+ (0x[0-9a-f]+)\]", insns[0].op_str)
                if m:
                    slot = int(m.group(1), 16) // 8
                    guess = f" (on a command list: {LIST_METHODS[slot]})" if slot < len(LIST_METHODS) else ""
                    return f"vtable slot {slot}{guess}"
                m = re.match(r"0x([0-9a-f]+)$", insns[0].op_str)
                if m:
                    return symbols.where(int(m.group(1), 16))[0]
                return "an indirect call"
        return "?"

    threads = {}
    for line in open(args.prefix + "_threads.tsv", encoding="utf-8").read().splitlines()[1:]:
        index, tid, name, sampled, busy, cpu_s, samples, copied = line.split("\t")
        threads[tid] = {"name": name or "(unnamed)", "busy": float(busy), "weight": 0.0, "ours": 0.0,
                        "inclusive": collections.Counter(), "self": collections.Counter(),
                        "calls": collections.Counter(), "lines": collections.Counter()}
    for line in open(args.prefix + "_stacks.tsv", encoding="utf-8"):
        if line.startswith("#"):
            continue
        tid, weight, stack = line.rstrip("\n").split("\t")
        t = threads[tid]
        w = float(weight)
        t["weight"] += w
        frames = stack.split(";") if stack else []
        ours = [(k, int(f.split(":")[1], 16)) for k, f in enumerate(frames) if f.split(":")[0] in modules]
        if not ours:
            continue
        t["ours"] += w
        seen = set()
        for k, rva in ours:
            function, chain = symbols.where(rva)
            if function not in seen:
                t["inclusive"][function] += w
                seen.add(function)
            if args.function and args.function in function and "line" not in seen:
                own = next((place for name, place in reversed(chain) if args.function in name),
                           chain[-1][1] if chain else "?")
                t["lines"][own + ("" if k == 0 else " (and what it calls)")] += w
                seen.add("line")
        k, rva = ours[0]
        function = symbols.where(rva)[0]
        t["self"][function + ("" if k == 0 else " (its calls out)")] += w
        if k > 0:
            t["calls"][(function, call_before(rva))] += w

    busiest = sorted((t for t in threads.values() if t["ours"] > 0), key=lambda t: -t["busy"])[:args.threads]
    for t in busiest:
        share = 100 / t["weight"]
        print(f"\n{t['name']} (busy {100 * t['busy']:.0f}%, {t['weight'] / HZ:.1f} s run in its samples): "
              f"{module_name} {t['ours'] * share:.1f}% of its samples")
        print("  inclusive:")
        for function, w in t["inclusive"].most_common(args.top):
            print(f"    {w * share:5.2f}%  {function}")
        print("  self:")
        for function, w in t["self"].most_common(args.top):
            print(f"    {w * share:5.2f}%  {function}")
        if t["calls"]:
            print("  calls out, by call site:")
            for (function, call), w in t["calls"].most_common(args.top):
                print(f"    {w * share:5.2f}%  {call} from {function}")
        if t["lines"]:
            print(f"  {args.function}'s lines (inclusive):")
            for place, w in t["lines"].most_common(args.top):
                print(f"    {w * share:5.2f}%  {place}")


if __name__ == "__main__":
    main()
