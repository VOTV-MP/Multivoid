import struct, sys

path = sys.argv[1]
data = open(path, "rb").read()

sig, ver, nstreams, diroff = struct.unpack_from("<IIII", data, 0)[0:4]
assert sig == 0x504D444D, hex(sig)  # 'MDMP'

streams = {}
for i in range(nstreams):
    st, sz, rva = struct.unpack_from("<III", data, diroff + i * 12)
    streams.setdefault(st, []).append((sz, rva))

def read_modules():
    mods = []
    if 4 not in streams:  # ModuleListStream
        return mods
    sz, rva = streams[4][0]
    (count,) = struct.unpack_from("<I", data, rva)
    off = rva + 4
    for i in range(count):
        base, size, chksum, tds = struct.unpack_from("<QIII", data, off)
        name_rva = struct.unpack_from("<I", data, off + 0x14)[0]
        (nlen,) = struct.unpack_from("<I", data, name_rva)
        name = data[name_rva + 4:name_rva + 4 + nlen].decode("utf-16-le", "replace")
        mods.append((base, size, name))
        off += 108  # sizeof(MINIDUMP_MODULE)
    return mods

mods = read_modules()

def resolve(addr):
    for base, size, name in mods:
        if base <= addr < base + size:
            short = name.rsplit("\\", 1)[-1]
            if short.lower() == "main.dll":
                short = f"main.dll[base=0x{base:X},sz=0x{size:X}]"
            return f"{short}+0x{addr-base:X}"
    return f"0x{addr:X} (unmapped)"

# ExceptionStream = 6
if 6 in streams:
    sz, rva = streams[6][0]
    tid, _align = struct.unpack_from("<II", data, rva)
    code, flags, rec, addr = struct.unpack_from("<IIQQ", data, rva + 8)
    nparams = struct.unpack_from("<I", data, rva + 8 + 24)[0]
    params = struct.unpack_from("<8Q", data, rva + 8 + 32)
    print(f"exception: code=0x{code:08X} thread={tid} at {resolve(addr)}")
    if code == 0xC0000005 and nparams >= 2:
        kind = {0: "READ", 1: "WRITE", 8: "EXEC(DEP)"}.get(params[0], str(params[0]))
        print(f"  AV {kind} of address 0x{params[1]:X}")
    # thread context for stack walk (rough): ContextRecord rva
    ctx_sz, ctx_rva = struct.unpack_from("<II", data, rva + 8 + 152)  # after MINIDUMP_EXCEPTION (152 B)
    # x64 CONTEXT: Rip @ 0xF8, Rsp @ 0x98
    rip = struct.unpack_from("<Q", data, ctx_rva + 0xF8)[0]
    rsp = struct.unpack_from("<Q", data, ctx_rva + 0x98)[0]
    print(f"  RIP={resolve(rip)}  RSP=0x{rsp:X}")
    # naive stack scan via MemoryListStream (5) -- minidumps carry thread stacks there
    if 5 in streams:
        msz, mrva = streams[5][0]
        (cnt,) = struct.unpack_from("<I", data, mrva)
        off = mrva + 4
        found = None
        for i in range(cnt):
            start, dsz, drva = struct.unpack_from("<QII", data, off + i * 16)
            if start <= rsp < start + dsz:
                found = (start, dsz, drva)
                break
        if found:
            start, dsz, drva = found
            print("  stack scan (module-resolving qwords):")
            n = 0
            for i in range(0, min(0x3000, start + dsz - rsp), 8):
                (q,) = struct.unpack_from("<Q", data, drva + (rsp - start) + i)
                r = resolve(q)
                if "unmapped" not in r and (".exe" in r or ".dll" in r):
                    print(f"    rsp+0x{i:04X}: {r}")
                    n += 1
                    if n >= 30:
                        break
        else:
            print("  (no MemoryList range contains RSP)")
    # full-dump fallback: Memory64ListStream=9
    if 9 in streams:
        msz, mrva = streams[9][0]
        cnt, base_rva = struct.unpack_from("<QQ", data, mrva)
        off = mrva + 16
        run = base_rva
        found = None
        ranges = []
        for i in range(cnt):
            start, size = struct.unpack_from("<QQ", data, off + i * 16)
            ranges.append((start, size, run))
            run += size
        for start, size, frva in ranges:
            if start <= rsp < start + size:
                found = (start, size, frva)
                break
        if found:
            start, size, frva = found
            print("  stack scan (module-resolving qwords):")
            n = 0
            for i in range(0, min(0x2000, start + size - rsp), 8):
                (q,) = struct.unpack_from("<Q", data, frva + (rsp - start) + i)
                r = resolve(q)
                if "unmapped" not in r and (".exe" in r or ".dll" in r):
                    print(f"    rsp+0x{i:04X}: {r}")
                    n += 1
                    if n >= 25:
                        break
else:
    print("no exception stream")

print("\nmodules of interest:")
for base, size, name in mods:
    short = name.rsplit("\\", 1)[-1].lower()
    if any(k in short for k in ("main.dll", "ue4ss", "shim", "xinput", "votv", "dwmapi", "crash")):
        print(f"  {name.rsplit(chr(92),1)[-1]}  base=0x{base:X} size=0x{size:X}")
