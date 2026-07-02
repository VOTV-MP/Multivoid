#!/usr/bin/env python3
"""ue_skelmesh -- parse a VOTV cooked FSkeletalMeshRenderData region (generic, UE4.27).

Parses the render region of any UE4.27 cooked SkeletalMesh (property block auto-found,
RefSkeleton auto-located, LOD/sections/buffers count-driven) and reports whether it
reaches the payload end (round-trip). Used to validate the cook re-targets from kel_lmao
(6-bone proof) to the REAL client skin kerfurOmega_KelSkin (anthro, 101 bones). Splice
next. Spec: tools/client_model/SPEC.md.

Usage: python ue_skelmesh.py <uasset-path-without-ext> [--verbose]
Dev/RE tool (RULE 3).
"""
import struct
import sys

import ue_pkg

DEFAULT = r"D:\Projects\Programming\VOTV_MP\research\pak_re\extracted\VotV\Content\meshes\kerfurAnthro\sk\kerfurOmega_KelSkin"


def load(base):
    b = open(base + ".uasset", "rb").read()
    c = ue_pkg.parse_summary(b); kv = {t[1]: t[3] for t in c.tok if t[1]}
    o = kv["nameOffset"]; names = []
    for _ in range(kv["nameCount"]):
        ln = struct.unpack_from("<i", b, o)[0]; o += 4
        s = b[o:o + ln - 1].decode("latin1", "replace") if ln > 0 else ""; o += ln if ln > 0 else 0
        o += 4; names.append(s)
    uexp = open(base + ".uexp", "rb").read()
    return uexp[:-4], names   # drop trailing 4-byte package tag


class Cur:
    def __init__(self, b, o, names, verbose):
        self.b, self.o, self.names, self.v = b, o, names, verbose
    def log(self, *a):
        if self.v: print("  ", *a)
    def i32(self, k=""):
        x = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4
        if k: self.log(f"@{self.o-4} {k}={x}")
        return x
    def boolean(self, k=""):
        x = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4
        if k: self.log(f"@{self.o-4} {k}={x}")
        return x
    def i16(self, k=""):
        x = struct.unpack_from("<h", self.b, self.o)[0]; self.o += 2
        if k: self.log(f"@{self.o-2} {k}={x}")
        return x
    def u32(self, k=""):
        x = struct.unpack_from("<I", self.b, self.o)[0]; self.o += 4
        if k: self.log(f"@{self.o-4} {k}={x}")
        return x
    def strip(self, k="strip"):
        g, cl = self.b[self.o], self.b[self.o + 1]; self.o += 2
        if k: self.log(f"@{self.o-2} {k} g={g} c={cl}"); return g, cl
    def skip(self, n): self.o += n
    def arr(self, elem):
        n = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4 + n * elem; return n
    def bulk(self):
        es = struct.unpack_from("<i", self.b, self.o)[0]; n = struct.unpack_from("<i", self.b, self.o + 4)[0]
        self.o += 8 + es * n; return es, n
    def fname(self, o):
        idx, num = struct.unpack_from("<ii", self.b, o)
        return (self.names[idx] if 0 <= idx < len(self.names) else None), idx, num


def property_block_end(payload, names):
    """Parse tagged properties; return (offset past 'None', bHasVertexColors)."""
    o = 0; has_colors = False
    def fn(off):
        idx, num = struct.unpack_from("<ii", payload, off)
        return (names[idx] if 0 <= idx < len(names) else "?")
    for _ in range(500):
        nm = fn(o); o += 8
        if nm == "None":
            return o, has_colors
        ty = fn(o); o += 8
        size = struct.unpack_from("<i", payload, o)[0]; o += 8   # size + arrayindex
        if ty == "StructProperty":
            o += 8 + 16    # struct name FName + struct guid
        elif ty == "BoolProperty":
            if nm == "bHasVertexColors":
                has_colors = payload[o] != 0
            o += 1         # bool value byte
        elif ty in ("ByteProperty", "EnumProperty", "ArrayProperty"):
            o += 8         # inner/enum FName
        o += 1             # has-property-guid byte
        o += size
    raise RuntimeError("property 'None' not found")


def find_refskel(payload, names, start):
    """Locate FinalRefBoneInfo: i32 count N, then N FMeshBoneInfo(FName8+parent4), bone0 parent=-1."""
    nc = len(names)
    for o in range(start, min(start + 40000, len(payload) - 16)):
        n = struct.unpack_from("<i", payload, o)[0]
        if not (2 <= n <= 400):
            continue
        p0 = struct.unpack_from("<i", payload, o + 4 + 8)[0]
        if p0 != -1:
            continue
        ok = True
        for i in range(n):
            bo = o + 4 + i * 12
            ni = struct.unpack_from("<i", payload, bo)[0]
            pa = struct.unpack_from("<i", payload, bo + 8)[0]
            if not (0 <= ni < nc and -1 <= pa < n):
                ok = False; break
        if ok:
            return o, n
    raise RuntimeError("RefSkeleton not found")


def main(base=None):
    verbose = "--verbose" in sys.argv
    if base is None:
        args = [a for a in sys.argv[1:] if not a.startswith("--")]
        base = args[0] if args else DEFAULT
    payload, names = load(base)
    END = len(payload)
    print(f"{base.split(chr(92))[-1]}: payload {END} bytes")

    rstart, has_colors = property_block_end(payload, names)
    print(f"  property block ends @{rstart} -> render region [{rstart}:{END}]  bHasVertexColors={has_colors}")
    c = Cur(payload, rstart, names, verbose)
    c.strip("USkelMesh.strip"); c.skip(28)  # ImportedBounds
    rs, nb = find_refskel(payload, names, c.o)
    print(f"  materials keep-blob [{c.o}:{rs}] ({rs-c.o}B); RefSkeleton @{rs}, {nb} bones")
    c.o = rs
    nbi = c.i32(); c.skip(nbi * 12)          # FinalRefBoneInfo
    nbp = c.i32(); c.skip(nbp * 40)          # FinalRefBonePose (FTransform 40)
    nnm = c.i32(); c.skip(nnm * 12)          # FinalNameToIndexMap (FName8+i32)
    c.boolean("bCooked"); nlod = c.i32("numLODs")
    print(f"  bones={nbi} nameMap={nnm} numLODs={nlod}")

    for li in range(nlod):
        c.strip("lod.strip"); c.boolean("bIsLODCookedOut"); c.boolean("bInlined")
        c.arr(2)                              # RequiredBones
        nsec = c.i32("sections.count")
        sec = []
        for si in range(nsec):
            c.strip(); c.i16()               # sec strip, MaterialIndex
            bi = c.i32(); nt = c.i32()       # BaseIndex, NumTriangles
            c.boolean(); c.skip(1); c.boolean()  # bRecomputeTangent, RecomputeMaskChannel, bCastShadow
            c.u32()                          # BaseVertexIndex
            c.arr(1)                         # ClothMappingDataLODs
            nbm = c.arr(2)                   # BoneMap
            nv = c.i32(); mbi = c.i32()      # NumVertices, MaxBoneInfluences
            c.i16(); c.skip(20)              # CorrespondClothAssetIndex, ClothingData
            c.arr(4); c.arr(8)               # DupVertData, DupVertIndexData
            c.boolean()                      # bDisabled
            sec.append((nt, nv, nbm, mbi))
        c.arr(2)                             # ActiveBoneIndices
        c.skip(4)                            # buffersSize
        c.strip("streamed.strip")
        c.skip(1); _, ic = c.bulk()          # IndexBuffer (dataSize byte + bulk)
        c.i32(); pnv = c.i32(); c.bulk()     # PositionBuffer Stride, NumVerts, bulk
        c.strip(); ntc = c.i32(); c.i32()    # smvb strip, NumTexCoords, NumVertices
        c.boolean(); c.boolean()             # FullPrecUVs, HighPrecTangent
        c.bulk(); c.bulk()                   # tangents, texcoords
        c.strip(); c.boolean(); c.u32(); c.u32(); c.u32(); c.boolean()  # skinweight meta
        c.bulk()                             # newData
        c.strip(); c.i32(); c.bulk()         # lookup strip, numLookupVerts, lookupData
        if has_colors:
            c.strip(); c.i32(); c.i32(); c.bulk()   # FColorVertexBuffer: strip, Stride, NumVerts, bulk<FColor>
        c.skip(1); c.bulk()                  # AdjacencyIndexBuffer
        print(f"  LOD{li}: sections={nsec} verts={pnv} indices={ic} texcoords={ntc} maxBoneInf={max(x[3] for x in sec)}")
        for k, (nt, nv, nbm, mbi) in enumerate(sec):
            print(f"    sec{k}: tris={nt} verts={nv} boneMap={nbm} maxInf={mbi}")

    delta = END - c.o
    ok = 0 <= delta < 64
    tag = "ROUND-TRIP OK (tail keep-blob)" if ok else "DRIFT (color buffer / bHasVertexColors?)"
    print(f"  reached @{c.o} of {END}, remaining {delta}B -> {tag}")
    return ok


if __name__ == "__main__":
    main()
