#!/usr/bin/env python3
"""abi_gate -- assert the shipped mod DLL imports NOTHING from UE4SS.

WHY THIS EXISTS. D-3 (docs/UE4SS_ARC.md section 0) chose the C-ABI loading contract
(`start_mod`/`uninstall_mod`) over UE4SS's `CppUserModBase` C++ vtable, because that vtable
and UE4SS's exported C++ surface are ABI-unstable across UE4SS builds. The entire measured
payoff of that choice is a NUMBER: `[V]` field C++ mods import 32/40/130 mangled symbols
from `UE4SS.dll`; Multivoid imports ZERO (UE4SS_ARC section 7.2b).

Until this file existed, that number was enforced by NOTHING. `[V]` `mp.py` `_lane_check`
asserts only that the boot line says `entry=cppmod`, that the retired proxy line is absent,
and that no REFUSE fired -- none of which can see an added `#include` that pulls in a UE4SS
export. A single future commit could take the dependency and no gate, test or review step
in the tree would notice until a user on a different UE4SS build reported a mod that does
not load.

THAT FAILURE IS NOT HYPOTHETICAL AND WE HAVE THE COUNTER-EXAMPLE ON DISK. `[V]` 2026-08-26:
DebugMod 5.0.3 uses `CppUserModBase`, imports UE4SS's C++ symbols, and fails to load with
`0x7f` (ERROR_PROC_NOT_FOUND) on the pinned UE4SS 3.0.1 -- while the SAME binary starts on
the experimental build, and Multivoid starts on both. See docs/DebugMod_ARC.md section 6a.
That binary is this gate's RED control: `--selftest` runs the gate against it and REQUIRES
a violation, because a gate that has never been shown failing is not evidence of anything.

TWO RED ARMS, deliberately: `--selftest <path>` is the FIELD-shaped control (a real foreign
binary this parser must flag) and needs that binary on disk, so it cannot run in CI.
`--drill` is the CI must-fire control, same family as the tree's text gates ("shown RED by
injection each run"): it synthesizes a minimal, structurally valid PE32+ whose import
directory names ue4ss.dll, runs the SAME check() on it, and fails the run unless the
violation is flagged. A drill that synthesizes its own violator proves the parse+match path
can fire; the one-time DebugMod selftest proves it fires on the real-world shape.

WHAT IT CHECKS, precisely: the PE IMPORT DIRECTORY -- the list of DLLs the loader must
resolve at load time. It deliberately does NOT grep the file for the string "UE4SS": our own
binary legitimately carries that text (`LogUe4ssPresence` logs whether UE4SS is present), so
a string scan would report a violation that is not one. The import table is the thing the
Windows loader actually acts on, and it is what produced DebugMod's 0x7f.

Delay-loaded imports are checked too (directory 13): a delay-load is still a hard dependency
on that DLL's exports, it just fails later and less legibly.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# The forbidden dependency. Matched case-insensitively -- the import table stores whatever
# name the linker recorded, and Windows resolves it case-insensitively.
FORBIDDEN = ("ue4ss.dll",)

IMPORT_DIR = 1
DELAY_IMPORT_DIR = 13


class PEError(Exception):
    pass


def _sections(data: bytes, pe: int, n_sections: int, opt_size: int):
    """(virtual_address, virtual_size, raw_ptr, raw_size) per section."""
    base = pe + 24 + opt_size
    out = []
    for i in range(n_sections):
        o = base + i * 40
        if o + 40 > len(data):
            raise PEError("section header past EOF")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
        out.append((vaddr, vsize, rptr, rsize))
    return out


def _rva_to_off(secs, rva: int):
    for vaddr, vsize, rptr, rsize in secs:
        # Use the RAW size as the upper bound where it is larger: a section's virtual size
        # can be smaller than what is on disk (alignment padding), and the import table
        # legitimately lands in that padding on some linkers.
        span = max(vsize, rsize)
        if vaddr <= rva < vaddr + span:
            off = rptr + (rva - vaddr)
            return off if off < len(data_len_guard[0]) else None
    return None


data_len_guard = [b""]  # set by imported_dlls; keeps _rva_to_off's bounds check honest


def _cstr(data: bytes, off: int) -> str:
    end = data.find(b"\0", off)
    if end < 0:
        raise PEError("unterminated name string")
    return data[off:end].decode("latin-1")


def imported_dlls(path: Path) -> list[str]:
    """Every DLL name in the PE import + delay-import directories."""
    data = path.read_bytes()
    data_len_guard[0] = data

    if data[:2] != b"MZ":
        raise PEError(f"{path.name}: not a PE (no MZ)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise PEError(f"{path.name}: no PE signature at e_lfanew")

    n_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic not in (0x10B, 0x20B):
        raise PEError(f"{path.name}: unknown optional-header magic {magic:#x}")
    # Data directories start after the optional header's fixed part: 96 bytes for PE32,
    # 112 for PE32+ (the extra 16 are the four ULONGLONG stack/heap fields).
    ddir = pe + 24 + (112 if magic == 0x20B else 96)

    secs = _sections(data, pe, n_sections, opt_size)
    names: list[str] = []

    for which, entry_size, name_field in ((IMPORT_DIR, 20, 12), (DELAY_IMPORT_DIR, 32, 4)):
        rva, size = struct.unpack_from("<II", data, ddir + which * 8)
        if rva == 0 or size == 0:
            continue
        off = _rva_to_off(secs, rva)
        if off is None:
            raise PEError(f"{path.name}: directory {which} RVA {rva:#x} maps to no section")
        while True:
            if off + entry_size > len(data):
                raise PEError(f"{path.name}: directory {which} runs past EOF")
            entry = data[off:off + entry_size]
            if entry == b"\0" * entry_size:
                break                                  # the null terminator entry
            name_rva = struct.unpack_from("<I", data, off + name_field)[0]
            if name_rva == 0:
                break
            name_off = _rva_to_off(secs, name_rva)
            if name_off is None:
                raise PEError(f"{path.name}: name RVA {name_rva:#x} maps to no section")
            names.append(_cstr(data, name_off))
            off += entry_size

    return names


def _synth_violator() -> bytes:
    """A minimal, structurally valid PE32+ DLL whose import directory names ue4ss.dll.

    The --drill must-fire control. Kept structurally honest -- a real descriptor with a
    real ILT/IAT and hint/name entry, not just the one field our parser happens to read --
    so the drill does not degenerate into testing the parser's shortcuts against itself.
    """
    file_align, sect_align = 0x200, 0x1000
    pe_off = 0x80
    opt_size = 112 + 16 * 8                     # PE32+ fixed part + 16 data directories

    # --- section payload (RVA 0x1000, raw offset 0x200) ---------------------------
    # 0x00 import descriptor: OFT=0x1050  Name=0x1040  FT=0x1060
    # 0x14 null descriptor
    # 0x40 "ue4ss.dll\0"    0x50 ILT[2]    0x60 IAT[2]    0x70 hint/name
    sect = bytearray(0x200)
    struct.pack_into("<IIIII", sect, 0x00, 0x1050, 0, 0, 0x1040, 0x1060)
    sect[0x40:0x4A] = b"ue4ss.dll\0"
    struct.pack_into("<Q", sect, 0x50, 0x1070)
    struct.pack_into("<Q", sect, 0x60, 0x1070)
    sect[0x70:0x72] = b"\0\0"                   # hint
    sect[0x72:0x82] = b"start_something\0"

    # --- headers ------------------------------------------------------------------
    hdr = bytearray(file_align)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, pe_off)
    hdr[pe_off:pe_off + 4] = b"PE\0\0"
    # COFF: x64, 1 section, characteristics EXECUTABLE|DLL|LARGE_ADDRESS_AWARE
    struct.pack_into("<HHIIIHH", hdr, pe_off + 4, 0x8664, 1, 0, 0, 0, opt_size, 0x2022)
    opt = pe_off + 24
    struct.pack_into("<H", hdr, opt, 0x20B)                       # magic PE32+
    struct.pack_into("<I", hdr, opt + 16, 0x1000)                 # AddressOfEntryPoint (unused)
    struct.pack_into("<Q", hdr, opt + 24, 0x180000000)            # ImageBase
    struct.pack_into("<II", hdr, opt + 32, sect_align, file_align)
    struct.pack_into("<H", hdr, opt + 48, 6)                      # MajorSubsystemVersion
    struct.pack_into("<II", hdr, opt + 56, 0x2000, file_align)    # SizeOfImage, SizeOfHeaders
    struct.pack_into("<H", hdr, opt + 68, 2)                      # Subsystem: GUI
    struct.pack_into("<I", hdr, opt + 108, 16)                    # NumberOfRvaAndSizes
    ddir = opt + 112
    struct.pack_into("<II", hdr, ddir + IMPORT_DIR * 8, 0x1000, 0x28)
    sh = opt + opt_size                                           # the one section header
    hdr[sh:sh + 8] = b".idata\0\0"
    struct.pack_into("<IIII", hdr, sh + 8, len(sect), 0x1000, len(sect), file_align)
    struct.pack_into("<I", hdr, sh + 36, 0xC0000040)              # INITIALIZED_DATA|R|W

    return bytes(hdr) + bytes(sect)


def check(path: Path, verbose: bool) -> list[str]:
    """Returns the list of FORBIDDEN dlls this binary imports (empty == clean)."""
    dlls = imported_dlls(path)
    if verbose:
        print(f"  {path.name}: {len(dlls)} imported DLL(s)")
        for d in sorted(dlls):
            print(f"      {d}")
    return [d for d in dlls if d.lower() in FORBIDDEN]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dll", nargs="?", help="path to the shipped main.dll")
    ap.add_argument("--selftest", metavar="VIOLATOR",
                    help="path to a binary that MUST violate the invariant (the field-shaped "
                         "RED control). A gate that has never been shown failing proves nothing.")
    ap.add_argument("--drill", action="store_true",
                    help="CI must-fire control: synthesize a violator PE in a temp file and "
                         "require the gate to flag it (no foreign binary needed).")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    rc = 0

    if args.drill:
        import tempfile
        tmp = Path(tempfile.gettempdir()) / "abi_gate_drill_violator.dll"
        tmp.write_bytes(_synth_violator())
        try:
            bad = check(tmp, args.verbose)
        except PEError as e:
            print(f"DRILL FAIL: parser rejected the synthesized violator: {e}")
            return 2
        finally:
            try:
                tmp.unlink()
            except OSError:
                pass
        if bad:
            print(f"DRILL PASS: synthesized violator flagged ({', '.join(bad)}) -- the gate can fire")
        else:
            print("DRILL FAIL: synthesized violator reported CLEAN -- the gate is blind")
            return 2

    if args.selftest:
        p = Path(args.selftest)
        if not p.is_file():
            print(f"SELFTEST INCONCLUSIVE: RED control not found at {p}")
            print("  (the control is DebugMod's main.dll -- see docs/DebugMod_ARC.md section 6a)")
            rc = 2
        else:
            try:
                bad = check(p, args.verbose)
            except PEError as e:
                print(f"SELFTEST FAIL: parser rejected the RED control: {e}")
                return 2
            if bad:
                print(f"SELFTEST PASS: RED control {p.name} violates as expected ({', '.join(bad)})")
            else:
                # This is the important branch. If the control comes back clean, the gate is
                # blind and every GREEN verdict it has ever printed is worthless.
                print(f"SELFTEST FAIL: RED control {p.name} reported CLEAN -- the gate is blind")
                return 2

    if args.dll:
        p = Path(args.dll)
        if not p.is_file():
            print(f"FAIL: {p} not found")
            return 2
        try:
            bad = check(p, args.verbose)
        except PEError as e:
            print(f"FAIL: {e}")
            return 2
        if bad:
            print(f"FAIL: {p.name} imports {', '.join(bad)} -- D-3's C-ABI invariant is BROKEN.")
            print("  Multivoid must not depend on UE4SS's exported C++ surface: it is unstable")
            print("  across UE4SS builds, and a mod that takes it stops loading when the user's")
            print("  UE4SS differs from the one it was linked against (docs/DebugMod_ARC.md 6a).")
            rc = 1
        else:
            print(f"PASS: {p.name} imports nothing from UE4SS (C-ABI contract intact)")

    if not args.dll and not args.selftest and not args.drill:
        ap.error("nothing to do: pass a dll, --selftest, --drill, or a combination")
    return rc


if __name__ == "__main__":
    sys.exit(main())
