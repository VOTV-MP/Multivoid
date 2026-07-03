#!/usr/bin/env python3
"""mdl_extract -- GoldSrc (Half-Life 1) .mdl -> mesh + per-vertex bone + textures.

Pulls geometry (bind pose), UVs, the per-vertex bone assignment, and the embedded
8-bit textures out of a GoldSrc studiomdl v10 file, in PURE Python. GoldSrc uses
RIGID 1-bone-per-vertex skinning with clean "Bip01" biped bones, so the per-vertex
bone (from vertinfo) lets tools/skin_to_rig deterministically remap onto the VOTV
6-bone Kel rig (HL region -> Kel region), no geometric guessing.

We DISCARD the HL skeleton's animations/sequences (VOTV drives the puppet via its
own AnimBP). We keep the bone hierarchy + names only to build the remap.

Output (under an out dir): model.obj (v + vt + per-texture face groups),
model.bones.json (bone names/parents + per-position-vertex bone index + vert
offsets), tex/<name>.png (decoded).

Dev/RE tool only (RULE 3). Input model is the user's; output is derived -> keep it
gitignored (research/...). Usage:
  python tools/mdl_extract.py <model.mdl> <outDir>
"""
import json
import os
import struct
import sys
import zlib

import numpy as np

# ---- studiohdr_t (v10) field offsets ----
H = dict(numbones=140, boneindex=144, numtextures=180, textureindex=184,
         numskinref=192, numskinfamilies=196, skinindex=200,
         numbodyparts=204, bodypartindex=208)


def _s(b, o, n):
    return b[o:o + n].split(b"\0")[0].decode("latin1", "replace")


def angle_quat(a):
    # HL AngleQuaternion: a=[roll(x), pitch(y), yaw(z)] radians
    sr, cr = np.sin(a[0] * .5), np.cos(a[0] * .5)
    sp, cp = np.sin(a[1] * .5), np.cos(a[1] * .5)
    sy, cy = np.sin(a[2] * .5), np.cos(a[2] * .5)
    return np.array([sr * cp * cy - cr * sp * sy,
                     cr * sp * cy + sr * cp * sy,
                     cr * cp * sy - sr * sp * cy,
                     cr * cp * cy + sr * sp * sy])  # x,y,z,w


def quat_mat3(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * w * z, 2 * x * z + 2 * w * y],
        [2 * x * y + 2 * w * z, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * w * x],
        [2 * x * z - 2 * w * y, 2 * y * z + 2 * w * x, 1 - 2 * x * x - 2 * y * y]])


def compose(A, B):  # 3x4 (R|t) compose: A then B under A -> world = A * B
    R = A[:, :3] @ B[:, :3]
    t = A[:, :3] @ B[:, 3] + A[:, 3]
    return np.hstack([R, t[:, None]])


def write_png(path, w, h, rgb):
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def _interior_groups(allV, obj_faces):
    """Auto-detect fully-ENCLOSED per-texture face groups: geometry the source
    model keeps hidden inside its outer shell. GoldSrc ports are full of such
    leftovers (the LD `dm_base` body under the HD scientist coat, the inner
    mouth plane, sci_v1sc's Gordon head INSIDE the scientist head); GoldSrc's
    own skeleton kept them covered, but the anthro-rig repose deforms
    differently and they poke through the outer skin (user 2026-07-03: "inner
    вылазит изнутри наружу"). A group is interior when nearly every sampled
    face point is blocked by OTHER groups' triangles in nearly every direction
    ("cannot see the sky"). Visible-but-layered parts survive: the shirt/belt
    under the open coat and the chrome glasses see open sky through their
    exposed side. Returns {safe_tex: enclosed_fraction}."""
    groups = {}
    for safe, tri in obj_faces:
        groups.setdefault(safe, []).append([tri[0][0], tri[1][0], tri[2][0]])
    tri_pts = {s: allV[np.array(idx)] for s, idx in groups.items()}  # (N,3,3)

    # 26 directions: every nonzero {-1,0,1}^3 combination, normalized.
    dirs = np.array([(x, y, z) for x in (-1, 0, 1) for y in (-1, 0, 1)
                     for z in (-1, 0, 1) if (x, y, z) != (0, 0, 0)], float)
    dirs /= np.linalg.norm(dirs, axis=1)[:, None]

    def blocked(origins, d, tris):
        """Moller-Trumbore, vectorized over samples x triangles for one ray
        direction: does ANY triangle block each origin along d? -> (S,) bool"""
        v0, e1, e2 = tris[:, 0], tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]
        p = np.cross(d, e2)                      # (M,3)
        det = np.einsum("mk,mk->m", e1, p)       # (M,)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        t0 = origins[:, None, :] - v0[None, :, :]            # (S,M,3)
        u = np.einsum("smk,mk->sm", t0, p) * inv[None, :]
        q = np.cross(t0, e1[None, :, :])                     # (S,M,3)
        v = np.einsum("smk,k->sm", q, d) * inv[None, :]
        t = np.einsum("smk,mk->sm", q, e2) * inv[None, :]
        hit = ok[None, :] & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-3)
        return hit.any(axis=1)

    frac = {}
    for safe, tris in tri_pts.items():
        others = [t for s, t in tri_pts.items() if s != safe]
        if not others:
            frac[safe] = 0.0
            continue
        occl = np.concatenate(others)
        cent = tris.mean(axis=1)
        if len(cent) > 96:                        # bounded sampling, even stride
            cent = cent[:: max(1, len(cent) // 96)][:96]
        blocked_dirs = np.zeros(len(cent))
        for d in dirs:
            blocked_dirs += blocked(cent, d, occl)
        enclosed = blocked_dirs >= len(dirs) - 1  # allow one leak direction
        frac[safe] = float(enclosed.mean())
    return frac


def extract(path, out, strip_materials=(), keep_materials=()):
    os.makedirs(os.path.join(out, "tex"), exist_ok=True)
    b = open(path, "rb").read()
    gi = lambda o: struct.unpack_from("<i", b, o)[0]
    gh = lambda o: struct.unpack_from("<h", b, o)[0]
    gf = lambda o: struct.unpack_from("<f", b, o)[0]

    # ---- bones + bind-pose world matrices ----
    nb, bi = gi(H["numbones"]), gi(H["boneindex"])
    bones, world = [], [None] * nb
    for i in range(nb):
        o = bi + i * 112
        name, parent = _s(b, o, 32), gi(o + 32)
        val = [gf(o + 64 + k * 4) for k in range(6)]      # pos[3], rot[3] radians
        R = quat_mat3(angle_quat(val[3:6]))
        local = np.hstack([R, np.array(val[0:3])[:, None]])
        world[i] = local if parent < 0 else compose(world[parent], local)
        bones.append({"name": name, "parent": parent})

    # ---- skin families (mesh.skinref -> texture index, family 0) ----
    nsr, nsf, si = gi(H["numskinref"]), gi(H["numskinfamilies"]), gi(H["skinindex"])
    skin = [gh(si + k * 2) for k in range(max(nsr * nsf, 0))]

    # ---- textures (decoded now, WRITTEN after the interior-group filter so a
    # dropped group's texture never reaches the atlas) ----
    nt, ti = gi(H["numtextures"]), gi(H["textureindex"])
    tex_names, tex_rgb = [], {}
    for i in range(nt):
        o = ti + i * 80
        tn, w, h, idx = _s(o if False else b, o, 64), gi(o + 68), gi(o + 72), gi(o + 76)
        pix = b[idx: idx + w * h]
        pal = b[idx + w * h: idx + w * h + 768]
        rgb = bytearray(w * h * 3)
        for p in range(w * h):
            c = pix[p]
            rgb[p * 3:p * 3 + 3] = pal[c * 3:c * 3 + 3]
        safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in tn)
        tex_rgb[safe] = (w, h, bytes(rgb))
        tex_names.append((tn, safe, w, h))

    # ---- bodyparts -> model[0] -> verts + meshes(tricmds) ----
    nbp, bpi = gi(H["numbodyparts"]), gi(H["bodypartindex"])
    V, VB, OBJv, OBJvt, groups = [], [], [], [], {}   # groups: safe_tex -> list of (a,b,c) pos idx, and parallel vt idx
    voff = 0
    obj_faces = []   # (safe_tex, (p0,uv0),(p1,uv1),(p2,uv2))
    for ip in range(nbp):
        o = bpi + ip * 76
        modelidx = gi(o + 72)
        mo = modelidx                                   # model[0]
        nummesh, meshindex = gi(mo + 72), gi(mo + 76)
        numverts, vertinfoindex, vertindex = gi(mo + 80), gi(mo + 84), gi(mo + 88)
        # verts (bone-local) -> world (bind pose)
        vb = [b[vertinfoindex + k] for k in range(numverts)]
        vw = np.empty((numverts, 3))
        for k in range(numverts):
            v = np.array(struct.unpack_from("<3f", b, vertindex + k * 12))
            M = world[vb[k]]
            vw[k] = M[:, :3] @ v + M[:, 3]
        V.append(vw); VB.extend(vb)
        # meshes
        for m in range(nummesh):
            meo = meshindex + m * 20
            numtris, triindex, skinref = gi(meo), gi(meo + 4), gi(meo + 8)
            texi = skin[skinref] if skin else skinref
            tw, th = (tex_names[texi][2], tex_names[texi][3]) if texi < len(tex_names) else (1, 1)
            safe = tex_names[texi][1] if texi < len(tex_names) else f"tex{texi}"
            p = triindex
            while True:
                n = gh(p); p += 2
                if n == 0:
                    break
                fan = n < 0
                n = abs(n)
                strip = []
                for _ in range(n):
                    vi, ni, s, t = gh(p), gh(p + 2), gh(p + 4), gh(p + 6); p += 8
                    uv = (s / tw, 1.0 - t / th)
                    OBJvt.append(uv)
                    strip.append((vi + voff, len(OBJvt)))     # (global pos idx, vt idx(1-based later))
                for j in range(2, len(strip)):
                    if fan:
                        tri = (strip[0], strip[j - 1], strip[j])
                    else:
                        tri = (strip[j - 2], strip[j - 1], strip[j]) if j % 2 == 0 else (strip[j - 1], strip[j - 2], strip[j])
                    obj_faces.append((safe, tri))
        voff += numverts

    allV = np.concatenate(V)

    # ---- interior-shell filter: drop face groups the model keeps fully hidden
    # inside its outer skin (see _interior_groups). Manual overrides win:
    # strip_materials forces a drop, keep_materials exempts from the auto-drop.
    frac = _interior_groups(allV, obj_faces)
    lower = lambda pats: tuple(p.lower() for p in pats if p)
    strip_pats, keep_pats = lower(strip_materials), lower(keep_materials)
    dropped = set()
    for safe in sorted(frac):
        s = safe.lower()
        forced = any(p in s for p in strip_pats)
        kept = any(p in s for p in keep_pats)
        auto = frac[safe] >= 0.90
        drop = forced or (auto and not kept)
        ntri = sum(1 for g, _ in obj_faces if g == safe)
        tag = ("DROPPED (forced)" if forced else
               "DROPPED (interior shell)" if drop else
               "kept (keep-material)" if auto and kept else "kept")
        print(f"[mdl_extract] interior-scan: {safe:28s} tris={ntri:4d} "
              f"enclosed={frac[safe]*100:3.0f}% -> {tag}")
        if drop:
            dropped.add(safe)
    if dropped:
        obj_faces = [(safe, tri) for safe, tri in obj_faces if safe not in dropped]

    # write only the textures still referenced by faces (a dropped group's
    # texture must not waste atlas space).
    referenced = {safe for safe, _ in obj_faces}
    for safe, (w, h, rgb) in tex_rgb.items():
        if safe in referenced:
            write_png(os.path.join(out, "tex", safe + ".png"), w, h, rgb)

    # write OBJ
    with open(os.path.join(out, "model.obj"), "w") as f:
        f.write(f"# mdl_extract from {os.path.basename(path)}\n")
        for v in allV:
            f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
        for uv in OBJvt:
            f.write(f"vt {uv[0]:.5f} {uv[1]:.5f}\n")
        cur = None
        for safe, tri in obj_faces:
            if safe != cur:
                f.write(f"usemtl {safe}\n"); cur = safe
            f.write("f " + " ".join(f"{pi+1}/{vt}" for pi, vt in tri) + "\n")
    json.dump({"bones": bones, "vert_bone": VB, "num_verts": int(allV.shape[0]),
               "world": [world[i].flatten().tolist() for i in range(nb)]},
              open(os.path.join(out, "model.bones.json"), "w"), indent=0)

    bb = allV.max(0) - allV.min(0)
    print(f"[mdl_extract] verts={allV.shape[0]} tris={len(obj_faces)} textures={nt}")
    print(f"[mdl_extract] bbox (HL units) = {bb.round(1)}  (humanoid ~one axis 60-75)")
    print(f"[mdl_extract] wrote model.obj + model.bones.json + tex/*.png -> {out}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    strip, keep = [], []
    for o in opts:
        if o.startswith("--strip-material="):
            strip += o.split("=", 1)[1].split(",")
        elif o.startswith("--keep-material="):
            keep += o.split("=", 1)[1].split(",")
        else:
            print(f"unknown option {o}"); return 2
    if len(args) < 2:
        print("usage: mdl_extract <model.mdl> <outDir> "
              "[--strip-material=a,b] [--keep-material=c,d]"); return 2
    extract(args[0], args[1], strip_materials=strip, keep_materials=keep)


if __name__ == "__main__":
    main()
