"""Deferred-decal analog: VOTV paints grime/graffiti/stains with DecalComponents.

A deferred decal exists only where geometry lies inside its box: the engine
projects it onto receiver surfaces and nothing renders in empty space. The
Blender analog is a real PROJECTION at import time: each decal casts a grid of
rays along its local X (through the whole box depth) onto the assembled scene;
grid cells whose rays miss are dropped (the surface mask), hits become mesh
vertices lifted a few mm along the hit normal (so the decal hugs walls, wraps
corners and never sticks out past its receiver). Decals are queued during
assembly and projected in one post-pass after the statics/BSP/landscape exist.

Grime variant families: the grime BP picks its decal texture at RUNTIME from
numbered variant sets (measured: crack/leaky/dusty/light/grainy CDOs carry NO
material ref while the pak holds the families below).
"""
import hashlib
import math

from mathutils import Matrix, Vector

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


def size_matrix(size_ue):
    """DecalSize (UE half-extents, uu) -> local box scale (meters)."""
    return Matrix.Diagonal((max(float(size_ue[0]), 1.0) * 0.01,
                            max(float(size_ue[1]), 1.0) * 0.01,
                            max(float(size_ue[2]), 1.0) * 0.01, 1.0))


_OFFSET = 0.006        # lift off the receiver surface (m)
_CELL = 0.12           # target grid cell size (m)
_EXTRA_DEPTH = 0.15    # placement tolerance beyond the box depth (m)


def _uv(fy, fz):
    """UE4 deferred-decal UV: U runs along local Z, V along Y (DeferredDecal.usf
    DecalVector.zy mapping; Blender's bottom-origin V absorbs the D3D flip).
    Measured: the baseMural boxes are long in Z with +Y up and the mural art is
    landscape - the old U-along-Y mapping rendered every decal rotated 90deg."""
    return ((fz + 1.0) * 0.5, (fy + 1.0) * 0.5)


def _wind(verts, faces, direction):
    """Flip face winding so the sheet's normal faces back along the cast ray."""
    if not faces:
        return faces
    a, b, c = (Vector(verts[i]) for i in faces[0][:3])
    n = (b - a).cross(c - a)
    if n.dot(direction) > 0.0:
        return [tuple(reversed(f)) for f in faces]
    return faces


def project_decal(scene, depsgraph, m):
    """Project one decal box (world matrix incl. half-extent scale) onto the
    scene. -> list of sheets [(verts, faces, per-vertex uv)] in world space,
    empty when nothing inside the box receives it.

    The engine paints every surface inside the OBB, projecting along local X
    BOTH ways - a wall inside the box receives the decal on each face. So two
    passes, one per direction. Per pass, two phases: a 3x3 probe first - when
    every probe hits ONE plane (flat wall/floor, the common case) the full grid
    is intersected with that plane analytically, zero further rays. Partial
    hits or bent receivers (corners, surface edges - exactly where the mask
    matters) fall through to a full per-vertex ray grid."""
    loc, rot, scale = m.decompose()
    R = rot.to_matrix()
    x_axis = (R @ Vector((1.0, 0.0, 0.0))).normalized()
    y_axis = (R @ Vector((0.0, 1.0, 0.0))).normalized()
    z_axis = (R @ Vector((0.0, 0.0, 1.0))).normalized()
    sx, sy, sz = abs(scale.x), abs(scale.y), abs(scale.z)
    depth = sx + _EXTRA_DEPTH
    ny = max(3, min(16, int(round(2.0 * sy / _CELL))))
    nz = max(3, min(16, int(round(2.0 * sz / _CELL))))
    sheets = []
    for direction in (x_axis, -x_axis):
        sheet = _project_side(scene, depsgraph, loc, direction, y_axis, z_axis,
                              sy, sz, ny, nz, depth)
        if sheet is not None:
            sheets.append(sheet)
    return sheets


def _project_side(scene, depsgraph, loc, direction, y_axis, z_axis, sy, sz,
                  ny, nz, depth):
    def cast(fy, fz):
        origin = loc + y_axis * (fy * sy) + z_axis * (fz * sz) - direction * depth
        return scene.ray_cast(depsgraph, origin, direction, distance=2.0 * depth)

    # ---- phase A: 3x3 probe ------------------------------------------------
    probes = {}
    for fy in (-1.0, 0.0, 1.0):
        for fz in (-1.0, 0.0, 1.0):
            ok, hloc, hnorm, _i, _o, _mw = cast(fy, fz)
            if ok:
                probes[(fy, fz)] = (hloc, hnorm)
    if not probes:
        return None
    if len(probes) == 9:
        nmean = Vector((0.0, 0.0, 0.0))
        for _p, n in probes.values():
            nmean += n
        nmean.normalize()
        planar = all(n.dot(nmean) > 0.995 for _p, n in probes.values())
        if planar:
            p0 = probes[(0.0, 0.0)][0]
            residual = max(abs((p - p0).dot(nmean)) for p, _n in probes.values())
            planar = residual < 0.015
        denom = direction.dot(nmean)
        if planar and abs(denom) > 1e-4:
            verts = []
            uvs = []
            for iy in range(ny + 1):
                fy = -1.0 + 2.0 * iy / ny
                for iz in range(nz + 1):
                    fz = -1.0 + 2.0 * iz / nz
                    origin = loc + y_axis * (fy * sy) + z_axis * (fz * sz)
                    t = (p0 - origin).dot(nmean) / denom
                    p = origin + direction * t + nmean * _OFFSET
                    verts.append((p.x, p.y, p.z))
                    uvs.append(_uv(fy, fz))
            faces = [(iy * (nz + 1) + iz, (iy + 1) * (nz + 1) + iz,
                      (iy + 1) * (nz + 1) + iz + 1, iy * (nz + 1) + iz + 1)
                     for iy in range(ny) for iz in range(nz)]
            return verts, _wind(verts, faces, direction), uvs

    # ---- phase B: full ray grid -------------------------------------------
    hits = {}
    for iy in range(ny + 1):
        fy = -1.0 + 2.0 * iy / ny
        for iz in range(nz + 1):
            fz = -1.0 + 2.0 * iz / nz
            ok, hloc, hnorm, _i, _o, _mw = cast(fy, fz)
            if ok:
                hits[(iy, iz)] = (hloc + hnorm * _OFFSET, _uv(fy, fz))
    if not hits:
        return None

    cell_diag = math.hypot(2.0 * sy / ny, 2.0 * sz / nz)
    max_span = max(cell_diag * 2.5, 0.25)
    verts = []
    uvs = []
    vidx = {}

    def vid(key):
        i = vidx.get(key)
        if i is None:
            i = len(verts)
            vidx[key] = i
            p, uv = hits[key]
            verts.append((p.x, p.y, p.z))
            uvs.append(uv)
        return i

    faces = []
    for iy in range(ny):
        for iz in range(nz):
            ks = ((iy, iz), (iy + 1, iz), (iy + 1, iz + 1), (iy, iz + 1))
            if any(k not in hits for k in ks):
                continue
            pts = [hits[k][0] for k in ks]
            span = max((pts[a] - pts[b]).length
                       for a in range(4) for b in range(a + 1, 4))
            if span > max_span:
                continue  # a web across a gap/depth jump, not a surface
            faces.append(tuple(vid(k) for k in ks))
    if not faces:
        return None
    return verts, _wind(verts, faces, direction), uvs
