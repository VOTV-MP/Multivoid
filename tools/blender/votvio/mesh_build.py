"""Build a Blender mesh datablock from a cooked UStaticMesh (LOD0)."""
import numpy as np

import bpy

from . import convert


def _uv_pair(item, set_index, full_precision):
    uv = item.UV[set_index]
    u, v = uv.U, uv.V
    if not full_precision:
        # pyUE4Parse's FMeshUVHalf.to_mesh_uv_float returns RAW uint16 half bits.
        u = float(np.uint16(u).view(np.float16))
        v = float(np.uint16(v).view(np.float16))
    return u, 1.0 - v  # UE UV origin is top-left; Blender bottom-left


def _static_materials(sm):
    """Material package paths per slot, from the property block (reliable post-fix)."""
    mats = []
    try:
        props = sm.GetValue().get("Properties") or {}
        for entry in props.get("StaticMaterials") or []:
            ref = entry.get("MaterialInterface") or {}
            outer = ref.get("OuterIndex") or {}
            pkg = outer.get("ObjectName") or ref.get("Outer") or ""
            mats.append(str(pkg) if pkg else "")
    except Exception:  # noqa: BLE001
        pass
    return mats


def build_mesh(game, mesh_pkg_path, warnings):
    """-> (bpy.types.Mesh, [material package paths]) or (None, [])."""
    sm = game.find_export(mesh_pkg_path, "StaticMesh")
    lods = getattr(sm, "LODs", ()) if sm is not None else ()
    if not lods:
        if sm is not None:
            warnings.append(f"mesh has no LODs: {mesh_pkg_path}")
        return None, []
    lod = lods[0]
    pvb = lod.positionVertexBuffer
    if pvb is None or not getattr(pvb, "Verts", None):
        warnings.append(f"mesh has no positions: {mesh_pkg_path}")
        return None, []

    verts = [(v.X * convert.SCALE, -v.Y * convert.SCALE, v.Z * convert.SCALE)
             for v in pvb.Verts]

    idx = lod.indexBuffer
    indices = (getattr(idx, "indices32", None) or getattr(idx, "indices16", None) or []) if idx else []
    # winding reversed: the y-mirror flips handedness
    faces = [(indices[i], indices[i + 2], indices[i + 1])
             for i in range(0, len(indices) - 2, 3)]
    if not faces:
        warnings.append(f"mesh has no indices: {mesh_pkg_path}")
        return None, []

    name = mesh_pkg_path.rsplit("/", 1)[-1]
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)

    # UV set 0 (per-vertex in the UE buffer -> per-loop here)
    vb = getattr(lod, "vertexBuffer", None)
    uv_items = getattr(vb, "UV", None) if vb else None
    if uv_items:
        full = bool(getattr(vb, "UseFullPrecisionUVs", True))
        try:
            per_vertex = [_uv_pair(uv_items[i], 0, full) for i in range(len(verts))]
            layer = me.uv_layers.new(name="UVMap")
            flat = np.empty(len(me.loops) * 2, dtype=np.float32)
            loop_verts = np.empty(len(me.loops), dtype=np.int32)
            me.loops.foreach_get("vertex_index", loop_verts)
            uvs = np.asarray(per_vertex, dtype=np.float32)
            flat = uvs[loop_verts].reshape(-1)
            layer.data.foreach_set("uv", flat)
        except Exception as e:  # noqa: BLE001 - UVs are cosmetic; geometry must survive
            warnings.append(f"uv build failed: {mesh_pkg_path}: {type(e).__name__}")

    # material slots per section
    mat_paths = _static_materials(sm)
    sections = getattr(lod, "sections", []) or []
    if sections:
        poly_mat = np.zeros(len(me.polygons), dtype=np.int32)
        for si, sec in enumerate(sections):
            first_tri = int(sec.FirstIndex) // 3
            ntris = int(sec.NumTriangles)
            slot = int(getattr(sec, "MaterialIndex", si))
            poly_mat[first_tri:first_tri + ntris] = slot
        me.polygons.foreach_set("material_index", poly_mat)

    me.polygons.foreach_set("use_smooth", np.ones(len(me.polygons), dtype=bool))
    me.validate()
    me.update()
    return me, mat_paths
