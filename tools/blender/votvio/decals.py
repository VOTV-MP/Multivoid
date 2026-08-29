"""Deferred-decal analog: VOTV paints grime/graffiti/stains with DecalComponents
(992 in the umap; 936 grime_* actors carry one each, material often via a MID
whose parent sits on the class template). Blender has no projected decals, so
each decal becomes a thin double-sided quad at the decal's transform: the decal
box projects along local X, the quad spans local Y/Z at +-2 cm so whichever
side the receiving surface is on, one face sits just proud of it.
"""
import hashlib
import math

import bpy
from mathutils import Matrix

_PLANE_NAME = "votvio_decal_plane"

# The grime BP picks its decal texture at RUNTIME from a per-type variant
# family (measured: the CDOs of crack/leaky/dusty/light/grainy carry NO
# material ref, and the pak holds numbered variant sets). Class -> (format,
# first index, count). Classes absent here fall through to the CDO 'material'
# variable, then the class template's DecalMaterial.
_CRACKS = ("/Game/textures/decals/grime/cracks/inst_decalCrack_{}", 0, 17)
_LEAK = ("/Game/textures/decals/grime/inst_DecalGrunge_leak_{}", 0, 8)
_DIRT = ("/Game/textures/decals/grime/inst_DecalGrunge_dirt_{}", 0, 35)
_LEAVES = ("/Game/textures/decals/grime/leaves/inst_decalLeaves_{}", 1, 4)
GRIME_FAMILY = {
    "grime_C": _DIRT,
    "grime_crack_C": _CRACKS,
    "grime_leaky_C": _LEAK,
    "grime_leaky_rusty_C": _LEAK,
    "grime_leaky_wet_C": _LEAK,
    "grime_dusty_C": _DIRT,
    "grime_light_C": _DIRT,
    "grime_grainy_C": _DIRT,
    "grime_fallenLeaves_C": _LEAVES,
}

# leak drips must stay vertical (their CDOs set randomOrientation=False);
# everything else in the grime family gets the game's random spin around the
# projection axis
_NO_SPIN = {"grime_leaky_C", "grime_leaky_rusty_C", "grime_leaky_wet_C"}


def _seed_int(seed):
    return int.from_bytes(hashlib.sha1(str(seed).encode()).digest()[:4], "little")


def grime_material(class_name, seed):
    """Stable per-instance variant pick, or None when the class has no family."""
    fam = GRIME_FAMILY.get(class_name)
    if fam is None:
        return None
    fmt, first, count = fam
    return fmt.format(first + _seed_int(seed) % count)


def grime_spin(class_name, seed):
    """Seeded rotation about the projection axis (local X), or None."""
    if class_name not in GRIME_FAMILY or class_name in _NO_SPIN:
        return None
    ang = (_seed_int(str(seed) + ":spin") % 3600) / 3600.0 * 2.0 * math.pi
    return Matrix.Rotation(ang, 4, "X")


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
