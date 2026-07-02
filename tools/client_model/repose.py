#!/usr/bin/env python3
"""repose -- auto scale+repose a GoldSrc A-pose model to the VOTV T-pose standard.

LEARNED from manual examples: the user poses+scales ONE model onto dr_kel's anthro
T-pose in Blender and exports a PSK. Because GoldSrc skinning is RIGID (1 bone/vertex),
that manual repose is EXACTLY a per-bone rigid transform, and -- since bone head
offsets are rest-invariant under posing -- it reduces to ONE transferable quantity per
bone: a LOCAL POSE DELTA (R+t in the bone's own rest frame) + target height/placement.

A profile transfers by bone NAME and is exact on the skeleton it was learned from; on
a DIFFERENT skeleton it degrades two ways (both MEASURED on rvi_scientist 2026-07-02,
the bad-pak postmortem): bones the profile never saw keep their rest orientation
(103/764 verts stayed A-posed), and even covered bones drift because the deltas are
rest-relative (error compounds down the arm chain: 2.6 -> 12 units by the fingers).
So the library keeps ONE profile per learned example, and the fit is MEASURED, never
assumed:

  learn  <origDir> <posed.psk> <profile.json>     # extract the standard from an example
  apply  <origDir> <profile.json|auto> <out.obj>  [--validate <psk>]
  select <origDir>                                # print the library scoring table only

auto-select scores every library profile against the model: vertex-weighted bone-name
COVERAGE first, then REST-POSE similarity (geodesic angle between the model's and the
profile's recorded rest_local rotations -- "is this the skeleton I was learned on?").
Profiles with status "rejected" (look vetoed in-game) are skipped. apply() always
prints the uncovered-bones report; any uncovered share means: pose the model manually
once (Blender -> PSK next to the .mdl) and the converter LEARNS its exact profile
(portable/driver.py does that automatically).

Profile format 3 (the ONLY supported format; the library was relearned 2026-07-02):
pose_local (per-bone 4x4 R+t delta) + rest_local (the source skeleton's rest local
transforms = the fit metric) + status + placement. Older format-1/2 files: relearn
from their source PSK (RULE 2 -- no dual loaders).

<origDir> = mdl_extract output (model.obj + model.bones.json with bone world matrices).
Pipeline: mdl_extract -> repose.apply -> ue_cook. Dev/RE tool (RULE 3).
"""
import datetime
import json
import os
import struct
import sys

import numpy as np

PROFILE_LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles")


# ---------- IO ----------
def load_apose(d):
    V = []
    for line in open(os.path.join(d, "model.obj")):
        if line.startswith("v "):
            V.append([float(x) for x in line.split()[1:4]])
    V = np.array(V, float)
    m = json.load(open(os.path.join(d, "model.bones.json")))
    vbone = np.array(m["vert_bone"])
    W = np.array(m["world"]).reshape(-1, 3, 4)     # 3x4 (R|t) world, column-vector convention
    parent = [b["parent"] for b in m["bones"]]
    names = [b["name"] for b in m["bones"]]
    return V, vbone, W, parent, names


def psk_points(path):
    b = open(path, "rb").read(); o = 0; ch = {}
    while o + 32 <= len(b):
        c = b[o:o + 20].split(b"\0")[0].decode("latin1")
        _, ds, dc = struct.unpack_from("<iii", b, o + 20)
        if ds < 0 or dc < 0:
            break
        ch[c] = (o + 32, ds, dc); o += 32 + ds * dc
    o, ds, dc = ch["PNTS0000"]; P = np.frombuffer(b, "<f4", dc * 3, o).reshape(dc, 3).astype(float)
    o, ds, dc = ch["RAWWEIGHTS"]; pb = np.full(len(P), -1, int)
    for i in range(dc):
        w, pi, bi = struct.unpack_from("<fii", b, o + i * 12); pb[pi] = bi
    return P, pb


# ---------- math ----------
def to4(W3):
    M = np.eye(4); M[:3, :] = W3; return M


def orthonormal(R):
    U, _, Vt = np.linalg.svd(R[:3, :3])
    Rr = U @ Vt
    if np.linalg.det(Rr) < 0:
        U[:, -1] *= -1; Rr = U @ Vt
    return Rr


def umeyama(X, Y):
    """least-squares similarity mapping X->Y (Nx3): returns s, R(3x3), t(3)."""
    mx, my = X.mean(0), Y.mean(0); Xc, Yc = X - mx, Y - my
    S = Xc.T @ Yc / len(X); U, D, Vt = np.linalg.svd(S)
    d = np.sign(np.linalg.det(U @ Vt)); Dd = np.array([1, 1, d])
    R = U @ np.diag(Dd) @ Vt; R = R.T
    var = (Xc ** 2).sum() / len(X); s = (D * Dd).sum() / var
    t = my - s * R @ mx
    return s, R, t


def rest_locals(WA, parent):
    """Per-bone LOCAL rest transform (root = world). The profile records these so a
    later apply can MEASURE how far a new model's skeleton is from the learned one."""
    out = []
    for b in range(len(parent)):
        pa = parent[b]
        out.append((WA[b] if pa < 0 else np.linalg.inv(WA[pa]) @ WA[b]).tolist())
    return out


# ---------- learn ----------
def learn(orig_dir, posed_psk, prof_out):
    V, vbone, W3, parent, names = load_apose(orig_dir)
    P, pb = psk_points(posed_psk)
    nb = len(names)
    if len(V) != len(P) or not (vbone == pb).all():
        sys.exit(f"[learn] {os.path.basename(posed_psk)} does not correspond to the extracted "
                 f"model ({len(P)} vs {len(V)} points) -- export the PSK from the SAME model, "
                 f"pose/scale only (no added/deleted geometry, no reordering)")
    WA = np.array([to4(W3[i]) for i in range(nb)])

    # per-bone measured similarity A->T (only bones that carry vertices)
    Sb = [None] * nb; scales = []
    for b in range(nb):
        m = vbone == b
        if m.sum() >= 3:
            s, R, t = umeyama(V[m], P[m])
            M = np.eye(4); M[:3, :3] = s * R; M[:3, 3] = t; Sb[b] = M; scales.append(s)
    scale = float(np.median(scales))

    # measured UNIT-scale T-world frame per vertex-bone: W_Tu = (S/scale) @ WA
    WTu = [None] * nb
    for b in range(nb):
        if Sb[b] is not None:
            Su = Sb[b].copy(); Su[:3, :] /= scale
            WTu[b] = Su @ WA[b]

    # fill vertless connector bones (root, neck, ...) so the hierarchy is complete.
    # their pose rotation is unconstrained by verts; choose a natural default:
    #   root  -> orientation of its first posed child (rigid with pelvis)
    #   inner -> identity local pose (stays aligned with its parent)
    order = sorted(range(nb), key=lambda b: (0 if parent[b] < 0 else 1, b))
    for b in order:
        if WTu[b] is not None:
            continue
        pa = parent[b]
        if pa < 0:  # root: borrow a child's world rotation
            ch = next((c for c in range(nb) if parent[c] == b and WTu[c] is not None), None)
            R = orthonormal(WTu[ch]) if ch is not None else np.eye(3)
            M = np.eye(4); M[:3, :3] = R; WTu[b] = M          # at origin
        else:       # inner: identity local pose relative to parent
            RestL = np.linalg.inv(WA[pa]) @ WA[b]
            WTu[b] = WTu[pa] @ RestL

    # transferable = per-bone LOCAL POSE DELTA: a FULL rigid (R + t) in the bone's own
    # rest frame -- pose[b] = RestL_A[b]^-1 @ L_Tu[b]. The rotation is the re-pose; the
    # translation carries JOINT MOVES (e.g. the v2 wide example pushed shoulder/arm
    # joints outward -- rotation-only left a 16-unit residual, 2026-07-02). Both ride
    # in the rest-local frame, so they transfer by bone NAME.
    pose_local = {}
    for b in range(nb):
        pa = parent[b]
        if pa < 0:
            M = np.eye(4); M[:3, :3] = orthonormal(WTu[b])   # root: world rotation @ origin
            pose_local[b] = M.tolist()
        else:
            RestL = np.linalg.inv(WA[pa]) @ WA[b]
            LTu = np.linalg.inv(WTu[pa]) @ WTu[b]
            D = np.linalg.inv(RestL) @ LTu
            M = np.eye(4); M[:3, :3] = orthonormal(D); M[:3, 3] = D[:3, 3]
            pose_local[b] = M.tolist()

    # UP is a CONSTANT of the target space, never inferred from the mesh: the PSK/cook
    # space is UE Z-up (the cook is a pure Y-negation, SPEC.md). The old argmax(bbox)
    # heuristic broke the moment a T-pose ARM SPAN (X) grew wider than the height --
    # learn then measured "height" along the arms and grounded the model sideways
    # (2026-07-02: the new-profile example read up=axis0, self-reproduce residual 17.58).
    up = 2
    prof = {
        "format": 3,
        "skeleton": f"{names[0]}/{nb}bones",
        "source": os.path.basename(posed_psk),
        "learned": datetime.date.today().isoformat(),
        "status": "active",   # "rejected" = look vetoed in-game; auto-select skips it
        "bones": names, "parent": parent,
        "pose_local": [pose_local[b] for b in range(nb)],
        "rest_local": rest_locals(WA, parent),
        "target_height": float(P[:, up].max() - P[:, up].min()),
        "up_axis": up,
        "foot": float(P[:, up].min()),
        "center": [float((P[:, i].max() + P[:, i].min()) / 2) for i in range(3)],
    }
    json.dump(prof, open(prof_out, "w"), indent=1)
    print(f"[learn] {nb} bones, scale={scale:.4f}, target_height={prof['target_height']:.2f}, "
          f"up=axis{up}  -> {prof_out}")

    # self-validate: rebuild from the profile and compare to the ground-truth PSK
    Vt = _apply(V, vbone, WA, parent, prof, names)
    err = np.linalg.norm(Vt - P, axis=1)
    print(f"[learn] self-reproduce residual: max={err.max():.5f} mean={err.mean():.5f} "
          f"(should be ~0; units ~{prof['target_height']:.0f})")


# ---------- fit measurement ----------
def coverage_report(vbone, names, prof, quiet=False):
    """Vert-carrying bones the profile does NOT cover keep their A-pose orientation
    (identity local pose). Print them; return (covered_verts, total_verts)."""
    nb = len(names)
    vc = np.bincount(vbone, minlength=nb)
    have = set(prof["bones"])
    unc = [(names[b], int(vc[b])) for b in range(nb) if vc[b] and names[b] not in have]
    tot = int(vc.sum()); bad = sum(v for _, v in unc)
    if unc and not quiet:
        print(f"[repose] WARNING: profile covers {tot - bad}/{tot} verts "
              f"({100 * (tot - bad) / tot:.1f}%) -- uncovered bones KEEP their A-pose:")
        for n, v in unc:
            print(f"[repose]   {n}: {v} verts")
        print("[repose]   fix: pose this model manually once in Blender, export a PSK "
              "next to the .mdl, and the converter learns its exact profile.")
    elif not quiet:
        print(f"[repose] profile covers all {tot} skinned verts.")
    return tot - bad, tot


def rest_fit_deg(WA, parent, names, vbone, prof):
    """Vertex-weighted mean geodesic angle (degrees) between the model's rest_local
    rotations and the profile's recorded ones, over shared vert-carrying bones.
    ~0 = the very skeleton the profile was learned on; grows with A-pose difference."""
    if "rest_local" not in prof:
        return None
    pr = {n: np.array(m, float) for n, m in zip(prof["bones"], prof["rest_local"])}
    vc = np.bincount(vbone, minlength=len(names))
    num = den = 0.0
    for b in range(len(names)):
        if not vc[b] or names[b] not in pr:
            continue
        pa = parent[b]
        L = WA[b] if pa < 0 else np.linalg.inv(WA[pa]) @ WA[b]
        Ra, Rb = orthonormal(L), orthonormal(pr[names[b]])
        c = (np.trace(Ra.T @ Rb) - 1.0) / 2.0
        num += vc[b] * float(np.degrees(np.arccos(np.clip(c, -1.0, 1.0))))
        den += vc[b]
    return num / den if den else None


def select_profile(orig_dir, lib_dir=None):
    """Score every library profile against the model; return the best one's path.
    Rank: status!=rejected, vertex COVERAGE desc, then rest-pose similarity asc."""
    lib_dir = lib_dir or PROFILE_LIB
    V, vbone, W3, parent, names = load_apose(orig_dir)
    WA = np.array([to4(W3[i]) for i in range(len(names))])
    vc = np.bincount(vbone, minlength=len(names)); tot = int(vc.sum())
    rows = []
    for f in sorted(os.listdir(lib_dir)):
        if not f.endswith(".json"):
            continue
        p = os.path.join(lib_dir, f)
        try:
            prof = json.load(open(p))
        except Exception as e:
            print(f"[select] {f}: unreadable ({e}) -- skipped"); continue
        if prof.get("format") != 3:
            print(f"[select] {f}: format {prof.get('format', 1)} retired -- relearn from its "
                  f"source PSK; skipped")
            continue
        status = prof.get("status", "active")
        have = set(prof["bones"])
        cov = sum(int(vc[b]) for b in range(len(names)) if names[b] in have) / tot
        fit = rest_fit_deg(WA, parent, names, vbone, prof)
        rows.append((f, p, status, cov, fit))
    if not rows:
        sys.exit(f"[select] no usable format-3 profiles in {lib_dir}")
    rows.sort(key=lambda r: (r[2] == "rejected", -round(r[3], 4),
                             r[4] if r[4] is not None else 1e9))
    print(f"[select] profile library scoring ({tot} skinned verts):")
    for f, p, status, cov, fit in rows:
        fs = f"{fit:7.2f} deg" if fit is not None else "    no-rest"
        print(f"[select]   {f:44} {status:8} cov={100 * cov:5.1f}%  restfit={fs}")
    best = next((r for r in rows if r[2] != "rejected"), None)
    if best is None:
        sys.exit("[select] every profile is status=rejected -- learn a new one")
    print(f"[select] -> {best[0]}")
    return best[1]


# ---------- apply ----------
def _apply(V, vbone, WA, parent, prof, names):
    nb = len(parent)
    # Match pose deltas by bone NAME -- a new model's skeleton can differ from the
    # profile's. Bones absent from the profile get an identity local pose (keep their
    # rest orientation); they still inherit their parent's repose through the
    # hierarchy -- coverage_report() makes that share visible instead of silent.
    # Root stores a WORLD rotation (see learn).
    fmt = prof.get("format", 1)
    if fmt != 3:
        sys.exit(f"[repose] profile format {fmt} is retired -- relearn it from its source "
                 f"PSK (repose.py learn) or take the format-3 copy from profiles/")
    pmap = {n: np.array(m, float) for n, m in zip(prof["bones"], prof["pose_local"])}
    I4 = np.eye(4)
    pose = [pmap.get(names[b], I4) for b in range(nb)]
    order = sorted(range(nb), key=lambda b: (0 if parent[b] < 0 else 1, b))
    WTu = [None] * nb
    for b in order:
        pa = parent[b]
        if pa < 0:
            WTu[b] = pose[b]                                  # root world rot at origin
        else:
            RestL = np.linalg.inv(WA[pa]) @ WA[b]
            LT = RestL @ pose[b]                              # keep new bone offset, apply std delta
            WTu[b] = WTu[pa] @ LT
    # rigid per-bone repose into unit T-space
    Vu = np.empty_like(V)
    invWA = [np.linalg.inv(WA[b]) for b in range(nb)]
    for i in range(len(V)):
        b = int(vbone[i])
        vh = np.array([V[i, 0], V[i, 1], V[i, 2], 1.0])
        Vu[i] = (WTu[b] @ (invWA[b] @ vh))[:3]
    # scale to target height, then place (feet on ground, centered) per profile
    up = prof["up_axis"]
    h = Vu[:, up].max() - Vu[:, up].min()
    s = prof["target_height"] / h
    Vs = Vu * s
    out = np.empty_like(Vs)
    for i in range(3):
        if i == up:
            out[:, i] = Vs[:, i] - Vs[:, i].min() + prof["foot"]
        else:
            out[:, i] = Vs[:, i] - (Vs[:, i].max() + Vs[:, i].min()) / 2 + prof["center"][i]
    return out


def apply(orig_dir, prof_path, out_obj, validate=None):
    V, vbone, W3, parent, names = load_apose(orig_dir)
    prof = json.load(open(prof_path))
    print(f"[apply] profile: {os.path.basename(prof_path)} "
          f"(source {prof.get('source', '?')}, learned {prof.get('learned', '?')})")
    coverage_report(vbone, names, prof)
    WA = np.array([to4(W3[i]) for i in range(len(names))])
    Vt = _apply(V, vbone, WA, parent, prof, names)

    # write posed OBJ: reposed verts, original vt/f copied verbatim (topology unchanged)
    with open(os.path.join(orig_dir, "model.obj")) as f:
        lines = f.readlines()
    vi = 0
    with open(out_obj, "w") as o:
        o.write("# repose.py -> VOTV T-pose\n")
        for ln in lines:
            if ln.startswith("v "):
                o.write(f"v {Vt[vi,0]:.5f} {Vt[vi,1]:.5f} {Vt[vi,2]:.5f}\n"); vi += 1
            elif ln.startswith(("vt ", "f ", "usemtl", "g ", "o ")):
                o.write(ln)
    print(f"[apply] reposed {vi} verts -> {out_obj}  bbox={ (Vt.max(0)-Vt.min(0)).round(1) }")

    if validate:
        P, pb = psk_points(validate)
        if len(P) == len(Vt):
            err = np.linalg.norm(Vt - P, axis=1)
            print(f"[apply] validate vs {os.path.basename(validate)}: "
                  f"max={err.max():.5f} mean={err.mean():.5f}")
        else:
            print(f"[apply] validate: point count {len(Vt)} != {len(P)} (different model) -- skipped")


def main():
    a = sys.argv[1:]
    if len(a) >= 4 and a[0] == "learn":
        learn(a[1], a[2], a[3])
    elif len(a) >= 4 and a[0] == "apply":
        val = a[a.index("--validate") + 1] if "--validate" in a else None
        prof = select_profile(a[1]) if a[2] == "auto" else a[2]
        apply(a[1], prof, a[3], val)
    elif len(a) >= 2 and a[0] == "select":
        select_profile(a[1])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
