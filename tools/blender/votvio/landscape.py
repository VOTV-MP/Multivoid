"""Landscape rebuild: 256 LandscapeComponents -> displaced grid meshes with the
game's own layer materials.

Measured (untitled_1): ComponentSizeQuads=126, NumSubsections=2, SubsectionSizeQuads=63,
heightmaps = PF_B8G8R8A8 Texture2D exports INSIDE the umap package; height = (R<<8|G),
world Z = (h - 32768) * scaleZ / 128 (UE's LANDSCAPE_ZSCALE). Subsection texel packing:
each subsection block is (SubsectionSizeQuads+1) px wide, so local vert x -> texel
sub*(ssq+1) + (x - sub*ssq), plus the component's HeightmapScaleBias UV offset.

Weightmaps ride the same packing (128x128 B8G8R8A8 exports, WeightmapScaleBias
Z/W is a half-texel center offset -> block origin 0): each WeightmapLayerAllocation
names (LayerInfo, texture index, channel). Census: grass everywhere, gravel/dirt/
rock/sand patches; 'DataLayer' is the landscape hole (visibility) mask - its
quads are CARVED from the grid (the bunker entrance stairwell), never painted.
The layer detail textures + world tiling come from inst_mainLandscape via
materials.landscape_layer_specs.
"""
import numpy as np

import bpy

from . import bc_decode, convert
from . import materials as materials_mod


def _ref_name(v):
    return str(v.get("ObjectName", "")) if isinstance(v, dict) else ""


def _decoded_tex(pkg, name, cache, warnings, label):
    if name in cache:
        return cache[name]
    # one name->export index per package (a linear ExportMap scan per texture
    # is ~300 x tens-of-thousands of str() conversions on the full map)
    index = cache.get("__index__")
    if index is None:
        index = {str(ex.ObjectName): ex for ex in pkg.ExportMap}
        cache["__index__"] = index
    img = None
    ex = index.get(name)
    if ex is not None:
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
    if img is None:
        warnings.append(f"landscape {label} missing: {name}")
    cache[name] = img
    return img


def _block_indices(nverts, ssq, csq, px0, py0, tex_w, tex_h):
    """Component-local vert (row, col) -> packed texel (row, col) index arrays."""
    xs = np.arange(nverts)
    sub = np.minimum(xs // ssq, (csq // ssq) - 1)
    texc = px0 + sub * (ssq + 1) + (xs - sub * ssq)
    texr = py0 + sub * (ssq + 1) + (xs - sub * ssq)
    return np.clip(texr, 0, tex_h - 1), np.clip(texc, 0, tex_w - 1)


def _layer_name(alloc_entry):
    nm = _ref_name((alloc_entry or {}).get("LayerInfo"))
    if nm.endswith("_LayerInfo"):
        nm = nm[:-len("_LayerInfo")]
    return nm.lower()


def _hole_mask(pkg, p, nverts, ssq, csq, tex_cache, warnings):
    """The 'DataLayer' weightmap allocation is the landscape VISIBILITY mask:
    where it reads high the game carves the quad out (the bunker entrance
    stairwell). Returns a (csq, csq) bool grid of carved quads, or None."""
    for a in p.get("WeightmapLayerAllocations") or []:
        if _layer_name(a) != "datalayer":
            continue
        wt_names = [_ref_name(x) for x in (p.get("WeightmapTextures") or [])]
        tidx = int(a.get("WeightmapTextureIndex", 0) or 0)
        chan = int(a.get("WeightmapTextureChannel", 0) or 0)
        if tidx >= len(wt_names):
            return None
        wm = _decoded_tex(pkg, wt_names[tidx], tex_cache, warnings, "hole mask")
        if wm is None:
            return None
        tr, tc = _block_indices(nverts, ssq, csq, 0, 0, wm.shape[1], wm.shape[0])
        w = wm[np.ix_(tr, tc)][:, :, min(chan, 3)]
        quad = (w[:-1, :-1] + w[1:, :-1] + w[:-1, 1:] + w[1:, 1:]) * 0.25
        hole = quad > 0.5
        return hole if hole.any() else None
    return None


def build_landscape(game, map_path, dicts, collection, warnings, builder=None,
                    fallback_material=None, caches=None):
    pkg = game.load_package(map_path)
    caches = caches if caches is not None else {"mat": {}, "img": {}}

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

    specs = {}
    if fallback_material is None:
        try:
            specs = materials_mod.landscape_layer_specs(game, caches)
        except Exception as e:  # noqa: BLE001
            warnings.append(f"landscape layer specs failed: {type(e).__name__} {e}")

    tex_cache = {}
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
        if builder is not None and builder.radius > 0.0:
            # component center, with the component's half-diagonal as slack
            cx = root_loc[0] + (base_x + csq * 0.5) * root_scale[0]
            cy = root_loc[1] + (base_y + csq * 0.5) * root_scale[1]
            center = convert.pos((cx, cy, 0.0))
            half = csq * root_scale[0] * convert.SCALE * 0.75
            dx = center[0] - builder.origin[0]
            dy = center[1] - builder.origin[1]
            if (dx * dx + dy * dy) ** 0.5 > builder.radius_bl + half:
                continue
        hm = _decoded_tex(pkg, _ref_name(p.get("HeightmapTexture")), tex_cache,
                          warnings, "heightmap")
        if hm is None:
            continue
        tex_h, tex_w = hm.shape[0], hm.shape[1]
        px0 = int(round(bias_u * tex_w)) if inv_w else 0
        py0 = int(round(bias_v * tex_h)) if inv_h else 0

        nverts = csq + 1
        tr, tc = _block_indices(nverts, ssq, csq, px0, py0, tex_w, tex_h)
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
        hole = _hole_mask(pkg, p, nverts, ssq, csq, tex_cache, warnings)
        if hole is not None:
            faces = faces[~hole.ravel()]

        me = bpy.data.meshes.new(f"landscape_{e.get('Name')}")
        me.from_pydata(verts.tolist(), [], faces.tolist())
        me.validate()

        mat = fallback_material
        if fallback_material is None:
            mat = _component_material(game, pkg, p, e.get("Name", "comp"), nverts,
                                      ssq, csq, tex_cache, specs, caches, warnings)
        if mat is not None:
            me.materials.append(mat)
            uvl = me.uv_layers.new(name="ComponentUV")
            pv = np.stack([(gx + 0.5) / nverts, (gy + 0.5) / nverts],
                          axis=-1).reshape(-1, 2).astype(np.float32)
            vidx = np.empty(len(me.loops), dtype=np.int32)
            me.loops.foreach_get("vertex_index", vidx)
            uvl.data.foreach_set("uv", pv[vidx].ravel())
        me.polygons.foreach_set("use_smooth", np.ones(len(me.polygons), dtype=bool))
        ob = bpy.data.objects.new(me.name, me)
        collection.objects.link(ob)
        built += 1
    return built


def _component_material(game, pkg, p, comp_name, nverts, ssq, csq, tex_cache,
                        specs, caches, warnings):
    """The component's layer set -> a blended material (weight image when the
    component paints more than one layer)."""
    alloc = p.get("WeightmapLayerAllocations") or []
    layers = []
    for a in alloc:
        nm = _layer_name(a)
        if not nm or nm == "datalayer":
            continue   # the landscape hole mask, not a paint layer
        layers.append((nm, int(a.get("WeightmapTextureIndex", 0) or 0),
                       int(a.get("WeightmapTextureChannel", 0) or 0)))
    if not layers:
        layers = [("grass", 0, 0)]
    if len(layers) == 1:
        key = ("landscape", layers[0][0])
        return materials_mod.landscape_material(
            game, [(layers[0][0], 0)], None, specs, caches, warnings, key)

    wt_names = [_ref_name(x) for x in (p.get("WeightmapTextures") or [])]
    n = min(len(layers), 4)
    if len(layers) > 4:
        warnings.append(f"landscape {comp_name}: {len(layers)} paint layers, "
                        f"only 4 blended ({[nm for nm, _t, _c in layers[4:]]} dropped)")
    weights = np.zeros((nverts, nverts, 4), dtype=np.float32)
    got_any = False
    for k, (nm, tidx, chan) in enumerate(layers[:n]):
        if tidx >= len(wt_names):
            continue
        wm = _decoded_tex(pkg, wt_names[tidx], tex_cache, warnings, "weightmap")
        if wm is None:
            continue
        # Z/W of WeightmapScaleBias is a half-texel center offset -> origin 0
        tr, tc = _block_indices(nverts, ssq, csq, 0, 0, wm.shape[1], wm.shape[0])
        weights[:, :, k] = wm[np.ix_(tr, tc)][:, :, min(chan, 3)]
        got_any = True
    weight_img = None
    if got_any:
        weight_img = bpy.data.images.new(f"votv_weights_{comp_name}",
                                         width=nverts, height=nverts, alpha=True)
        weight_img.pixels.foreach_set(weights.reshape(-1))
        weight_img.pack()
    return materials_mod.landscape_material(
        game, [(nm, k) for k, (nm, _t, _c) in enumerate(layers[:n])],
        weight_img, specs, caches, warnings, ("landscape", comp_name))
