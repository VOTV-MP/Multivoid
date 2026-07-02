#!/usr/bin/env python3
"""ue_tex -- cook a PNG into a UE4.27 cooked UTexture2D package (pure Python, no editor).

Rung 2 of the texture ladder (docs/COOP_CLIENT_MODEL.md 7): produce
/Game/Mods/VOTVCoop/tex_scientist (tex_scientist.uasset/.uexp) from the game's own
tex_kel3_skin as the byte-layout template, with OUR pixels: PF_B8G8R8A8, 1 mip, fully
inline (no .ubulk). The runtime then binds it via MID SetTextureParameterValue('tex', ...).

Layout facts (empirically validated against tex_kel3_skin, 2026-07-02):
- uexp: [tagged props]['None'][bHasGuid u32][UTexture strip u8x2][UTexture2D strip u8x2]
  [bCooked u32=1][PixelFormatName FName][SkipOffset i64 = ABSOLUTE offset of the
  terminating 'None' FName (totalHeaderSize + uexp pos)][SizeX][SizeY][PackedData=NumSlices]
  [PixelFormat FString][FirstMipToSerialize=0][NumMips][per mip: bCooked u32=1,
  BulkDataFlags u32, ElementCount i32, SizeOnDisk i32, OffsetInFile i64 = ABSOLUTE file
  offset of the payload, (inline payload), SizeX, SizeY, SizeZ][bIsVirtual u32=0]
  ['None' FName][PACKAGE_FILE_TAG u32]
- inline mip flags 0x48 = BULKDATA_ForceInlinePayload|BULKDATA_SingleUse (the template's
  own tail mips use exactly this).
- name-map entry = FString + u16 NonCasePreservingHash + u16 CasePreservingHash where
  NonCase = Strihash_DEPRECATED(upper, low-byte-per-char, MSB table poly 0x04C11DB7) & 0xFFFF
  and Case = StrCrc32(reflected 0xEDB88320, each char as 4 LE bytes) & 0xFFFF.
  (Validated 19/19 both slots vs the template. The 8 formula in COOP_CLIENT_MODEL.md was
  wrong for the NonCase slot -- corrected here.)

Usage:
  python tools/client_model/ue_tex.py cook <in.png> <out_base>   # writes out_base.uasset/.uexp
  python tools/client_model/ue_tex.py selftest                   # hash + layout checks only
Dev/RE tool (RULE 3).
"""
import struct
import sys

import numpy as np
from PIL import Image

import ue_pkg

TEMPLATE = r"D:\Projects\Programming\VOTV_MP\research\pak_re\extracted\VotV\Content\meshes\kel\4\tex_kel3_skin"
# Model keeps its ORIGINAL name (user 2026-07-02): tex package follows the mesh.
NEW_PKG_PATH = "/Game/Mods/VOTVCoop/tex_hl_einstein_v1sc"
NEW_OBJ_NAME = "tex_hl_einstein_v1sc"
NEW_PF = "PF_B8G8R8A8"

# ---- FName hashes (validated 19/19 vs the template name map) ----
_MSB_TBL = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if _c & 0x80000000 else (_c << 1) & 0xFFFFFFFF
    _MSB_TBL.append(_c)

_REF_TBL = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _REF_TBL.append(_c)


def noncase_hash16(s):
    h = 0
    for ch in s:
        h = ((h >> 8) & 0x00FFFFFF) ^ _MSB_TBL[(h ^ (ord(ch.upper()) & 0xFF)) & 0xFF]
    return h & 0xFFFF


def case_hash16(s):
    crc = 0xFFFFFFFF
    for ch in s:
        cv = ord(ch)
        for _ in range(4):
            crc = (crc >> 8) ^ _REF_TBL[(crc ^ cv) & 0xFF]
            cv >>= 8
    return (~crc) & 0xFFFF


def name_entry(s):
    b = s.encode("latin1") + b"\x00"
    return struct.pack("<i", len(b)) + b + struct.pack("<HH", noncase_hash16(s), case_hash16(s))


# ---- template load ----
def load_template(template=TEMPLATE):
    ua = open(template + ".uasset", "rb").read()
    uexp = open(template + ".uexp", "rb").read()
    c = ue_pkg.parse_summary(ua)
    kv = {t[1]: t[3] for t in c.tok if t[1] and t[3] is not None}
    names, hashes = [], []
    o = kv["nameOffset"]
    for _ in range(kv["nameCount"]):
        ln = struct.unpack_from("<i", ua, o)[0]; o += 4
        names.append(ua[o:o + ln - 1].decode("latin1")); o += ln
        hashes.append(struct.unpack_from("<HH", ua, o)); o += 4
    return ua, uexp, c, kv, names, hashes, o  # o = name map END offset


def selftest(template=TEMPLATE):
    ua, uexp, c, kv, names, hashes, nm_end = load_template(template)
    ok = sum(1 for n, (h1, h2) in zip(names, hashes)
             if (noncase_hash16(n), case_hash16(n)) == (h1, h2))
    print(f"hash self-test: {ok}/{len(names)}")
    assert ok == len(names), "hash recipe drift"
    # layout anchors
    assert names[13] == "PF_DXT1" and names[11] == "None" and names[15] == "tex_kel3_skin"
    assert struct.unpack_from("<i", uexp, 49)[0] == 512, "ImportedSize.X anchor moved"
    assert uexp[171:179] == b"\x00\x00\x00\x00\x01\x00\x01\x00", "native tail anchor moved"
    print("layout anchors OK")
    return True


# ---- cook ----
def cook(png_path, out_base, pkg_path=NEW_PKG_PATH, obj_name=NEW_OBJ_NAME,
         template=TEMPLATE):
    ua, uexp, c, kv, names, hashes, nm_end = load_template(template)
    selftest(template)

    img = Image.open(png_path).convert("RGBA")
    W, H = img.size
    rgba = np.asarray(img, dtype=np.uint8)
    bgra = rgba[:, :, [2, 1, 0, 3]].tobytes()  # UE PF_B8G8R8A8 byte order
    print(f"cook: {png_path} {W}x{H} -> {pkg_path}.{obj_name} ({NEW_PF}, 1 mip, inline)")

    # -- new name map: 3 in-place renames, all hashes recomputed --
    new_names = list(names)
    new_names[0] = pkg_path            # was /Game/meshes/kel/4/tex_kel3_skin
    new_names[15] = obj_name           # was tex_kel3_skin
    new_names[13] = NEW_PF             # was PF_DXT1
    nm_blob = b"".join(name_entry(n) for n in new_names)
    delta = len(nm_blob) - (nm_end - kv["nameOffset"])

    # -- uexp: props (patch ImportedSize) + rebuilt native tail --
    props = bytearray(uexp[:171])          # tagged props + 'None' (ends @171 incl. bHasGuid? no: 171 = props end)
    struct.pack_into("<ii", props, 49, W, H)   # ImportedSize.X/.Y (anchor asserted above)
    new_ths = kv["totalHeaderSize"] + delta

    tail = bytearray()
    tail += struct.pack("<i", 0)               # bHasGuid
    tail += b"\x01\x00\x01\x00"                # UTexture + UTexture2D strip flags
    tail += struct.pack("<i", 1)               # bCooked
    tail += struct.pack("<ii", 13, 0)          # PixelFormatName FName -> new_names[13] = PF_B8G8R8A8
    skip_pos = len(props) + len(tail)
    tail += struct.pack("<q", 0)               # SkipOffset placeholder (abs offset of 'None', fixed below)
    tail += struct.pack("<ii", W, H)
    tail += struct.pack("<I", 1)               # PackedData: NumSlices=1
    pf = NEW_PF.encode("latin1") + b"\x00"
    tail += struct.pack("<i", len(pf)) + pf    # PixelFormat FString
    tail += struct.pack("<i", 0)               # FirstMipToSerialize
    tail += struct.pack("<i", 1)               # NumMips
    tail += struct.pack("<I", 1)               # mip.bCooked
    tail += struct.pack("<I", 0x48)            # flags: ForceInlinePayload|SingleUse
    tail += struct.pack("<ii", len(bgra), len(bgra))   # ElementCount, SizeOnDisk
    off_pos = len(props) + len(tail)
    tail += struct.pack("<q", 0)               # OffsetInFile placeholder (abs, fixed below)
    payload_pos = len(props) + len(tail)
    tail += bgra
    tail += struct.pack("<iii", W, H, 1)       # mip SizeX/Y/Z
    tail += struct.pack("<i", 0)               # bIsVirtual
    none_pos = len(props) + len(tail)
    tail += struct.pack("<ii", 11, 0)          # 'None' FName terminator
    tail += uexp[-4:]                          # PACKAGE_FILE_TAG

    new_uexp = bytes(props) + bytes(tail)
    new_uexp = bytearray(new_uexp)
    struct.pack_into("<q", new_uexp, skip_pos, new_ths + none_pos)
    struct.pack_into("<q", new_uexp, off_pos, new_ths + payload_pos)

    # -- uasset: summary(offsets+delta) + new name map + shifted remainder --
    for t in c.tok:
        kind, key, raw, val = t
        if key in ("totalHeaderSize", "exportOffset", "importOffset", "dependsOffset",
                   "assetRegistryDataOffset", "preloadDependencyOffset",
                   "gatherableOffset", "softRefOffset", "searchableNamesOffset",
                   "thumbnailTableOffset", "worldTileInfoDataOffset") and val and val > 0:
            t[2] = struct.pack("<i", val + delta); t[3] = val + delta
        elif key == "bulkDataStartOffset":
            # bulkDataStartOffset = end of (header + uexp payload); recompute fully
            v = new_ths + len(new_uexp) - 4
            t[2] = struct.pack("<q", v); t[3] = v
    head = ue_pkg.reserialize(c)
    mid = ua[c.o:kv["nameOffset"]]
    rest = bytearray(ua[nm_end:kv["totalHeaderSize"]])
    # patch export table inside `rest`: SerialSize @exportOffset+28, SerialOffset @+36
    exp_rel = kv["exportOffset"] - nm_end
    struct.pack_into("<q", rest, exp_rel + 28, len(new_uexp) - 4)
    struct.pack_into("<q", rest, exp_rel + 36, new_ths)

    new_ua = head + mid + nm_blob + bytes(rest)
    assert len(new_ua) == new_ths, f"header size mismatch {len(new_ua)} != {new_ths}"

    open(out_base + ".uasset", "wb").write(new_ua)
    open(out_base + ".uexp", "wb").write(bytes(new_uexp))
    print(f"  wrote {out_base}.uasset ({len(new_ua)}B) + .uexp ({len(new_uexp)}B) "
          f"[SkipOffset={new_ths + none_pos}, payload@{new_ths + payload_pos}, {len(bgra)}B pixels]")

    # -- re-parse own output (walk to the terminator) --
    b = bytes(new_uexp)
    o2 = 171 + 4 + 4 + 4
    idx, num = struct.unpack_from("<ii", b, o2); o2 += 8
    assert new_names[idx] == NEW_PF, "PF FName wrong"
    o2 += 8  # skip offset
    sx, sy = struct.unpack_from("<ii", b, o2); o2 += 8
    assert (sx, sy) == (W, H)
    o2 += 4
    ln = struct.unpack_from("<i", b, o2)[0]; o2 += 4 + ln
    o2 += 8  # firstmip + nummips
    o2 += 16  # mip bCooked+flags+ec+sod
    o2 += 8   # offset
    o2 += len(bgra) + 12 + 4
    idx, num = struct.unpack_from("<ii", b, o2)
    assert new_names[idx] == "None", "terminator misplaced"
    print("  re-parse OK (PF/dims/terminator verified)")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        selftest()
    elif len(sys.argv) >= 4 and sys.argv[1] == "cook":
        cook(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
