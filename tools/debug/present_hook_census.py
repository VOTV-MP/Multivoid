# present_hook_census.py -- WHO is patching the graphics present chain in a live
# process, and where does each patch actually LEAD?
#
# WHY THIS EXISTS: `docs/OVERLAY_CAPTURE_COEXIST.md` roots two user-visible defects
# (our overlay invisible under RivaTuner; OBS not capturing it) in ONE fact -- we own
# an inline hook on `IDXGISwapChain::Present`, a function several other programs also
# patch. Before installing ANY inline hook the question is "who else is on this
# function?", and this is the instrument that answers it with bytes instead of
# guesses. Used 2026-08-23 to census the live game: it attributed our own two hooks
# to main.dll and found a THIRD hooker nobody knew was in the process
# (`NahimicOSD.dll` on `IDXGISwapChain1::Present1`) -- see that doc's section 6c.
#
# USAGE
#   python tools/debug/present_hook_census.py census <pid|exe-substring>
#       -> loaded modules of interest + the prologue bytes of each present-chain
#          function + a verdict + (for E9 patches) the owning module of the target
#   python tools/debug/present_hook_census.py follow <pid> <hex-addr> [<hex-addr>...]
#       -> walk a jmp/relay chain hop by hop until it lands in a real module
#
# NOTES
#  - The function offsets below are RTSS's OWN resolved-offset cache
#    (`RivaTuner Statistics Server\Profiles\Config`, [FnOffsetCache64]), i.e. exactly
#    the addresses RTSS targets. If RTSS is not installed, or Windows updates dxgi,
#    re-read that file (or resolve the vtable slots yourself) -- a STALE offset reads
#    as "clean prologue" and the instrument then lies by omission.
#  - A hook engine puts its trampoline in a MEM_PRIVATE page near the target, so the
#    E9 destination is usually NOT in any module; `follow` resolves the next hop
#    (MinHook's `FF25` relay, or the followJmp-immune `mov rax,imm64; jmp rax` form)
#    until it reaches module code. That final module IS the hook's owner.

import ctypes as C
import ctypes.wintypes as W
import sys

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
k32 = C.windll.kernel32
psapi = C.windll.psapi

# ctypes guesses a 32-bit C int for an undeclared pointer argument, which silently
# OVERFLOWS on a real 64-bit module base (it raises here, but the class of bug is
# a wrong address read as success). Declare every signature that takes a pointer.
psapi.GetModuleFileNameExW.argtypes = [C.c_void_p, C.c_void_p, C.c_wchar_p, W.DWORD]
psapi.GetModuleFileNameExW.restype = W.DWORD
psapi.GetModuleInformation.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, W.DWORD]
psapi.GetModuleInformation.restype = W.BOOL
k32.ReadProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p,
                                  C.c_size_t, C.POINTER(C.c_size_t)]
k32.ReadProcessMemory.restype = W.BOOL

TARGETS = [
    ("dxgi.dll",      0x18C0,  "IDXGISwapChain::Present"),
    ("dxgi.dll",      0x22810, "IDXGISwapChain::ResizeBuffers"),
    ("dxgi.dll",      0x6AA90, "IDXGISwapChain1::Present1"),
    ("d3d12core.dll", 0xB58F0, "ID3D12CommandQueue::ExecuteCommandLists"),
]
INTERESTING = ("rtss", "obs", "dxgi", "d3d11", "d3d12", "multivoid", "main.dll",
               "ue4ss", "afterburner", "nahimic", "avolute", "discord", "steam_api",
               "gameoverlay", "nvoglv", "amdih")


class MODULEINFO(C.Structure):
    _fields_ = [("lpBaseOfDll", C.c_void_p), ("SizeOfImage", W.DWORD),
                ("EntryPoint", C.c_void_p)]


class MEMORY_BASIC_INFORMATION(C.Structure):
    _fields_ = [("BaseAddress", C.c_void_p), ("AllocationBase", C.c_void_p),
                ("AllocationProtect", W.DWORD), ("__a", W.DWORD),
                ("RegionSize", C.c_size_t), ("State", W.DWORD),
                ("Protect", W.DWORD), ("Type", W.DWORD), ("__a2", W.DWORD)]


k32.VirtualQueryEx.argtypes = [C.c_void_p, C.c_void_p,
                               C.POINTER(MEMORY_BASIC_INFORMATION), C.c_size_t]
k32.VirtualQueryEx.restype = C.c_size_t
_MEM_TYPE = {0x1000000: "MEM_IMAGE", 0x40000: "MEM_MAPPED", 0x20000: "MEM_PRIVATE"}


def open_proc(arg):
    try:
        pid = int(arg)
    except ValueError:
        pid = None
        arr = (W.DWORD * 4096)()
        need = W.DWORD(0)
        psapi.EnumProcesses(arr, C.sizeof(arr), C.byref(need))
        for i in range(need.value // C.sizeof(W.DWORD)):
            h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, arr[i])
            if not h:
                continue
            nm = C.create_unicode_buffer(512)
            if psapi.GetModuleFileNameExW(h, None, nm, 512) and arg.lower() in nm.value.lower():
                pid = arr[i]
            k32.CloseHandle(h)
            if pid:
                break
        if not pid:
            print("ERROR: no process matched %r" % arg)
            sys.exit(1)
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print("ERROR OpenProcess(%d): %d" % (pid, k32.GetLastError()))
        sys.exit(1)
    return pid, h


def modules(h):
    out = []
    mods = (C.c_void_p * 4096)()
    need = W.DWORD(0)
    if psapi.EnumProcessModulesEx(h, mods, C.sizeof(mods), C.byref(need), 0x03):
        for i in range(need.value // C.sizeof(C.c_void_p)):
            nm = C.create_unicode_buffer(512)
            psapi.GetModuleFileNameExW(h, mods[i], nm, 512)
            mi = MODULEINFO()
            psapi.GetModuleInformation(h, mods[i], C.byref(mi), C.sizeof(mi))
            out.append((mi.lpBaseOfDll, mi.SizeOfImage, nm.value))
    return out


def owner(mods_, a):
    for b, sz, p in mods_:
        if b <= a < b + sz:
            return "%s +0x%X" % (p.rsplit("\\", 1)[-1], a - b)
    return None


def rd(h, a, n):
    buf = (C.c_ubyte * n)()
    got = C.c_size_t(0)
    if k32.ReadProcessMemory(h, C.c_void_p(a), buf, n, C.byref(got)):
        return bytes(buf[:got.value])
    return b""


def classify(b):
    if not b:
        return "UNREADABLE"
    if b[0] == 0xE9:
        return "HOOKED (E9 rel32 jmp -- MinHook/Detours style)"
    if b[0] == 0xFF and b[1] == 0x25:
        return "HOOKED (FF25 indirect jmp)"
    if b[0] == 0x48 and b[1] == 0xB8 and b[10:12] == bytes([0xFF, 0xE0]):
        return "HOOKED (mov rax,imm64 + jmp rax -- followJmp-immune relay)"
    if b[0] == 0xEB:
        return "HOOKED (EB short jmp)"
    if b[0] == 0xCC:
        return "HOOKED (INT3 / breakpoint-style)"
    return "clean prologue (no jmp/int3 at byte 0)"


def follow(h, mods_, a, maxhops=6):
    for hop in range(maxhops):
        b = rd(h, a, 24)
        if not b:
            print("  [%d] 0x%016X  UNREADABLE" % (hop, a))
            return
        own = owner(mods_, a)
        print("  [%d] 0x%016X  %-34s %s"
              % (hop, a, own or "(not in a module)", " ".join("%02X" % x for x in b[:16])))
        if own:
            return  # reached real module code -- that is the hook's owner
        if b[0] == 0xE9:
            a = a + 5 + int.from_bytes(b[1:5], "little", signed=True)
        elif b[0] == 0xFF and b[1] == 0x25:
            slot = a + 6 + int.from_bytes(b[2:6], "little", signed=True)
            tgt = rd(h, slot, 8)
            if len(tgt) < 8:
                print("      (pointer slot 0x%X unreadable)" % slot)
                return
            a = int.from_bytes(tgt, "little")
            print("      FF25 indirect via slot 0x%X -> 0x%016X" % (slot, a))
        elif b[0] == 0x48 and b[1] == 0xB8:
            a = int.from_bytes(b[2:10], "little")
            print("      mov rax,imm64 -> 0x%016X" % a)
        else:
            print("      (no further jmp -- this is the detour/trampoline body)")
            return


def cmd_census(arg):
    pid, h = open_proc(arg)
    mods_ = modules(h)
    bases = {p.rsplit("\\", 1)[-1].lower(): (b, sz, p) for b, sz, p in mods_}
    print("=== process %d ===" % pid)
    print("\n--- loaded modules of interest ---")
    for short, (b, sz, p) in sorted(bases.items()):
        if any(k in short or k in p.lower() for k in INTERESTING):
            print("  base=0x%016X size=0x%-8X %s" % (b, sz, p))
    print("\n--- present-chain prologues (offsets from RTSS's own resolved cache) ---")
    for mod, off, label in TARGETS:
        if mod not in bases:
            print("  %-42s : module %s NOT LOADED" % (label, mod))
            continue
        addr = bases[mod][0] + off
        raw = rd(h, addr, 24)
        print("  %-42s @0x%016X" % (label, addr))
        print("      bytes: %s" % (" ".join("%02X" % x for x in raw) if raw else "(read failed)"))
        print("      verdict: %s" % classify(raw))
        if raw and raw[0] == 0xE9:
            tgt = addr + 5 + int.from_bytes(raw[1:5], "little", signed=True)
            own = owner(mods_, tgt)
            if own:
                print("      jmp -> 0x%016X  IN MODULE %s" % (tgt, own))
            else:
                mbi = MEMORY_BASIC_INFORMATION()
                extra = ""
                if k32.VirtualQueryEx(h, C.c_void_p(tgt), C.byref(mbi), C.sizeof(mbi)):
                    extra = " %s allocbase=0x%X" % (_MEM_TYPE.get(mbi.Type, hex(mbi.Type)),
                                                    mbi.AllocationBase or 0)
                print("      jmp -> 0x%016X  trampoline (not a module)%s" % (tgt, extra))
                print("      following it to the owner:")
                follow(h, mods_, tgt)
    k32.CloseHandle(h)


def cmd_follow(argv):
    pid, h = open_proc(argv[0])
    mods_ = modules(h)
    for a in argv[1:]:
        print("\n=== follow 0x%s ===" % a)
        follow(h, mods_, int(a, 16))
    k32.CloseHandle(h)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "census":
        cmd_census(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "follow":
        cmd_follow(sys.argv[2:])
    else:
        print(__doc__ or "")
        print("usage: present_hook_census.py census <pid|exe-substring>")
        print("       present_hook_census.py follow <pid> <hex-addr> [...]")
        sys.exit(2)


main()
