"""Build a Blender mesh datablock from a cooked UStaticMesh (LOD0)."""
import numpy as np

import bpy

from . import convert, spline_mesh


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


def _lod0(game, mesh_pkg_path, warnings):
    sm = game.find_export(mesh_pkg_path, "StaticMesh")
    lods = getattr(sm, "LODs", ()) if sm is not None else ()
    if not lods:
        if sm is not None:
            warnings.append(f"mesh has no LODs: {mesh_pkg_path}")
        return None, None
    lod = lods[0]
    pvb = lod.positionVertexBuffer
    if pvb is None or not getattr(pvb, "Verts", None):
        warnings.append(f"mesh has no positions: {mesh_pkg_path}")
        return None, None
    return sm, lod


def _assemble(name, sm, lod, verts_bl, mesh_pkg_path, warnings):
    idx = lod.indexBuffer
    indices = (getattr(idx, "indices32", None) or getattr(idx, "indices16", None) or []) if idx else []
    # NATURAL index order: the y-mirror alone turns UE/D3D's CW-front into
    # Blender's CCW-front. The old extra (i0,i2,i1) swap double-compensated and
    # turned every mesh inside-out - invisible to two-sided viewport shading,
    # but ray_cast returned INWARD hit normals, so decals were projected 13mm
    # INTO their walls and wound facing the wall cavity (field report: decal
    # visible only with the camera inside the wall / after flipping it).
    faces = [(indices[i], indices[i + 1], indices[i + 2])
             for i in range(0, len(indices) - 2, 3)]
    if not faces:
        warnings.append(f"mesh has no indices: {mesh_pkg_path}")
        return None, []

    me = bpy.data.meshes.new(name)
    me.from_pydata(verts_bl, [], faces)

    # UV set 0 (per-vertex in the UE buffer -> per-loop here)
    vb = getattr(lod, "vertexBuffer", None)
    uv_items = getattr(vb, "UV", None) if vb else None
    if uv_items:
        full = bool(getattr(vb, "UseFullPrecisionUVs", True))
        try:
            per_vertex = [_uv_pair(uv_items[i], 0, full) for i in range(len(verts_bl))]
            layer = me.uv_layers.new(name="UVMap")
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


def build_mesh(game, mesh_pkg_path, warnings):
    """-> (bpy.types.Mesh, [material package paths]) or (None, [])."""
    sm, lod = _lod0(game, mesh_pkg_path, warnings)
    if lod is None:
        return None, []
    verts = [(v.X * convert.SCALE, -v.Y * convert.SCALE, v.Z * convert.SCALE)
             for v in lod.positionVertexBuffer.Verts]
    name = mesh_pkg_path.rsplit("/", 1)[-1]
    return _assemble(name, sm, lod, verts, mesh_pkg_path, warnings)


def build_bsp_mesh(bsp, surf_mat_paths, name, warnings):
    """Cooked-UModel node soup -> mesh. Per-loop UVs from each surf's texture
    basis: uv = ((P - Points[pBase]) . Vectors[vTexU|V]) / 128."""
    points = bsp["points"]
    vectors = bsp["vectors"]
    surfs = bsp["surfs"]
    verts_bl = np.empty_like(points)
    verts_bl[:, 0] = points[:, 0] * convert.SCALE
    verts_bl[:, 1] = -points[:, 1] * convert.SCALE
    verts_bl[:, 2] = points[:, 2] * convert.SCALE

    faces = []
    face_slot = []
    loop_uv = []
    slot_of = {}
    unique_paths = []
    npts = len(points)
    nvec = len(vectors)
    for i_surf, idxs in bsp["polys"]:
        path = surf_mat_paths[i_surf] if i_surf < len(surf_mat_paths) else ""
        if path not in slot_of:
            slot_of[path] = len(unique_paths)
            unique_paths.append(path)
        slot = slot_of[path]
        _mat, p_base, v_tu, v_tv = surfs[i_surf]
        if 0 <= p_base < npts and 0 <= v_tu < nvec and 0 <= v_tv < nvec:
            base = points[p_base]
            tu = vectors[v_tu]
            tv = vectors[v_tv]
        else:
            base = np.zeros(3)
            tu = np.array([1.0, 0.0, 0.0])
            tv = np.array([0.0, 1.0, 0.0])

        def uv(pi):
            d = points[pi] - base
            return (float(d @ tu) / 128.0, 1.0 - float(d @ tv) / 128.0)

        n = len(idxs)
        for k in range(1, n - 1):
            # natural fan order, same handedness rule as _assemble
            a, b, c = int(idxs[0]), int(idxs[k]), int(idxs[k + 1])
            faces.append((a, b, c))
            face_slot.append(slot)
            loop_uv.extend((uv(a), uv(b), uv(c)))
    if not faces:
        warnings.append("bsp model produced no faces")
        return None, []
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts_bl], [], faces)
    try:
        layer = me.uv_layers.new(name="UVMap")
        layer.data.foreach_set("uv", np.asarray(loop_uv, dtype=np.float32).reshape(-1))
    except Exception as e:  # noqa: BLE001
        warnings.append(f"bsp uv failed: {type(e).__name__}")
    me.polygons.foreach_set("material_index", np.asarray(face_slot, dtype=np.int32))
    me.validate()
    me.update()
    return me, unique_paths


def build_spline_mesh(game, mesh_pkg_path, params, warnings):
    """Spline-deformed copy of a mesh (UE-space Hermite bend, then conversion)."""
    sm, lod = _lod0(game, mesh_pkg_path, warnings)
    if lod is None:
        return None, []
    verts_ue = np.array([[v.X, v.Y, v.Z] for v in lod.positionVertexBuffer.Verts],
                        dtype=np.float64)
    bent = spline_mesh.deform(verts_ue, params)
    verts = np.empty_like(bent)
    verts[:, 0] = bent[:, 0] * convert.SCALE
    verts[:, 1] = -bent[:, 1] * convert.SCALE
    verts[:, 2] = bent[:, 2] * convert.SCALE
    name = mesh_pkg_path.rsplit("/", 1)[-1] + ".spline"
    return _assemble(name, sm, lod, [tuple(v) for v in verts], mesh_pkg_path, warnings)
