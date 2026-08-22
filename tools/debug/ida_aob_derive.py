# ida_aob_derive.py -- derive a UNIQUE AOB signature for a function, and locate UE
# engine seams by their __FILE__ string. Headless IDAPython, READ-ONLY (no DB edits).
#
# WHY THIS EXISTS: `docs/VERSION_MIGRATION.md` says the AOB signatures are the part of
# a version migration that needs a disassembler. This is that work, mechanized: point it
# at a function address (or let it find one from a UE source-path string) and it prints a
# signature that is PROVEN unique across the image, with build-variable displacements
# wildcarded. Used 2026-08-22 to derive `kSigD3D11ViewportPresentChecked`
# (docs/OVERLAY_CAPTURE_COEXIST.md §6b).
#
# USAGE (Windows, from the folder holding the .i64):
#   "C:\Program Files\IDA Professional 9.2\idat.exe" -A \
#       -S"<repo>\tools\debug\ida_aob_derive.py <mode> <arg>" -L<log> VotV-Win64-Shipping.exe.i64
#
#   mode = "sig"  arg = 0x<function ea>   -> emit a unique AOB for that function
#   mode = "file" arg = <substring>       -> list functions xref'ing a UE __FILE__ string
#                                            (e.g. "WindowsD3D11Viewport.cpp"), which is how
#                                            you FIND the function in the first place
#
# Output goes to <same dir as this script>/ida_aob_derive.txt (IDA's console is awkward
# to capture headless).
#
# THE METHOD (why the output is trustworthy):
#  - UE ships __FILE__ strings into shipping builds for its check/verify macros, so an
#    engine source file name is a reliable anchor into a subsystem's functions.
#  - A prologue is NOT automatically unique: MSVC emits the same
#    push/sub-rsp/GS-cookie preamble in hundreds of functions. This script therefore
#    GROWS the window until the occurrence count across the whole executable segment
#    is exactly 1, and reports the count at each length so you can see the margin.
#  - Any `mov rax,[rip+disp32]` (the /GS cookie load, 48 8B 05 xx xx xx xx) is
#    WILDCARDED -- its displacement moves with every rebuild, so leaving it literal
#    would produce a signature that breaks on a rebuild that did not touch the function.

import idautils
import idaapi
import idc
import ida_bytes
import ida_funcs
import ida_segment
import ida_xref
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ida_aob_derive.txt")
_lines = []


def w(s):
    _lines.append(str(s))


def _exec_seg():
    for segea in idautils.Segments():
        s = ida_segment.getseg(segea)
        if s.perm & ida_segment.SEGPERM_EXEC:
            return s
    return None


_SEG = _exec_seg()
_DATA = ida_bytes.get_bytes(_SEG.start_ea, _SEG.end_ea - _SEG.start_ea) if _SEG else b""


def _count(sig, mask):
    """Occurrences of a masked byte pattern in the exec segment."""
    n = len(sig)
    if n == 0:
        return 0
    hits = 0
    first = bytes([sig[0]])
    idx = _DATA.find(first)
    while idx >= 0 and idx + n <= len(_DATA):
        ok = True
        for k in range(n):
            if mask[k] and _DATA[idx + k] != sig[k]:
                ok = False
                break
        if ok:
            hits += 1
        idx = _DATA.find(first, idx + 1)
    return hits


def _mask_rip_loads(b):
    """Wildcard the disp32 of every `48 8B 05 xx xx xx xx` (mov rax,[rip+..]) -- the
    /GS cookie load and friends move on every rebuild."""
    m = [True] * len(b)
    for i in range(len(b) - 6):
        if b[i] == 0x48 and b[i + 1] == 0x8B and b[i + 2] == 0x05:
            for j in range(i + 3, min(i + 7, len(b))):
                m[j] = False
    return m


def emit_sig(ea, maxlen=96):
    f = ida_funcs.get_func(ea)
    if not f:
        w("ERROR: 0x%X is not inside a function" % ea)
        return
    w("function 0x%X (%s) size=%d" % (f.start_ea, idc.get_func_name(f.start_ea),
                                      f.end_ea - f.start_ea))
    raw = ida_bytes.get_bytes(f.start_ea, min(maxlen, f.end_ea - f.start_ea))
    w("raw bytes: %s" % " ".join("%02X" % x for x in raw))
    w("")
    w("length -> occurrences (1 == unique; grow until 1, then keep a margin):")
    winner = None
    for n in range(16, len(raw) + 1, 8):
        b = list(raw[:n])
        m = _mask_rip_loads(b)
        c = _count(b, m)
        sig = " ".join("??" if not m[k] else "%02X" % b[k] for k in range(n))
        w("  len=%-3d occ=%d" % (n, c))
        if c == 1 and winner is None:
            winner = (n, sig)
    if winner:
        # Re-emit one step longer than first-unique, for margin, if available.
        n_margin = min(winner[0] + 8, len(raw))
        b = list(raw[:n_margin])
        m = _mask_rip_loads(b)
        sig_margin = " ".join("??" if not m[k] else "%02X" % b[k] for k in range(n_margin))
        w("")
        w("FIRST UNIQUE at len=%d:" % winner[0])
        w("  %s" % winner[1])
        w("")
        w("RECOMMENDED (first-unique + 8 bytes of margin, len=%d, occ=%d):"
          % (n_margin, _count(b, m)))
        w("  %s" % sig_margin)
        w("")
        w("Paste into sdk_profile.h as a C string; keep the `??` wildcards.")
    else:
        w("")
        w("NO UNIQUE WINDOW within %d bytes -- this function's prologue is shared." % len(raw))
        w("Pick a different anchor (a distinctive mid-body instruction) or a different function.")


def find_by_file(needle):
    w("functions referencing a UE __FILE__ string containing %r:" % needle)
    hits = 0
    for s in idautils.Strings():
        try:
            txt = str(s)
        except Exception:
            continue
        if needle.lower() not in txt.lower():
            continue
        hits += 1
        w("\n  STR @0x%X : %s" % (s.ea, txt if len(txt) < 100 else txt[:100] + "..."))
        seen = set()
        r = ida_xref.get_first_dref_to(s.ea)
        while r != idaapi.BADADDR:
            f = ida_funcs.get_func(r)
            if f and f.start_ea not in seen:
                seen.add(f.start_ea)
                w("     FUNC 0x%X size=%-6d %s"
                  % (f.start_ea, f.end_ea - f.start_ea, idc.get_func_name(f.start_ea)))
            r = ida_xref.get_next_dref_to(s.ea, r)
    if not hits:
        w("  (no string matched)")


def main():
    args = idc.ARGV[1:] if len(idc.ARGV) > 1 else []
    w("=== ida_aob_derive ===")
    w("input: %s" % idaapi.get_input_file_path())
    w("image base: 0x%X" % idaapi.get_imagebase())
    w("args: %r" % (args,))
    w("")
    if len(args) >= 2 and args[0] == "sig":
        emit_sig(int(args[1], 16))
    elif len(args) >= 2 and args[0] == "file":
        find_by_file(args[1])
    else:
        w("usage: -S\"ida_aob_derive.py sig 0x<ea>\"  |  -S\"ida_aob_derive.py file <substring>\"")
        w("")
        w("example (the 2026-08-22 overlay seam, docs/OVERLAY_CAPTURE_COEXIST.md):")
        w("  file WindowsD3D11Viewport.cpp   -> finds the D3D11 viewport present cluster")
        w("  sig 0x1416F4BA0                 -> emits the PresentChecked signature")
    with open(OUT, "w") as fp:
        fp.write("\n".join(_lines))
    print("WROTE %s" % OUT)
    idc.qexit(0)


main()
