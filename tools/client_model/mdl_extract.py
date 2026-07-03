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


def extract(path, out):
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

    # ---- textures ----
    nt, ti = gi(H["numtextures"]), gi(H["textureindex"])
    tex_names = []
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
        write_png(os.path.join(out, "tex", safe + ".png"), w, h, bytes(rgb))
        tex_names.append((tn, safe, w, h))

    # ---- bodyparts -> model[0] -> verts + norms + meshes(tricmds) ----
    nbp, bpi = gi(H["numbodyparts"]), gi(H["bodypartindex"])
    V, VB, N, NB, OBJv, OBJvt, groups = [], [], [], [], [], [], {}
    voff, noff = 0, 0
    obj_faces = []   # (safe_tex, (p0,uv0,n0),(p1,uv1,n1),(p2,uv2,n2))
    for ip in range(nbp):
        o = bpi + ip * 76
        modelidx = gi(o + 72)
        mo = modelidx                                   # model[0]
        nummesh, meshindex = gi(mo + 72), gi(mo + 76)
        numverts, vertinfoindex, vertindex = gi(mo + 80), gi(mo + 84), gi(mo + 88)
        numnorms, norminfoindex, normindex = gi(mo + 92), gi(mo + 96), gi(mo + 100)
        # verts (bone-local) -> world (bind pose)
        vb = [b[vertinfoindex + k] for k in range(numverts)]
        vw = np.empty((numverts, 3))
        for k in range(numverts):
            v = np.array(struct.unpack_from("<3f", b, vertindex + k * 12))
            M = world[vb[k]]
            vw[k] = M[:, :3] @ v + M[:, 3]
        V.append(vw); VB.extend(vb)
        # AUTHORED shading normals (studiomdl computed them from the source's
        # smoothing groups; the suit mesh has ~18% inconsistent winding, so
        # recomputing from faces is NOT equivalent -- 2026-07-03 dark-suit root).
        # Bone-local like verts; rotation-only to bind-pose world.
        nb_ = [b[norminfoindex + k] for k in range(numnorms)]
        for k in range(numnorms):
            nv = np.array(struct.unpack_from("<3f", b, normindex + k * 12))
            nw = world[nb_[k]][:, :3] @ nv
            l = np.linalg.norm(nw)
            N.append(nw / l if l > 1e-8 else np.array([0.0, 0.0, 1.0]))
        NB.extend(nb_)
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
                    strip.append((vi + voff, len(OBJvt), ni + noff))  # (global pos idx, vt idx(1-based later), global norm idx)
                for j in range(2, len(strip)):
                    if fan:
                        tri = (strip[0], strip[j - 1], strip[j])
                    else:
                        tri = (strip[j - 2], strip[j - 1], strip[j]) if j % 2 == 0 else (strip[j - 1], strip[j - 2], strip[j])
                    obj_faces.append((safe, tri))
        voff += numverts; noff += numnorms

    allV = np.concatenate(V)
    allN = np.array(N)
    # write OBJ (vn = authored normals, bind-pose world space; f = p/t/n)
    with open(os.path.join(out, "model.obj"), "w") as f:
        f.write(f"# mdl_extract from {os.path.basename(path)}\n")
        for v in allV:
            f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
        for uv in OBJvt:
            f.write(f"vt {uv[0]:.5f} {uv[1]:.5f}\n")
        for n in allN:
            f.write(f"vn {n[0]:.5f} {n[1]:.5f} {n[2]:.5f}\n")
        cur = None
        for safe, tri in obj_faces:
            if safe != cur:
                f.write(f"usemtl {safe}\n"); cur = safe
            f.write("f " + " ".join(f"{pi+1}/{vt}/{ni+1}" for pi, vt, ni in tri) + "\n")
    json.dump({"bones": bones, "vert_bone": VB, "num_verts": int(allV.shape[0]),
               "norm_bone": NB, "num_norms": int(allN.shape[0]),
               "world": [world[i].flatten().tolist() for i in range(nb)]},
              open(os.path.join(out, "model.bones.json"), "w"), indent=0)

    bb = allV.max(0) - allV.min(0)
    print(f"[mdl_extract] verts={allV.shape[0]} norms={allN.shape[0]} tris={len(obj_faces)} textures={nt}")
    print(f"[mdl_extract] bbox (HL units) = {bb.round(1)}  (humanoid ~one axis 60-75)")
    print(f"[mdl_extract] wrote model.obj + model.bones.json + tex/*.png -> {out}")


def main():
    if len(sys.argv) < 3:
        print("usage: mdl_extract <model.mdl> <outDir>"); return 2
    extract(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
