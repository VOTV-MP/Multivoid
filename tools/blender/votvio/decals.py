"""Deferred-decal analog: VOTV paints grime/graffiti/stains with DecalComponents
(992 in the umap; 936 grime_* actors carry one each, material often via a MID
whose parent sits on the class template). Blender has no projected decals, so
each decal becomes a thin double-sided quad at the decal's transform: the decal
box projects along local X, the quad spans local Y/Z at +-2 cm so whichever
side the receiving surface is on, one face sits just proud of it.
"""
import bpy
from mathutils import Matrix

_PLANE_NAME = "votvio_decal_plane"


def plane_mesh():
    """Shared unit decal quad (+-1m in Y/Z, two faces at x=+-0.02)."""
    me = bpy.data.meshes.get(_PLANE_NAME)
    if me is not None:
        return me
    verts = [
        (0.02, -1, -1), (0.02, 1, -1), (0.02, 1, 1), (0.02, -1, 1),      # +X face
        (-0.02, -1, -1), (-0.02, -1, 1), (-0.02, 1, 1), (-0.02, 1, -1),  # -X face
    ]
    faces = [(0, 1, 2, 3), (4, 5, 6, 7)]
    me = bpy.data.meshes.new(_PLANE_NAME)
    me.from_pydata(verts, [], faces)
    layer = me.uv_layers.new(name="UVMap")
    for li, loop in enumerate(me.loops):
        v = verts[loop.vertex_index]
        layer.data[li].uv = ((v[1] + 1.0) * 0.5, (v[2] + 1.0) * 0.5)
    me.validate()
    me.update()
    return me


def size_matrix(size_ue):
    """DecalSize (UE half-extents, uu) -> local scale for the unit quad."""
    return Matrix.Diagonal((1.0, max(float(size_ue[1]), 1.0) * 0.01,
                            max(float(size_ue[2]), 1.0) * 0.01, 1.0))
