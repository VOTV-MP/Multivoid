#!/usr/bin/env python3
# Map a faulting RVA (logged by the game_thread Pump firewall's absorbed-fault
# localization as "ip - modbase") to the enclosing function in the payload .map (multivoid-*.map).
# Usage: python tools/maprva.py 0x167a0 [0x12610 ...]
# The diagnostic logs RVA already relative to the module base, and the .map
# lists each symbol's preferred VA (base 0x180000000 + symbol RVA), so the
# enclosing function is the symbol with the largest RVA <= the target RVA.
import sys, re, pathlib

PREF_BASE = 0x180000000
MAP = max((pathlib.Path(__file__).resolve().parent.parent / "build" / "votv-coop" / "Release").glob("multivoid-*.map"),
          key=lambda p: p.stat().st_mtime)

def load_syms():
    syms = []
    # lines like: " 0001:000167a0  ?ApplyToEngine@RemotePlayer@coop@@... 00000001800167a0 f   remote_player.obj"
    pat = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+\w?\s*(\S+)?")
    for ln in MAP.read_text(errors="replace").splitlines():
        m = pat.match(ln)
        if not m:
            continue
        name, va_hex, obj = m.group(1), m.group(2), (m.group(3) or "")
        va = int(va_hex, 16)
        if va < PREF_BASE:
            continue
        syms.append((va - PREF_BASE, name, obj))
    syms.sort()
    return syms

def lookup(syms, rva):
    lo, hi, best = 0, len(syms) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= rva:
            best = syms[mid]; lo = mid + 1
        else:
            hi = mid - 1
    return best

def main():
    if not MAP.exists():
        print("MAP not found:", MAP); return
    syms = load_syms()
    for arg in sys.argv[1:]:
        rva = int(arg, 16)
        b = lookup(syms, rva)
        if b:
            off = rva - b[0]
            print(f"RVA {hex(rva)} -> {b[1]} (+0x{off:X})  [{b[2]}]")
        else:
            print(f"RVA {hex(rva)} -> <no symbol>")

if __name__ == "__main__":
    main()
