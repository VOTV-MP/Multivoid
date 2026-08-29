"""UE4 <-> Blender space conversion.

UE: left-handed, Z-up, X-forward, centimeters. Blender: right-handed, Z-up, meters.
Conversion = mirror across the XZ plane (y -> -y) + 0.01 scale.
Under that mirror a rotation (axis a, angle t) becomes (Ma, -t), which in quaternion
terms is (x, y, z, w) -> (-x, +y, -z, w). Triangle winding flips (handled in mesh_build).
"""
import math

from mathutils import Matrix, Quaternion, Vector

SCALE = 0.01


def pos(v):
    return Vector((v[0] * SCALE, -v[1] * SCALE, v[2] * SCALE))


def quat(q_xyzw):
    x, y, z, w = q_xyzw
    return Quaternion((w, -x, y, -z))


def rotator_to_ue_quat(pitch, yaw, roll):
    """UE FRotator (degrees) -> UE FQuat (x, y, z, w). Mirrors FRotator::Quaternion()."""
    hp = math.radians(pitch) * 0.5
    hy = math.radians(yaw) * 0.5
    hr = math.radians(roll) * 0.5
    sp, cp = math.sin(hp), math.cos(hp)
    sy, cy = math.sin(hy), math.cos(hy)
    sr, cr = math.sin(hr), math.cos(hr)
    return (
        cr * sp * sy - sr * cp * cy,
        -cr * sp * cy - sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    )


def matrix(q_xyzw, loc, scale):
    """UE transform pieces -> Blender 4x4 world matrix."""
    m = quat(q_xyzw).to_matrix().to_4x4()
    m = Matrix.Translation(pos(loc)) @ m
    return m @ Matrix.Diagonal((scale[0], scale[1], scale[2], 1.0))


def matrix_from_rotator(rot_pyr, loc, scale):
    return matrix(rotator_to_ue_quat(*rot_pyr), loc, scale)


def ue_fmatrix_to_bl(m_rows):
    """UE FMatrix (4x4, row-vector convention: v' = v @ M, rows 0-2 = basis,
    row 3 = translation, cm) -> Blender 4x4 Matrix (column convention, m).

    Column form is M^T; the y-mirror conjugates the 3x3 (D @ A @ D) and the
    translation maps through pos()."""
    import numpy as np

    a = np.asarray(m_rows, dtype=np.float64)
    lin = a[:3, :3].T.copy()          # column-major 3x3
    lin[0, 1] = -lin[0, 1]
    lin[1, 0] = -lin[1, 0]
    lin[1, 2] = -lin[1, 2]
    lin[2, 1] = -lin[2, 1]
    t = a[3, :3]
    out = Matrix.Identity(4)
    for i in range(3):
        for j in range(3):
            out[i][j] = lin[i, j]
    out[0][3] = t[0] * SCALE
    out[1][3] = -t[1] * SCALE
    out[2][3] = t[2] * SCALE
    return out
