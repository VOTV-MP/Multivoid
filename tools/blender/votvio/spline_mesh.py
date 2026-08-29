"""USplineMeshComponent deform: bend a static mesh along its Hermite spline.

The river (85 segments of meshes/misc/river under Landscape_0) and any other
SplineMeshComponent serialize `SplineParams` (Start/End pos+tangent, per-axis
2D scale, roll, offset). The engine bends vertices in the vertex factory; we
reproduce the same math in numpy at import time (UE-space, before the Blender
axis conversion): t from the vertex's forward-axis position across the mesh
bounds, slice frame from the spline tangent + SplineUpDir, cross-section (y,z)
scaled/offset/rolled into the frame.
"""
import numpy as np


def _v3(d, default=(0.0, 0.0, 0.0)):
    if isinstance(d, dict):
        return np.array([float(d.get("X", 0.0)), float(d.get("Y", 0.0)),
                         float(d.get("Z", 0.0))], dtype=np.float64)
    return np.array(default, dtype=np.float64)


def _v2(d, default=(1.0, 1.0)):
    if isinstance(d, dict):
        return np.array([float(d.get("X", default[0])), float(d.get("Y", default[1]))],
                        dtype=np.float64)
    return np.array(default, dtype=np.float64)


def parse_params(props):
    """SplineMeshComponent delta props -> plain dict of numpy params."""
    sp = props.get("SplineParams") or {}
    fwd = str(props.get("ForwardAxis", "X"))
    return {
        "p0": _v3(sp.get("StartPos")),
        "t0": _v3(sp.get("StartTangent"), (100.0, 0.0, 0.0)),
        "p1": _v3(sp.get("EndPos"), (100.0, 0.0, 0.0)),
        "t1": _v3(sp.get("EndTangent"), (100.0, 0.0, 0.0)),
        "s0": _v2(sp.get("StartScale")),
        "s1": _v2(sp.get("EndScale")),
        "o0": _v2(sp.get("StartOffset"), (0.0, 0.0)),
        "o1": _v2(sp.get("EndOffset"), (0.0, 0.0)),
        "r0": float(sp.get("StartRoll", 0.0) or 0.0),
        "r1": float(sp.get("EndRoll", 0.0) or 0.0),
        "up": _v3(props.get("SplineUpDir"), (0.0, 0.0, 1.0)),
        "bmin": float(props.get("SplineBoundaryMin", 0.0) or 0.0),
        "bmax": float(props.get("SplineBoundaryMax", 0.0) or 0.0),
        "forward_x": "Z" not in fwd and "Y" not in fwd,
    }


def cache_key(params):
    vals = []
    for k in ("p0", "t0", "p1", "t1", "s0", "s1", "o0", "o1"):
        vals.extend(round(float(x), 2) for x in params[k])
    vals.append(round(params["r0"], 3))
    vals.append(round(params["r1"], 3))
    return tuple(vals)


def deform(verts_ue, params):
    """verts_ue: (N,3) float64 UE-space -> deformed (N,3)."""
    v = np.asarray(verts_ue, dtype=np.float64)
    if not params["forward_x"]:
        # no measured case (all 85 river segments default to X); keep undeformed
        return v
    x = v[:, 0]
    bmin, bmax = params["bmin"], params["bmax"]
    if bmax <= bmin:
        bmin, bmax = float(x.min()), float(x.max())
    rng = bmax - bmin
    if rng <= 1e-6:
        return v
    t = np.clip((x - bmin) / rng, 0.0, 1.0)

    t2 = t * t
    t3 = t2 * t
    a = 2 * t3 - 3 * t2 + 1
    b = t3 - 2 * t2 + t
    c = -2 * t3 + 3 * t2
    d = t3 - t2
    p0, t0, p1, t1 = params["p0"], params["t0"], params["p1"], params["t1"]
    pos = (a[:, None] * p0 + b[:, None] * t0 + c[:, None] * p1 + d[:, None] * t1)
    da = 6 * t2 - 6 * t
    db = 3 * t2 - 4 * t + 1
    dc = -6 * t2 + 6 * t
    dd = 3 * t2 - 2 * t
    tan = (da[:, None] * p0 + db[:, None] * t0 + dc[:, None] * p1 + dd[:, None] * t1)
    ln = np.linalg.norm(tan, axis=1, keepdims=True)
    ln[ln < 1e-6] = 1.0
    xdir = tan / ln

    up = params["up"]
    right = np.cross(np.broadcast_to(up, xdir.shape), xdir)
    ln = np.linalg.norm(right, axis=1, keepdims=True)
    ln[ln < 1e-6] = 1.0
    right /= ln
    up2 = np.cross(xdir, right)

    roll = params["r0"] * (1 - t) + params["r1"] * t
    if abs(params["r0"]) > 1e-6 or abs(params["r1"]) > 1e-6:
        cr = np.cos(roll)[:, None]
        sr = np.sin(roll)[:, None]
        right, up2 = cr * right + sr * up2, -sr * right + cr * up2

    scale = params["s0"][None, :] * (1 - t)[:, None] + params["s1"][None, :] * t[:, None]
    off = params["o0"][None, :] * (1 - t)[:, None] + params["o1"][None, :] * t[:, None]
    yy = v[:, 1] * scale[:, 0] + off[:, 0]
    zz = v[:, 2] * scale[:, 1] + off[:, 1]
    return pos + right * yy[:, None] + up2 * zz[:, None]
