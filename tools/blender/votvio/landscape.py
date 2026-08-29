"""Landscape rebuild: 256 LandscapeComponents -> displaced grid meshes.

Measured (untitled_1): ComponentSizeQuads=126, NumSubsections=2, SubsectionSizeQuads=63,
heightmaps = PF_B8G8R8A8 Texture2D exports INSIDE the umap package; height = (R<<8|G),
world Z = (h - 32768) * scaleZ / 128 (UE's LANDSCAPE_ZSCALE). Subsection texel packing:
each subsection block is (SubsectionSizeQuads+1) px wide, so local vert x -> texel
sub*(ssq+1) + (x - sub*ssq), plus the component's HeightmapScaleBias UV offset.
"""
import numpy as np

import bpy

from . import bc_decode, convert


def _ref_name(v):
    return str(v.get("ObjectName", "")) if isinstance(v, dict) else ""


def _decoded_heightmap(pkg, name, cache, warnings):
    if name in cache:
        return cache[name]
    img = None
    for ex in pkg.ExportMap:
        if str(ex.ObjectName) == name:
            obj = getattr(ex, "exportObject", None)
            data = getattr(obj, "data", None)
            if data:
                pd = data[0]
                mip = next((m for m in pd.Mips
                            if getattr(getattr(m, "BulkData", None), "Data", None)), None)
                if mip is not None:
                    img = bc_decode.decode_pixels(
                        pd.PixelFormat.name, bytes(mip.BulkData.Data),
                        int(mip.SizeX), int(mip.SizeY))
            break
    if img is None:
        warnings.append(f"landscape heightmap missing: {name}")
    cache[name] = img
    return img


def build_landscape(game, map_path, dicts, collection, warnings, material):
    pkg = game.load_package(map_path)

    # the Landscape actor's root transform (scale is the height/extent unit)
    root_loc = (0.0, 0.0, 0.0)
    root_scale = (100.0, 100.0, 100.0)
    by_name = {e.get("Name"): e for e in dicts if isinstance(e, dict)}
    land = next((e for e in dicts if isinstance(e, dict) and e.get("Type") == "Landscape"), None)
    if land is not None:
        root = by_name.get(_ref_name((land.get("Properties") or {}).get("RootComponent")))
        if root:
            p = root.get("Properties") or {}
            rl = p.get("RelativeLocation")
            rs = p.get("RelativeScale3D")
            if isinstance(rl, dict):
                root_loc = (rl.get("X", 0.0), rl.get("Y", 0.0), rl.get("Z", 0.0))
            if isinstance(rs, dict):
                root_scale = (rs.get("X", 100.0), rs.get("Y", 100.0), rs.get("Z", 100.0))

    hm_cache = {}
    built = 0
    for e in dicts:
        if not isinstance(e, dict) or e.get("Type") != "LandscapeComponent":
            continue
        p = e.get("Properties") or {}
        csq = int(p.get("ComponentSizeQuads", 126))
        ssq = int(p.get("SubsectionSizeQuads", 63))
        sb = p.get("HeightmapScaleBias") or {}
        bias_u, bias_v = float(sb.get("Z", 0.0)), float(sb.get("W", 0.0))
        inv_w, inv_h = float(sb.get("X", 0.0)), float(sb.get("Y", 0.0))
        rl = p.get("RelativeLocation") or {}
        base_x = float(rl.get("X", p.get("SectionBaseX", 0) or 0))
        base_y = float(rl.get("Y", p.get("SectionBaseY", 0) or 0))
        hm = _decoded_heightmap(pkg, _ref_name(p.get("HeightmapTexture")), hm_cache, warnings)
        if hm is None:
            continue
        tex_h, tex_w = hm.shape[0], hm.shape[1]
        px0 = int(round(bias_u * tex_w)) if inv_w else 0
        py0 = int(round(bias_v * tex_h)) if inv_h else 0

        nverts = csq + 1
        xs = np.arange(nverts)
        sub = np.minimum(xs // ssq, (csq // ssq) - 1)
        texc = px0 + sub * (ssq + 1) + (xs - sub * ssq)
        texr = py0 + sub * (ssq + 1) + (xs - sub * ssq)
        tc = np.clip(texc, 0, tex_w - 1)
        tr = np.clip(texr, 0, tex_h - 1)
        block = hm[np.ix_(tr, tc)]                      # (n, n, 4) float 0..1
        hval = block[:, :, 0] * 255.0 * 256.0 + block[:, :, 1] * 255.0
        z_uu = (hval - 32768.0) * (root_scale[2] / 128.0)

        gy, gx = np.meshgrid(np.arange(nverts), np.arange(nverts), indexing="ij")
        wx = root_loc[0] + (base_x + gx) * root_scale[0]
        wy = root_loc[1] + (base_y + gy) * root_scale[1]
        wz = root_loc[2] + z_uu
        verts = np.stack([wx * convert.SCALE, -wy * convert.SCALE, wz * convert.SCALE],
                         axis=-1).reshape(-1, 3)

        i = (gy[:-1, :-1] * nverts + gx[:-1, :-1]).ravel()
        faces = np.stack([i, i + nverts, i + nverts + 1, i + 1], axis=-1)  # mirrored winding

        me = bpy.data.meshes.new(f"landscape_{e.get('Name')}")
        me.from_pydata(verts.tolist(), [], faces.tolist())
        me.validate()
        if material is not None:
            me.materials.append(material)
        me.polygons.foreach_set("use_smooth", np.ones(len(me.polygons), dtype=bool))
        ob = bpy.data.objects.new(me.name, me)
        collection.objects.link(ob)
        built += 1
    return built
