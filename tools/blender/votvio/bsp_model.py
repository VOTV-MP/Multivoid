"""Cooked UModel (BSP) reader: the alpha bunker's walls/floors are level BSP,
rendered in-game by 61 ModelComponents -- no StaticMesh carries them.

UModel::Serialize (4.27, cooked) after the property block:
  FStripDataFlags (2 bytes)
  FBoxSphereBounds (28)
  Vectors  BulkSerialize (esz 12)   -- texture basis vectors
  Points   BulkSerialize (esz 12)   -- world-space positions
  Nodes    BulkSerialize (esz 64)   -- FBspNode: polygon fan per node
  Surfs    TArray (count + 56/each) -- FBspSurf: Material FPackageIndex + pBase,
                                       vTextureU, vTextureV (UV mapping basis)
  Verts    BulkSerialize (esz 24)   -- FVert: pVertex point index
Every bulk header is validated (ElementSize must match); a mismatch aborts the
parse with geometry=None rather than guessing.
"""
import struct

import numpy as np

from UE4Parse.Assets.Exports.ExportRegistry import register_export
from UE4Parse.Assets.Exports.UObjects import UObject


class UModelExport(UObject):
    bsp = None   # dict(points, polys, surfs) or None

    def __init__(self, reader):
        super().__init__(reader)
        self.bsp = None

    def deserialize(self, validpos):
        super().deserialize(validpos)
        bs = self.reader.base_stream
        cur = bs.tell()
        n = validpos - cur
        if n < 40:
            return
        raw = bs.read(n)
        # the property reader may or may not have consumed the serial-guid flag;
        # try the exact offset, then +4 (flag) and +20 (flag + guid)
        for skip in (0, 4, 20):
            try:
                self.bsp = _parse_model_tail(raw[skip:])
                return
            except Exception:  # noqa: BLE001 - never let a bad model kill the package
                continue
        self.bsp = None


def _bulk(raw, o, expect_esz):
    esz, cnt = struct.unpack_from("<ii", raw, o)
    if esz != expect_esz or cnt < 0 or o + 8 + esz * cnt > len(raw):
        raise ValueError(f"bulk header mismatch: esz={esz} cnt={cnt} at {o}")
    return o + 8, cnt, o + 8 + esz * cnt


def _parse_model_tail(raw):
    o = 2                       # FStripDataFlags
    o += 28                     # FBoxSphereBounds
    do, dn, o = _bulk(raw, o, 12)            # Vectors
    vectors = np.frombuffer(raw, "<f4", dn * 3, do).reshape(dn, 3).copy()
    do, dn, o = _bulk(raw, o, 12)            # Points
    points = np.frombuffer(raw, "<f4", dn * 3, do).reshape(dn, 3).copy()
    do, dn, o = _bulk(raw, o, 64)            # Nodes
    nodes_raw = raw[do:do + dn * 64]
    (scount,) = struct.unpack_from("<i", raw, o)  # Surfs: plain TArray
    o += 4
    if scount < 0 or o + scount * 56 > len(raw):
        raise ValueError("surf array out of range")
    surfs = []
    for si in range(scount):
        so = o + si * 56
        mat_idx, _flags, p_base, v_texu, v_texv = struct.unpack_from("<iiiii", raw, so)
        surfs.append((mat_idx, p_base, v_texu, v_texv))
    o += scount * 56
    do, dn, o = _bulk(raw, o, 24)            # Verts
    verts = np.frombuffer(raw, "<i4", dn * 6, do).reshape(dn, 6)[:, 0].copy()

    polys = []
    npoints = len(points)
    for b in range(0, len(nodes_raw), 64):
        i_vert_pool, i_surf = struct.unpack_from("<ii", nodes_raw, b + 16)
        num_verts = nodes_raw[b + 54]
        if num_verts < 3 or i_surf < 0 or i_surf >= scount:
            continue
        if i_vert_pool < 0 or i_vert_pool + num_verts > len(verts):
            continue
        idxs = verts[i_vert_pool:i_vert_pool + num_verts]
        if idxs.min() < 0 or idxs.max() >= npoints:
            continue
        polys.append((i_surf, idxs))
    return {"points": points, "vectors": vectors, "surfs": surfs, "polys": polys}


register_export(UModelExport, Type="Model")
